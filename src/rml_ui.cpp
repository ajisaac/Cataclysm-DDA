#include "rml_ui.h"

#if !defined(CATA_RMLUI)

// Stubs so call sites need no guards of their own.
namespace rml_ui
{
void init( const SDL_Renderer_Ptr &, int, int ) {}
void shutdown() {}
void resize( int, int ) {}
void render() {}
void show_main_menu( const std::vector<std::string> &, int ) {}
void hide_main_menu() {}
bool active()
{
    return false;
}
} // namespace rml_ui

#else

#include <memory>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

#include "debug.h"
#include "path_info.h"
#include "rml_render_sdl.h"

namespace
{

class cata_system_interface : public Rml::SystemInterface
{
    public:
        double GetElapsedTime() override {
            return SDL_GetTicks() / 1000.0;
        }

        bool LogMessage( Rml::Log::Type type, const Rml::String &message ) override {
            switch( type ) {
                case Rml::Log::LT_ERROR:
                case Rml::Log::LT_ASSERT:
                    DebugLog( D_ERROR, DC_ALL ) << "RmlUi: " << message;
                    break;
                case Rml::Log::LT_WARNING:
                    DebugLog( D_WARNING, DC_ALL ) << "RmlUi: " << message;
                    break;
                default:
                    DebugLog( D_INFO, DC_ALL ) << "RmlUi: " << message;
                    break;
            }
            // true == keep going; false would break into the debugger.
            return true;
        }
};

std::unique_ptr<cata_system_interface> system_interface;
std::unique_ptr<rml_sdl_render_interface> render_interface;
Rml::Context *context = nullptr;
SDL_Renderer *saved_renderer = nullptr;
bool initialised = false;

Rml::ElementDocument *menu_document = nullptr;
Rml::Element *menu_items_container = nullptr;
std::vector<std::string> cached_items;
int cached_selection = -1;

// Deliberately offset to the right so the original catacurses menu stays
// visible underneath for side-by-side comparison.
const char *menu_document_rml = R"RML(
<rml>
<head>
<style>
body
{
    width: 100%;
    height: 100%;
    font-family: cata-sans;
    color: #e8e4d8;
}
#menu
{
    position: absolute;
    top: 48dp;
    left: 48dp;
    width: 300dp;
    padding: 20dp;
    background-color: #141821f2;
    border: 1dp #39465c;
    border-radius: 12dp;
}
#menu .heading
{
    display: block;
    font-size: 13dp;
    color: #6d7b92;
    margin-bottom: 14dp;
}
#menu .item
{
    display: block;
    font-size: 19dp;
    padding: 7dp 12dp;
    margin-bottom: 2dp;
    border-radius: 6dp;
    color: #b7b2a4;
}
#menu .item.selected
{
    background-color: #2b3a52;
    color: #ffffff;
}
#menu .item .key
{
    color: #7fa8e0;
}
#menu .item.selected .key
{
    color: #cfe2ff;
}
</style>
</head>
<body>
<div id="menu">
    <div class="heading">RmlUi &mdash; main menu replica</div>
    <div id="items"></div>
</div>
</body>
</rml>
)RML";

/// Turn the game's "<M|m>OTD" hotkey markup into RML, accenting the hotkey
/// letter. Everything outside the brackets is emitted as-is; the first
/// alternative inside them is what gets shown.
std::string markup_to_rml( const std::string &label )
{
    std::string out;
    out.reserve( label.size() + 32 );
    for( size_t i = 0; i < label.size(); ++i ) {
        if( label[i] != '<' ) {
            out += label[i];
            continue;
        }
        const size_t close = label.find( '>', i );
        if( close == std::string::npos ) {
            out += label[i];
            continue;
        }
        std::string alternatives = label.substr( i + 1, close - i - 1 );
        const size_t bar = alternatives.find( '|' );
        if( bar != std::string::npos ) {
            alternatives = alternatives.substr( 0, bar );
        }
        out += "<span class=\"key\">";
        out += alternatives;
        out += "</span>";
        i = close;
    }
    return out;
}

void rebuild_menu_items( const std::vector<std::string> &items )
{
    if( !menu_items_container || !menu_document ) {
        return;
    }
    while( Rml::Element *child = menu_items_container->GetFirstChild() ) {
        menu_items_container->RemoveChild( child );
    }
    for( const std::string &label : items ) {
        Rml::ElementPtr element = menu_document->CreateElement( "div" );
        element->SetClass( "item", true );
        element->SetInnerRML( markup_to_rml( label ) );
        menu_items_container->AppendChild( std::move( element ) );
    }
    cached_items = items;
    cached_selection = -1;
}

void apply_selection( int selected )
{
    if( !menu_items_container || selected == cached_selection ) {
        return;
    }
    const int count = static_cast<int>( menu_items_container->GetNumChildren() );
    for( int i = 0; i < count; ++i ) {
        if( Rml::Element *child = menu_items_container->GetChild( i ) ) {
            child->SetClass( "selected", i == selected );
        }
    }
    cached_selection = selected;
}

} // namespace

namespace rml_ui
{

void init( const SDL_Renderer_Ptr &renderer, int width, int height )
{
    if( initialised || !renderer ) {
        return;
    }

    saved_renderer = renderer.get();
    system_interface = std::make_unique<cata_system_interface>();
    render_interface = std::make_unique<rml_sdl_render_interface>( renderer.get() );
    Rml::SetSystemInterface( system_interface.get() );
    Rml::SetRenderInterface( render_interface.get() );

    if( !Rml::Initialise() ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: Rml::Initialise() failed";
        system_interface.reset();
        render_interface.reset();
        return;
    }

    const std::string font = PATH_INFO::fontdir() + "Roboto-Medium.ttf";
    if( !Rml::LoadFontFace( font, "cata-sans", Rml::Style::FontStyle::Normal ) ) {
        // Not fatal: boxes still draw, text just will not. Worth knowing which.
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: failed to load font face " << font;
    }

    context = Rml::CreateContext( "cata", Rml::Vector2i( width, height ) );
    if( !context ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: CreateContext failed";
        Rml::Shutdown();
        system_interface.reset();
        render_interface.reset();
        return;
    }

    menu_document = context->LoadDocumentFromMemory( menu_document_rml, "[cata-main-menu]" );
    if( menu_document ) {
        menu_items_container = menu_document->GetElementById( "items" );
    } else {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: LoadDocumentFromMemory failed";
    }

    initialised = true;
    DebugLog( D_INFO, DC_ALL ) << "RmlUi: initialised at " << width << "x" << height;
}

void shutdown()
{
    if( !initialised ) {
        return;
    }
    initialised = false;
    menu_items_container = nullptr;
    menu_document = nullptr;
    cached_items.clear();
    cached_selection = -1;
    context = nullptr;
    saved_renderer = nullptr;
    Rml::Shutdown();
    render_interface.reset();
    system_interface.reset();
}

void resize( int width, int height )
{
    if( context ) {
        context->SetDimensions( Rml::Vector2i( width, height ) );
    }
}

void show_main_menu( const std::vector<std::string> &items, int selected )
{
    if( !initialised || !menu_document ) {
        return;
    }
    if( items != cached_items ) {
        rebuild_menu_items( items );
    }
    apply_selection( selected );
    if( !menu_document->IsVisible() ) {
        menu_document->Show();
    }
}

void hide_main_menu()
{
    if( menu_document && menu_document->IsVisible() ) {
        menu_document->Hide();
    }
}

void render()
{
    if( !initialised || !context ) {
        return;
    }

    // draw_window() sets logical presentation to the display_buffer size when
    // scaling_factor > 1 (sdltiles.cpp) and never resets it, so by the time we
    // draw, renderer coordinates are buffer-space and get scaled up to the
    // window. Our context is sized in window pixels, so drop to 1:1 for the
    // duration of the draw -- otherwise the overlay lands off-screen.
    int saved_logical_w = 0;
    int saved_logical_h = 0;
    SDL_RendererLogicalPresentation saved_presentation = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation( saved_renderer, &saved_logical_w, &saved_logical_h,
                                      &saved_presentation );
    SDL_SetRenderLogicalPresentation( saved_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED );

    // RmlUi sets its own clip rect and blend mode while drawing. The rest of
    // the presentation path assumes it owns that state, so save and restore it
    // rather than leaving RmlUi's behind. Also clear any clip left over from
    // the game's own drawing, which would otherwise scissor the overlay away.
    SDL_Rect saved_clip{};
    const bool had_clip = SDL_RenderClipEnabled( saved_renderer );
    if( had_clip ) {
        SDL_GetRenderClipRect( saved_renderer, &saved_clip );
    }
    SDL_SetRenderClipRect( saved_renderer, nullptr );
    SDL_BlendMode saved_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode( saved_renderer, &saved_blend );

    context->Update();

    // One-shot: report where RmlUi actually laid the panel out, so a panel that
    // is invisible because it is zero-sized or off-screen is distinguishable
    // from one that is not being drawn at all.
    static bool logged_layout = false;
    if( !logged_layout && menu_document ) {
        logged_layout = true;
        DebugLog( D_INFO, DC_ALL ) << "RmlUi: menu document visible="
                                   << menu_document->IsVisible();
        const Rml::Vector2i dims = context->GetDimensions();
        if( Rml::Element *panel = menu_document->GetElementById( "menu" ) ) {
            DebugLog( D_INFO, DC_ALL ) << "RmlUi: context " << dims.x << "x" << dims.y
                                       << ", #menu at (" << panel->GetAbsoluteLeft() << ","
                                       << panel->GetAbsoluteTop() << ") size "
                                       << panel->GetClientWidth() << "x"
                                       << panel->GetClientHeight()
                                       << ", items=" << ( menu_items_container ?
                                               menu_items_container->GetNumChildren() : -1 );
        } else {
            DebugLog( D_ERROR, DC_ALL ) << "RmlUi: #menu element not found";
        }
    }

    context->Render();

    SDL_SetRenderClipRect( saved_renderer, had_clip ? &saved_clip : nullptr );
    SDL_SetRenderDrawBlendMode( saved_renderer, saved_blend );
    SDL_SetRenderLogicalPresentation( saved_renderer, saved_logical_w, saved_logical_h,
                                      saved_presentation );
}

bool active()
{
    return initialised;
}

} // namespace rml_ui

#endif // CATA_RMLUI

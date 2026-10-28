#include "rml_ui.h"

#include "ui_manager.h"

#if !defined(CATA_RMLUI)

// Stubs so call sites need no guards of their own.
namespace rml_ui
{
void init( const SDL_Renderer_Ptr &, int, int ) {}
void shutdown() {}
void resize( int, int ) {}
void render() {}
void begin_redraw_pass() {}
void end_redraw_pass() {}
bool active()
{
    return false;
}

document::document( const std::string &, const std::string & ) {}
document::~document() = default;
Rml::ElementDocument *document::get() const
{
    return doc;
}

main_menu_replica::main_menu_replica() : document( std::string(), std::string() ) {}
void main_menu_replica::update( const std::vector<std::string> &, int ) {}
} // namespace rml_ui

#else

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

#include "debug.h"
#include "path_info.h"
#include "rml_markup.h"
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

// Every document with a loaded Rml::ElementDocument.
std::vector<rml_ui::document *> live_documents;
// Documents whose adaptor has been redrawn so far this pass, bottom of the
// ui_manager stack first.
std::vector<rml_ui::document *> pass_documents;
// The stacking order last applied to the context, so documents are only
// reordered when the stack actually changes.
std::vector<rml_ui::document *> applied_order;

void forget_document( std::vector<rml_ui::document *> &list, const rml_ui::document *doc )
{
    list.erase( std::remove( list.begin(), list.end(), doc ), list.end() );
}

// The fg-/bg- classes rml_ui::markup_to_rml() emits. Built on first use rather
// than in init(), which runs from WinCreate() before the palette is loaded.
Rml::SharedPtr<Rml::StyleSheetContainer> palette_sheet;

const Rml::SharedPtr<Rml::StyleSheetContainer> &get_palette_sheet()
{
    if( !palette_sheet ) {
        palette_sheet = Rml::Factory::InstanceStyleSheetString( rml_ui::palette_rcss() );
    }
    return palette_sheet;
}

const char *main_menu_rml = R"RML(
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

/// The main menu's "<M|m>OTD" labels, split the way shortcut_text() splits
/// them, with the hotkey in a span the replica's stylesheet can accent.
std::string hotkey_label_to_rml( const std::string &label )
{
    const size_t pos = label.find_first_of( '<' );
    const size_t pos_end = label.find_first_of( '>' );
    if( pos_end == std::string::npos || pos >= pos_end ) {
        return rml_ui::markup_to_rml( label );
    }
    const size_t sep = std::min( label.find_first_of( '|', pos ), pos_end );
    return rml_ui::markup_to_rml( label.substr( 0, pos ) ) +
           "<span class=\"key\">" +
           rml_ui::markup_to_rml( label.substr( pos + 1, sep - pos - 1 ) ) +
           "</span>" +
           rml_ui::markup_to_rml( label.substr( pos_end + 1 ) );
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

    initialised = true;
    DebugLog( D_INFO, DC_ALL ) << "RmlUi: initialised at " << width << "x" << height;
}

void shutdown()
{
    if( !initialised ) {
        return;
    }
    initialised = false;
    // Rml::Shutdown() destroys every document along with the context, so leave
    // any document objects that outlive it holding nothing rather than a
    // dangling pointer.
    for( document *live : live_documents ) {
        live->doc = nullptr;
    }
    live_documents.clear();
    pass_documents.clear();
    applied_order.clear();
    palette_sheet.reset();
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

void begin_redraw_pass()
{
    pass_documents.clear();
}

void end_redraw_pass()
{
    if( !initialised ) {
        return;
    }
    // A document that was not redrawn this pass is below a disable_uis_below
    // adaptor, so it should not be seen.
    for( document *doc : live_documents ) {
        Rml::ElementDocument *element = doc->get();
        const bool wanted = std::find( pass_documents.begin(), pass_documents.end(),
                                       doc ) != pass_documents.end();
        if( !element || element->IsVisible() == wanted ) {
            continue;
        }
        if( wanted ) {
            // No focus: input still goes through input_context, and focusing
            // would also let RmlUi pull the document to the front on its own.
            element->Show( Rml::ModalFlag::None, Rml::FocusFlag::None );
        } else {
            element->Hide();
        }
    }
    // Pulling each to the front in turn, bottom first, leaves the context in
    // stack order.
    if( pass_documents != applied_order ) {
        for( document *doc : pass_documents ) {
            if( Rml::ElementDocument *element = doc->get() ) {
                element->PullToFront();
            }
        }
        applied_order = pass_documents;
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

document::document( const std::string &rml, const std::string &source_url )
{
    if( !initialised || !context ) {
        return;
    }
    doc = context->LoadDocumentFromMemory( rml, source_url );
    if( !doc ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: failed to load document " << source_url;
        return;
    }
    live_documents.push_back( this );

    // Every document gets the palette, so game text converted by
    // markup_to_rml() shows in its own colors without each document asking.
    if( const Rml::SharedPtr<Rml::StyleSheetContainer> &palette = get_palette_sheet() ) {
        const Rml::StyleSheetContainer *own = doc->GetStyleSheetContainer();
        doc->SetStyleSheetContainer( own ? own->CombineStyleSheetContainer( *palette ) : palette );
    }

    // Draws nothing into the display buffer, so it has no position: being
    // redrawn is just how this document learns it is still enabled, and where
    // it sits in the stack.
    adaptor = std::make_unique<ui_adaptor>();
    adaptor->is_rml = true;
    adaptor->on_redraw( [this]( ui_adaptor & ui ) {
        // No catacurses window here, so leave the cursor to the UIs that have one.
        ui.disable_cursor();
        pass_documents.push_back( this );
    } );
}

document::~document()
{
    // Pop the adaptor first so no redraw pass can see this document mid-teardown.
    adaptor.reset();
    forget_document( live_documents, this );
    forget_document( pass_documents, this );
    forget_document( applied_order, this );
    if( doc ) {
        // Detaches from the context immediately, so it is not drawn again.
        doc->Close();
    }
}

Rml::ElementDocument *document::get() const
{
    return doc;
}

main_menu_replica::main_menu_replica() : document( main_menu_rml, "[cata-main-menu]" ) {}

void main_menu_replica::update( const std::vector<std::string> &items, int selected )
{
    Rml::ElementDocument *menu = get();
    Rml::Element *container = menu ? menu->GetElementById( "items" ) : nullptr;
    if( !container ) {
        return;
    }

    if( items != cached_items ) {
        while( Rml::Element *child = container->GetFirstChild() ) {
            container->RemoveChild( child );
        }
        for( const std::string &label : items ) {
            Rml::ElementPtr element = menu->CreateElement( "div" );
            element->SetClass( "item", true );
            element->SetInnerRML( hotkey_label_to_rml( label ) );
            container->AppendChild( std::move( element ) );
        }
        cached_items = items;
    }

    // SetClass() is a no-op when the class is already in that state.
    const int count = static_cast<int>( container->GetNumChildren() );
    for( int i = 0; i < count; ++i ) {
        if( Rml::Element *child = container->GetChild( i ) ) {
            child->SetClass( "selected", i == selected );
        }
    }
}

} // namespace rml_ui

#endif // CATA_RMLUI

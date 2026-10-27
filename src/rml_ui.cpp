#include "rml_ui.h"

#if !defined(CATA_RMLUI)

// Stubs so call sites need no guards of their own.
namespace rml_ui
{
void init( const SDL_Renderer_Ptr &, int, int ) {}
void shutdown() {}
void resize( int, int ) {}
void render() {}
bool active()
{
    return false;
}
} // namespace rml_ui

#else

#include <memory>
#include <string>

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

// Inline rather than a data file: a spike should not also introduce a new
// asset search path. Moving this to data/rml/ is the next step, not this one.
const char *test_document = R"RML(
<rml>
<head>
<style>
body
{
    font-family: cata-sans;
    font-size: 18dp;
    color: #e8e4d8;
}
#probe
{
    position: absolute;
    top: 48dp;
    left: 48dp;
    width: 320dp;
    padding: 18dp;
    background-color: #1b1f2aee;
    border: 2dp #6f8ab0;
    border-radius: 10dp;
}
#probe h1
{
    font-size: 22dp;
    color: #9ec5ff;
    margin-bottom: 10dp;
}
#probe .row
{
    display: block;
    margin-bottom: 4dp;
    color: #b9b4a6;
}
</style>
</head>
<body>
<div id="probe">
    <h1>RmlUi is drawing</h1>
    <div class="row">Layout, CSS and font rasterisation all work.</div>
    <div class="row">Rounded border + alpha background prove styling.</div>
</div>
</body>
</rml>
)RML";

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

    if( Rml::ElementDocument *document =
            context->LoadDocumentFromMemory( test_document, "[cata-rmlui-spike]" ) ) {
        document->Show();
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

void render()
{
    if( !initialised || !context ) {
        return;
    }

    // RmlUi sets its own clip rect and blend mode while drawing. The rest of
    // the presentation path assumes it owns that state, so save and restore it
    // rather than leaving RmlUi's behind.
    SDL_Rect saved_clip{};
    const bool had_clip = SDL_RenderClipEnabled( saved_renderer );
    if( had_clip ) {
        SDL_GetRenderClipRect( saved_renderer, &saved_clip );
    }
    SDL_BlendMode saved_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode( saved_renderer, &saved_blend );

    context->Update();
    context->Render();

    SDL_SetRenderClipRect( saved_renderer, had_clip ? &saved_clip : nullptr );
    SDL_SetRenderDrawBlendMode( saved_renderer, saved_blend );
}

bool active()
{
    return initialised;
}

} // namespace rml_ui

#endif // CATA_RMLUI

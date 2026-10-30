#include "rml_ui.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

#include "cata_path.h"
#include "debug.h"
#include "filesystem.h"
#include "path_info.h"
#include "rml_markup.h"
#include "rml_render_sdl.h"
#include "sdltiles.h"
#include "ui_manager.h"

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

        // For copy, cut and paste in text fields.
        void SetClipboardText( const Rml::String &text ) override {
            SDL_SetClipboardText( text.c_str() );
        }

        void GetClipboardText( Rml::String &text ) override {
            if( char *clipboard = SDL_GetClipboardText() ) {
                text = clipboard;
                SDL_free( clipboard );
            }
        }
};

std::unique_ptr<cata_system_interface> system_interface;
std::unique_ptr<rml_sdl_render_interface> render_interface;
Rml::Context *context = nullptr;
SDL_Renderer *saved_renderer = nullptr;
bool initialised = false;
// Output pixels per window coordinate: what SDL mouse positions are scaled by,
// and the context's dp ratio.
float pixel_density = 1.f;

Rml::Vector2i output_size()
{
    int w = 0;
    int h = 0;
    if( !SDL_GetRenderOutputSize( saved_renderer, &w, &h ) ) {
        return Rml::Vector2i( 0, 0 );
    }
    return Rml::Vector2i( w, h );
}

// Checked every frame rather than hooked to the game's resize handling, since
// a DPI-only change, such as moving the window to another monitor, does not
// go through that path.
void sync_to_window()
{
    const Rml::Vector2i size = output_size();
    if( size.x > 0 && size.y > 0 && size != context->GetDimensions() ) {
        context->SetDimensions( size );
    }
    float density = SDL_GetWindowPixelDensity( SDL_GetRenderWindow( saved_renderer ) );
    if( density <= 0.f ) {
        density = 1.f;
    }
    if( density != pixel_density || density != context->GetDensityIndependentPixelRatio() ) {
        pixel_density = density;
        context->SetDensityIndependentPixelRatio( density );
    }
}

int key_modifier_state()
{
    const SDL_Keymod mods = SDL_GetModState();
    int state = 0;
    if( mods & SDL_KMOD_CTRL ) {
        state |= Rml::Input::KM_CTRL;
    }
    if( mods & SDL_KMOD_SHIFT ) {
        state |= Rml::Input::KM_SHIFT;
    }
    if( mods & SDL_KMOD_ALT ) {
        state |= Rml::Input::KM_ALT;
    }
    if( mods & SDL_KMOD_GUI ) {
        state |= Rml::Input::KM_META;
#if defined(__APPLE__)
        // RmlUi only knows the clipboard and select-all shortcuts by Ctrl,
        // which macOS puts on Cmd.
        state |= Rml::Input::KM_CTRL;
#endif
    }
    if( mods & SDL_KMOD_CAPS ) {
        state |= Rml::Input::KM_CAPSLOCK;
    }
    if( mods & SDL_KMOD_NUM ) {
        state |= Rml::Input::KM_NUMLOCK;
    }
    return state;
}

// RmlUi numbers the buttons left, right, middle; SDL left, middle, right.
int mouse_button_index( int sdl_button )
{
    switch( sdl_button ) {
        case SDL_BUTTON_LEFT:
            return 0;
        case SDL_BUTTON_RIGHT:
            return 1;
        case SDL_BUTTON_MIDDLE:
            return 2;
        default:
            return sdl_button - 1;
    }
}

// Where the mouse last was, in context pixels
point mouse_position;
// Keys pressed since rml_ui::take_key_presses() last took them
std::vector<rml_ui::key_press> key_presses;
// When the last frame was drawn, in ms, and the time between frames, smoothed
uint64_t last_frame_ms = 0;
float smoothed_frame_ms = 0.f;

void move_mouse( float x, float y, int modifiers )
{
    mouse_position = point( static_cast<int>( x * pixel_density ),
                            static_cast<int>( y * pixel_density ) );
    context->ProcessMouseMove( mouse_position.x, mouse_position.y, modifiers );
}

// Only the keys RmlUi acts on by default: text editing, navigation, and the
// letters its Ctrl shortcuts use. Printable text arrives as text input.
Rml::Input::KeyIdentifier convert_key( SDL_Keycode key )
{
    if( key >= SDLK_A && key <= SDLK_Z ) {
        return static_cast<Rml::Input::KeyIdentifier>( Rml::Input::KI_A +
                static_cast<int>( key - SDLK_A ) );
    }
    if( key >= SDLK_0 && key <= SDLK_9 ) {
        return static_cast<Rml::Input::KeyIdentifier>( Rml::Input::KI_0 +
                static_cast<int>( key - SDLK_0 ) );
    }
    switch( key ) {
        case SDLK_SPACE:
            return Rml::Input::KI_SPACE;
        case SDLK_RETURN:
            return Rml::Input::KI_RETURN;
        case SDLK_KP_ENTER:
            return Rml::Input::KI_NUMPADENTER;
        case SDLK_ESCAPE:
            return Rml::Input::KI_ESCAPE;
        case SDLK_BACKSPACE:
            return Rml::Input::KI_BACK;
        case SDLK_TAB:
            return Rml::Input::KI_TAB;
        case SDLK_DELETE:
            return Rml::Input::KI_DELETE;
        case SDLK_INSERT:
            return Rml::Input::KI_INSERT;
        case SDLK_HOME:
            return Rml::Input::KI_HOME;
        case SDLK_END:
            return Rml::Input::KI_END;
        case SDLK_PAGEUP:
            return Rml::Input::KI_PRIOR;
        case SDLK_PAGEDOWN:
            return Rml::Input::KI_NEXT;
        case SDLK_LEFT:
            return Rml::Input::KI_LEFT;
        case SDLK_RIGHT:
            return Rml::Input::KI_RIGHT;
        case SDLK_UP:
            return Rml::Input::KI_UP;
        case SDLK_DOWN:
            return Rml::Input::KI_DOWN;
        default:
            return Rml::Input::KI_UNKNOWN;
    }
}

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

// The player's theme, if any, and whether it has been looked for yet.
Rml::SharedPtr<Rml::StyleSheetContainer> theme_sheet;
bool theme_loaded = false;

const Rml::SharedPtr<Rml::StyleSheetContainer> &get_theme_sheet()
{
    if( !theme_loaded ) {
        theme_loaded = true;
        theme_sheet.reset();
        const cata_path theme_path = PATH_INFO::config_dir_path() / "rmlui_theme.rcss";
        if( file_exist( theme_path ) ) {
            theme_sheet = Rml::Factory::InstanceStyleSheetFile(
                              theme_path.get_unrelative_path().generic_u8string() );
        }
    }
    return theme_sheet;
}

// Each document's own stylesheet, before the palette and theme were added,
// so a new theme can be applied over it.
std::map<const rml_ui::document *, Rml::SharedPtr<Rml::StyleSheetContainer>> own_sheets;

void apply_shared_sheets( const rml_ui::document *owner, Rml::ElementDocument *doc )
{
    const Rml::SharedPtr<Rml::StyleSheetContainer> &own = own_sheets[owner];
    Rml::SharedPtr<Rml::StyleSheetContainer> combined = own;
    for( const Rml::SharedPtr<Rml::StyleSheetContainer> *sheet : {
             &get_palette_sheet(), &get_theme_sheet()
         } ) {
        if( *sheet ) {
            combined = combined ? combined->CombineStyleSheetContainer( **sheet ) : *sheet;
        }
    }
    doc->SetStyleSheetContainer( combined );
}

} // namespace

namespace rml_ui
{

void init( const SDL_Renderer_Ptr &renderer )
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
    // Unifont doubles as the monospace family and as the fallback for glyphs
    // Roboto lacks, such as CJK.
    const std::string mono_font = PATH_INFO::fontdir() + "unifont.ttf";
    if( !Rml::LoadFontFace( mono_font, "cata-mono", Rml::Style::FontStyle::Normal,
                            Rml::Style::FontWeight::Auto, true ) ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: failed to load font face " << mono_font;
    }

    context = Rml::CreateContext( "cata", output_size() );
    if( !context ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: CreateContext failed";
        Rml::Shutdown();
        system_interface.reset();
        render_interface.reset();
        return;
    }

    sync_to_window();
    initialised = true;
    const Rml::Vector2i size = context->GetDimensions();
    DebugLog( D_INFO, DC_ALL ) << "RmlUi: initialised at " << size.x << "x" << size.y
                               << ", pixel density " << pixel_density;
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
    theme_sheet.reset();
    theme_loaded = false;
    own_sheets.clear();
    context = nullptr;
    saved_renderer = nullptr;
    pixel_density = 1.f;
    Rml::Shutdown();
    render_interface.reset();
    system_interface.reset();
}

void process_event( const SDL_Event &ev )
{
    if( !initialised || !context ) {
        return;
    }
    const int modifiers = key_modifier_state();
    switch( ev.type ) {
        case SDL_EVENT_MOUSE_MOTION:
            move_mouse( ev.motion.x, ev.motion.y, modifiers );
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            // Move first, so a click with no motion before it -- say, on a
            // popup that opened under a resting cursor -- lands where it was made.
            move_mouse( ev.button.x, ev.button.y, modifiers );
            const int button = mouse_button_index( ev.button.button );
            if( ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ) {
                context->ProcessMouseButtonDown( button, modifiers );
            } else {
                context->ProcessMouseButtonUp( button, modifiers );
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            // SDL counts scrolling up as positive, RmlUi as negative.
            context->ProcessMouseWheel( Rml::Vector2f( 0.f, -ev.wheel.y ), modifiers );
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            context->ProcessMouseLeave();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT: {
            if( ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat ) {
                // Bounded, in case nothing takes them
                if( key_presses.size() >= 64 ) {
                    key_presses.erase( key_presses.begin() );
                }
                key_presses.push_back( { static_cast<int>( ev.key.key ),
                                         ( modifiers & Rml::Input::KM_CTRL ) != 0 } );
            }
            // See the header: keys only reach RmlUi while the document with
            // its focus is the top UI.
            Rml::Element *focus = context->GetFocusElement();
            Rml::ElementDocument *focus_doc = focus ? focus->GetOwnerDocument() : nullptr;
            const auto owner = std::find_if( live_documents.begin(), live_documents.end(),
            [focus_doc]( const document * live ) {
                return focus_doc && live->doc == focus_doc;
            } );
            // is_on_top is only refreshed for adaptors the last pass redrew, so
            // also require that: one below a disable_uis_below adaptor, such as
            // a debugmsg, keeps whatever it had before.
            if( owner == live_documents.end() ||
                std::find( pass_documents.begin(), pass_documents.end(),
                           *owner ) == pass_documents.end() ||
                !( *owner )->adaptor->is_on_top ) {
                break;
            }
            if( ev.type == SDL_EVENT_TEXT_INPUT ) {
                context->ProcessTextInput( Rml::String( ev.text.text ) );
            } else if( ev.type == SDL_EVENT_KEY_DOWN ) {
                context->ProcessKeyDown( convert_key( ev.key.key ), modifiers );
            } else {
                context->ProcessKeyUp( convert_key( ev.key.key ), modifiers );
            }
            break;
        }
        default:
            break;
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
    // adaptor, so it should not be seen; nor should one under a catacurses
    // UI, which RmlUi would otherwise draw over.
    for( document *doc : live_documents ) {
        Rml::ElementDocument *element = doc->get();
        const bool wanted = std::find( pass_documents.begin(), pass_documents.end(),
                                       doc ) != pass_documents.end() &&
                            !doc->adaptor->has_non_rml_above();
        if( !element || element->IsVisible() == wanted ) {
            continue;
        }
        if( wanted ) {
            // No focus here: a document asks for it explicitly, below.
            element->Show( Rml::ModalFlag::None, Rml::FocusFlag::None );
        } else {
            element->Hide();
        }
    }
    // Focusing makes RmlUi pull the document to the front on its own, so the
    // stacking order has to be reapplied after.
    bool focused = false;
    for( document *doc : pass_documents ) {
        if( doc->pending_focus.empty() || !doc->get() ) {
            continue;
        }
        if( Rml::Element *element = doc->get()->GetElementById( doc->pending_focus ) ) {
            element->Focus();
            focused = true;
        }
        doc->pending_focus.clear();
    }
    // Pulling each to the front in turn, bottom first, leaves the context in
    // stack order.
    if( focused || pass_documents != applied_order ) {
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

    sync_to_window();

    const uint64_t now = SDL_GetTicks();
    if( last_frame_ms != 0 ) {
        const float frame_ms = static_cast<float>( now - last_frame_ms );
        smoothed_frame_ms = smoothed_frame_ms <= 0.f ? frame_ms :
                            smoothed_frame_ms * 0.9f + frame_ms * 0.1f;
    }
    last_frame_ms = now;

    // draw_window() sets logical presentation to the display_buffer size when
    // scaling_factor > 1 (sdltiles.cpp) and never resets it, so by the time we
    // draw, renderer coordinates are buffer-space and get scaled up to the
    // window. Our context is sized in output pixels, so drop to 1:1 for the
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

void release_gpu_resources()
{
    if( initialised ) {
        Rml::ReleaseTextures( render_interface.get() );
    }
}

void rebind_renderer( const SDL_Renderer_Ptr &renderer )
{
    if( !initialised || !renderer ) {
        return;
    }
    saved_renderer = renderer.get();
    render_interface->set_renderer( renderer.get() );
}

static Rml::Element *focused_text_field()
{
    Rml::Element *focus = initialised && context ? context->GetFocusElement() : nullptr;
    return rmlui_dynamic_cast<Rml::ElementFormControlInput *>( focus ) != nullptr ? focus : nullptr;
}

bool text_field_focused()
{
    return focused_text_field() != nullptr;
}

void blur_text_field()
{
    if( Rml::Element *focus = focused_text_field() ) {
        focus->Blur();
    }
}

bool active()
{
    return initialised;
}

point viewport_px()
{
    if( !context ) {
        return point::zero;
    }
    const Rml::Vector2i size = context->GetDimensions();
    return point( size.x, size.y );
}

float frame_time_ms()
{
    return smoothed_frame_ms;
}

point mouse_px()
{
    return mouse_position;
}

std::vector<key_press> take_key_presses()
{
    std::vector<key_press> taken;
    taken.swap( key_presses );
    return taken;
}

float dp_ratio()
{
    return context ? context->GetDensityIndependentPixelRatio() : 1.f;
}

float display_buffer_scale()
{
    int buffer_w = 0;
    int buffer_h = 0;
    get_display_buffer_dims( &buffer_w, &buffer_h );
    const point view = viewport_px();
    if( buffer_w <= 0 || buffer_h <= 0 || view.x <= 0 || view.y <= 0 ) {
        return 1.f;
    }
    return static_cast<float>( std::max( 1, std::min( view.x / buffer_w, view.y / buffer_h ) ) );
}

void reload_theme()
{
    theme_loaded = false;
    for( document *live : live_documents ) {
        if( live->doc ) {
            apply_shared_sheets( live, live->doc );
        }
    }
}

std::string file_source( const cata_path &file )
{
    std::error_code ec;
    const std::filesystem::path documents = std::filesystem::absolute(
            ( PATH_INFO::datadir_path() / "raw" / "rmlui" ).get_unrelative_path(), ec );
    const std::filesystem::path target = std::filesystem::absolute( file.get_unrelative_path(), ec );
    const std::filesystem::path relative = std::filesystem::relative( target, documents, ec );
    // Another drive, on Windows: an absolute path with a drive letter works there
    return ( ec || relative.empty() ? target : relative ).generic_u8string();
}

document::document( const std::string &file )
{
    if( !initialised || !context ) {
        return;
    }
    const std::string path =
        ( PATH_INFO::datadir_path() / "raw" / "rmlui" / file ).generic_u8string();
    doc = context->LoadDocument( path );
    if( !doc ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: failed to load document " << path;
        return;
    }
    live_documents.push_back( this );

    // Every document gets the palette, so game text converted by
    // markup_to_rml() shows in its own colors without each document asking,
    // and then the theme, which can override any of it.
    if( const Rml::StyleSheetContainer *own = doc->GetStyleSheetContainer() ) {
        own_sheets[this] = own->CombineStyleSheetContainer( Rml::StyleSheetContainer() );
    }
    apply_shared_sheets( this, doc );

    // Draws nothing into the display buffer, so it has no position: being
    // redrawn is just how this document learns it is still enabled, and where
    // it sits in the stack.
    adaptor = std::make_unique<ui_adaptor>();
    adaptor->is_rml = true;
    adaptor->on_redraw( [this]( ui_adaptor & ui ) {
        // No catacurses window here, so leave the cursor to the UIs that have one.
        ui.disable_cursor();
        pass_documents.push_back( this );
        on_redraw();
    } );
}

document::~document()
{
    // Pop the adaptor first so no redraw pass can see this document mid-teardown.
    adaptor.reset();
    forget_document( live_documents, this );
    forget_document( pass_documents, this );
    forget_document( applied_order, this );
    own_sheets.erase( this );
    if( doc ) {
        // Detaches from the context immediately, so it is not drawn again.
        doc->Close();
    }
}

Rml::ElementDocument *document::get() const
{
    return doc;
}

void document::set_inner_rml( const std::string &id, const std::string &rml )
{
    if( Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr ) {
        element->SetInnerRML( rml );
    }
}

void document::set_class( const std::string &id, const std::string &name, bool active )
{
    if( Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr ) {
        element->SetClass( name, active );
    }
}

int document::hovered_child( const std::string &id ) const
{
    Rml::Element *container = doc && context ? doc->GetElementById( id ) : nullptr;
    if( !container ) {
        return -1;
    }
    for( Rml::Element *element = context->GetHoverElement(); element;
         element = element->GetParentNode() ) {
        if( element->GetParentNode() != container ) {
            continue;
        }
        for( int ind = 0; ind < container->GetNumChildren(); ++ind ) {
            if( container->GetChild( ind ) == element ) {
                return ind;
            }
        }
    }
    return -1;
}

bool document::hovering( const std::string &id ) const
{
    if( !doc || !context ) {
        return false;
    }
    for( Rml::Element *element = context->GetHoverElement(); element;
         element = element->GetParentNode() ) {
        if( element->GetOwnerDocument() != doc ) {
            return false;
        }
        if( element->GetId() == id ) {
            return true;
        }
    }
    return false;
}

std::optional<box> document::element_box( const std::string &id ) const
{
    Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr;
    if( !element ) {
        return std::nullopt;
    }
    const Rml::Vector2f offset = element->GetAbsoluteOffset( Rml::BoxArea::Border );
    const Rml::Vector2f size = element->GetBox().GetSize( Rml::BoxArea::Border );
    return box{ offset.x, offset.y, size.x, size.y };
}

std::string document::hovered_attribute( const std::string &name ) const
{
    if( !doc || !context ) {
        return std::string();
    }
    for( Rml::Element *element = context->GetHoverElement(); element;
         element = element->GetParentNode() ) {
        if( element->GetOwnerDocument() != doc ) {
            return std::string();
        }
        if( const Rml::Variant *value = element->GetAttribute( name ) ) {
            return value->Get<Rml::String>();
        }
    }
    return std::string();
}

void document::scroll_into_view( const std::string &id )
{
    if( !doc ) {
        return;
    }
    doc->UpdateDocument();
    if( Rml::Element *element = doc->GetElementById( id ) ) {
        element->ScrollIntoView( Rml::ScrollIntoViewOptions( Rml::ScrollAlignment::Nearest,
                                 Rml::ScrollAlignment::Nearest ) );
    }
}

void document::set_attribute( const std::string &id, const std::string &name,
                              const std::string &value )
{
    if( Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr ) {
        element->SetAttribute( name, value );
    }
}

std::optional<std::string> document::get_value( const std::string &id ) const
{
    Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr;
    if( auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>( element ) ) {
        return control->GetValue();
    }
    return std::nullopt;
}

void document::set_value( const std::string &id, const std::string &value )
{
    Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr;
    if( auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>( element ) ) {
        input->SetValue( value );
        const int end = static_cast<int>( Rml::StringUtilities::LengthUTF8( value ) );
        input->SetSelectionRange( end, end );
    } else if( auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>( element ) ) {
        control->SetValue( value );
    }
}

void document::focus( const std::string &id )
{
    pending_focus = id;
}

void document::blur( const std::string &id )
{
    if( pending_focus == id ) {
        pending_focus.clear();
    }
    if( Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr ) {
        element->Blur();
    }
}

void document::update_layout()
{
    if( doc ) {
        doc->UpdateDocument();
    }
}

void document::scroll( const std::string &id, rml_ui::scroll how )
{
    Rml::Element *element = doc ? doc->GetElementById( id ) : nullptr;
    if( !element ) {
        return;
    }
    const float line = element->GetLineHeight();
    const float page = std::max( line, element->GetClientHeight() - line );
    switch( how ) {
        case rml_ui::scroll::top:
            element->SetScrollTop( 0.f );
            break;
        case rml_ui::scroll::bottom:
            element->SetScrollTop( element->GetScrollHeight() );
            break;
        case rml_ui::scroll::line_up:
            element->SetScrollTop( element->GetScrollTop() - line );
            break;
        case rml_ui::scroll::line_down:
            element->SetScrollTop( element->GetScrollTop() + line );
            break;
        case rml_ui::scroll::page_up:
            element->SetScrollTop( element->GetScrollTop() - page );
            break;
        case rml_ui::scroll::page_down:
            element->SetScrollTop( element->GetScrollTop() + page );
            break;
    }
}

} // namespace rml_ui

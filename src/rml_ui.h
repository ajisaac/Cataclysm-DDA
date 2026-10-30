#pragma once
#ifndef CATA_SRC_RML_UI_H
#define CATA_SRC_RML_UI_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "point.h"
#include "sdl_wrappers.h"

class cata_path;
class ui_adaptor;

namespace Rml
{
class ElementDocument;
} // namespace Rml

/**
 * The RmlUi layer the game's UI is drawn with.
 *
 * RmlUi draws at present time, after the display buffer has been blitted, so
 * it always sits above the catacurses cell grid. Among themselves, documents
 * follow the ui_manager stack: see rml_ui::document.
 *
 * This header does not include RmlUi, so callers do not pay for its headers.
 */
namespace rml_ui
{

/**
 * Bring up RmlUi against an existing SDL_Renderer. Safe to call twice.
 *
 * The context works in the renderer's output pixels, with `dp` scaled by the
 * window's pixel density, so documents keep their physical size on HiDPI
 * displays. render() keeps both in step with the window.
 */
void init( const SDL_Renderer_Ptr &renderer );

/// Tear down. Safe to call without a prior init(). Documents still alive
/// afterwards become inert.
void shutdown();

/// Update and draw. Call inside the frame, before presenting.
void render();

/**
 * Drop RmlUi's textures, font glyph atlases included, after a renderer device
 * reset has invalidated them, or before the renderer that made them goes.
 * RmlUi recreates them as it next draws.
 */
void release_gpu_resources();

/// Draw with this renderer from now on, after the game has replaced its own.
void rebind_renderer( const SDL_Renderer_Ptr &renderer );

/// Whether a text field in a document has keyboard focus.
bool text_field_focused();
/// Take keyboard focus from whichever text field has it.
void blur_text_field();

/**
 * Pass an SDL event on to RmlUi, in the window coordinates SDL reported it
 * in. RmlUi sees events as well as the game, not instead of it: mouse events
 * always, and keyboard and text events only while the document holding
 * RmlUi's focus is the top UI, so that typing into whatever is on top never
 * also edits a field behind it. Game actions still come from input_context.
 */
void process_event( const SDL_Event &ev );

/// Whether init() has succeeded and shutdown() has not been called since.
bool active();

/// The context's size, in its own pixels -- RCSS `px`. Zero before init().
point viewport_px();
/// Context pixels per RCSS `dp`: the window's pixel density.
float dp_ratio();
/// Where the mouse last was, in context pixels.
point mouse_px();
/// The time between the frames render() has drawn lately, smoothed, in ms;
/// zero before two have been.
float frame_time_ms();

/// A key pressed, as SDL reported it, with whether Ctrl was down (Cmd on macOS).
struct key_press {
    int key = 0;
    bool ctrl = false;
};
/// The keys pressed since this was last called, oldest first, whichever UI
/// had them.
std::vector<key_press> take_key_presses();
/// Context pixels per display buffer pixel, for placing documents over
/// catacurses windows. The display buffer is shown at the largest whole
/// multiple of its size that fits.
float display_buffer_scale();

/**
 * Load the theme from config/rmlui_theme.rcss again, if there is one, and
 * apply it to every document, open ones included. A theme is a stylesheet
 * attached after each document's own, so its rules win where they match as
 * specifically; see data/raw/rmlui/themes/.
 */
void reload_theme();

/**
 * A file as a document's `src` can name it, such as an <img>'s: relative to
 * data/raw/rmlui/, since RmlUi takes a leading `/` to mean relative too.
 */
std::string file_source( const cata_path &file );

/// Where an element is and how big, in context pixels; see document::element_box().
struct box {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

/// How to scroll an element; see document::scroll().
enum class scroll : int {
    top,
    bottom,
    line_up,
    line_down,
    page_up,
    page_down
};

/**
 * Bracket one pass of ui_adaptor::redraw_invalidated(). Between the two,
 * every enabled document's adaptor is redrawn in stack order; end_redraw_pass()
 * then shows exactly those documents, in that order, and hides the rest.
 * Only ui_manager should call these.
 */
void begin_redraw_pass();
void end_redraw_pass();

/**
 * An RmlUi document that takes part in the ui_manager stack.
 *
 * Construction loads the document and pushes a ui_adaptor onto the stack;
 * destruction closes the document and pops the adaptor. In between, the
 * document is shown only while its adaptor is enabled -- a `disable_uis_below`
 * adaptor higher up hides it -- and documents stack in the same order as
 * their adaptors.
 *
 * Because RmlUi draws last, a document would be above every catacurses window
 * regardless of stack position. So while a catacurses UI is above it in the
 * stack, the document is hidden instead.
 *
 * If RmlUi is not initialised -- as in tests -- or the document fails to
 * load, no adaptor is pushed and get() returns nullptr, so callers can
 * construct one unconditionally.
 */
class document
{
    public:
        /**
         * @param file Document file under data/raw/rmlui/, e.g.
         *             "query_popup.rml". Relative paths inside it, such
         *             as a link to common.rcss, resolve from that directory.
         */
        explicit document( const std::string &file );
        virtual ~document();

        document( const document & ) = delete;
        document &operator=( const document & ) = delete;

        /// The loaded document, or nullptr. See the class comment.
        Rml::ElementDocument *get() const;

    protected:
        /**
         * Called on every redraw pass in which this document is enabled, the
         * place to bring it up to date with game state. Keep it cheap when
         * nothing has changed, since it runs on every pass.
         */
        virtual void on_redraw() {}

        /**
         * Small DOM helpers, so a subclass can drive its document without
         * including RmlUi. Each is a no-op when the document or the element
         * with that id is missing.
         */
        /// Replace the content of the element with this id.
        void set_inner_rml( const std::string &id, const std::string &rml );
        /// Turn a class on the element with this id on or off.
        void set_class( const std::string &id, const std::string &name, bool active );
        /// Index of the child of the element with this id that the mouse is
        /// over, directly or through one of its descendants, or -1.
        int hovered_child( const std::string &id ) const;
        /// Whether the mouse is over the element with this id.
        bool hovering( const std::string &id ) const;
        /// The border box of the element with this id, as last laid out; see
        /// update_layout(). nullopt if there is no such element.
        std::optional<rml_ui::box> element_box( const std::string &id ) const;
        /// The attribute with this name on the element the mouse is over, or
        /// on its nearest ancestor that has one; empty if none does, or if the
        /// mouse is not over this document.
        std::string hovered_attribute( const std::string &name ) const;
        /// Scroll the containers of the element with this id the least that
        /// brings it into view, laying the document out first so that an
        /// element just added can be revealed.
        void scroll_into_view( const std::string &id );
        /// Set an attribute on the element with this id.
        void set_attribute( const std::string &id, const std::string &name,
                            const std::string &value );
        /// The value of the form control with this id, as typed so far, or
        /// nullopt if there is no such control.
        std::optional<std::string> get_value( const std::string &id ) const;
        /// Replace the value of the form control with this id, leaving a text
        /// field's caret at the end.
        void set_value( const std::string &id, const std::string &value );
        /// Give the element with this id keyboard focus once the document is
        /// next shown -- RmlUi cannot focus an element while it is hidden.
        void focus( const std::string &id );
        /// Scroll the element with this id, which needs `overflow: auto`. A
        /// page is its height less a line, so a line of context stays.
        void scroll( const std::string &id, rml_ui::scroll how );
        /// Take keyboard focus from the element with this id, if it has it.
        void blur( const std::string &id );
        /// Lay the document out now rather than at the next draw, so that
        /// content just set can be measured and scrolled -- to its end, say.
        void update_layout();

    private:
        friend void shutdown();
        friend void reload_theme();
        friend void end_redraw_pass();
        friend void process_event( const SDL_Event &ev );

        Rml::ElementDocument *doc = nullptr;
        std::unique_ptr<ui_adaptor> adaptor;
        std::string pending_focus;
};

} // namespace rml_ui

#endif // CATA_SRC_RML_UI_H

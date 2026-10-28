#pragma once
#ifndef CATA_SRC_RML_UI_H
#define CATA_SRC_RML_UI_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "point.h"
#include "sdl_wrappers.h"

class ui_adaptor;

namespace Rml
{
class ElementDocument;
} // namespace Rml

/**
 * The RmlUi layer the game's UI is moving to.
 *
 * RmlUi draws at present time, after the display buffer has been blitted, so
 * it always sits above both the catacurses cell grid and ImGui. Among
 * themselves, documents follow the ui_manager stack: see rml_ui::document.
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
/// Context pixels per ImGui pixel, for honouring sizes and positions callers
/// worked out with ImGui. ImGui draws into the display buffer, which is shown
/// at the largest whole multiple of its size that fits.
float imgui_px_scale();

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
 * An RmlUi document that takes part in the ui_manager stack, the way
 * cataimgui::window does for ImGui.
 *
 * Construction loads the document and pushes a ui_adaptor onto the stack;
 * destruction closes the document and pops the adaptor. In between, the
 * document is shown only while its adaptor is enabled -- a `disable_uis_below`
 * adaptor higher up hides it -- and documents stack in the same order as
 * their adaptors.
 *
 * Limitation: because RmlUi draws last, a document is above every catacurses
 * and ImGui window regardless of stack position, so a catacurses window opened
 * on top of a document ends up underneath it.
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
         *             "main_menu_replica.rml". Relative paths inside it, such
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
         * place to bring it up to date with game state -- the counterpart of
         * cataimgui::window::draw_controls(). Keep it cheap when nothing has
         * changed, since it runs on every pass.
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

    private:
        friend void shutdown();
        friend void end_redraw_pass();
        friend void process_event( const SDL_Event &ev );

        Rml::ElementDocument *doc = nullptr;
        std::unique_ptr<ui_adaptor> adaptor;
        std::string pending_focus;
};

/**
 * RmlUi replica of the main menu, drawn over the catacurses original for
 * side-by-side comparison. A smoke test for rml_ui::document, not a
 * replacement.
 */
class main_menu_replica : public document
{
    public:
        main_menu_replica();

        /**
         * @param items Menu labels in the game's own markup, e.g. "<M|m>OTD" --
         *              the bracketed part marks the hotkey letter and is
         *              rendered as an accented character rather than printed
         *              literally.
         * @param selected Index of the highlighted entry, mirroring main_menu's
         *                 sel1.
         *
         * Cheap to call every redraw: the element list is only rebuilt when
         * `items` actually changes, and the selection is just a CSS class.
         */
        void update( const std::vector<std::string> &items, int selected );

    private:
        std::vector<std::string> cached_items;
};

} // namespace rml_ui

#endif // CATA_SRC_RML_UI_H

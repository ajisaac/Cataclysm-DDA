#pragma once
#ifndef CATA_SRC_RML_UI_H
#define CATA_SRC_RML_UI_H

#include <memory>
#include <string>
#include <vector>

#include "sdl_wrappers.h"

class ui_adaptor;

namespace Rml
{
class ElementDocument;
} // namespace Rml

/**
 * Experimental RmlUi layer, built behind the `RMLUI` CMake flag.
 *
 * RmlUi draws at present time, after the display buffer has been blitted, so
 * it always sits above both the catacurses cell grid and ImGui. Among
 * themselves, documents follow the ui_manager stack: see rml_ui::document.
 *
 * Everything here is a no-op unless CATA_RMLUI is defined, so the normal build
 * is unaffected and call sites need no guards of their own.
 */
namespace rml_ui
{

/// Bring up RmlUi against an existing SDL_Renderer. Safe to call twice.
void init( const SDL_Renderer_Ptr &renderer, int width, int height );

/// Tear down. Safe to call without a prior init(). Documents still alive
/// afterwards become inert.
void shutdown();

/// Tell RmlUi the output size changed.
void resize( int width, int height );

/// Update and draw. Call inside the frame, before presenting.
void render();

/// Whether the spike is compiled in and initialised.
bool active();

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
 * If RmlUi is compiled out, not initialised, or the document fails to load,
 * no adaptor is pushed and get() returns nullptr, so callers can construct one
 * unconditionally.
 */
class document
{
    public:
        /**
         * @param rml Document source.
         * @param source_url Name for RmlUi's log messages, and the base for
         *                   resolving relative paths inside the document.
         */
        document( const std::string &rml, const std::string &source_url );
        virtual ~document();

        document( const document & ) = delete;
        document &operator=( const document & ) = delete;

        /// The loaded document, or nullptr. See the class comment.
        Rml::ElementDocument *get() const;

    private:
        friend void shutdown();

        Rml::ElementDocument *doc = nullptr;
        std::unique_ptr<ui_adaptor> adaptor;
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

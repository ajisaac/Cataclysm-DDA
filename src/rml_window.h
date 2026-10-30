#pragma once
#ifndef CATA_SRC_RML_WINDOW_H
#define CATA_SRC_RML_WINDOW_H

#include <map>
#include <string>
#include <vector>

#include "color.h"
#include "rml_ui.h"

class input_context;

namespace rml_ui
{

/**
 * A framed window drawn from data/raw/rmlui/window.rml: a title bar with a
 * close button, optional tabs and header, a scrolling body, a filter field
 * and a footer, for screens that build their content as RML in code with the
 * classes in window.rcss.
 *
 * A subclass fills it from on_redraw(), the counterpart of draw_controls().
 * show() and the setters only touch the DOM when the content changes, so a
 * screen can rebuild its RML on every redraw.
 *
 * Mouse input still arrives through the screen's input_context, which needs
 * SELECT registered to see clicks:
 * - An element with `data-action="ACTION"` is a button. resolve_click() turns
 *   a click on it into that action.
 * - hovered_index() reads a number from a `data-...` attribute, for rows and
 *   the like. Tabs carry `data-tab`, read by hovered_tab().
 * The wheel scrolls whatever it is over by itself.
 */
class window : public document
{
    public:
        /**
         * @param title Markup for the title bar.
         * @param file A document with the same element ids as window.rml, for
         *             a screen that needs styles of its own.
         */
        explicit window( const std::string &title, const std::string &file = "window.rml" );

        void set_title( const std::string &markup );
        /// Size the window in RCSS lengths, such as "60em" or "90%". An empty
        /// length fits the content on that axis.
        void set_size( const std::string &width, const std::string &height );
        /// Place the window's top left corner, in RCSS lengths from the
        /// screen's. An empty length centers it on that axis, the default.
        void set_position( const std::string &left, const std::string &top );
        /// Tabs across the top, `active` highlighted; none if `names` is empty.
        /// Names are markup.
        void set_tabs( const std::vector<std::string> &names, size_t active );
        /// The RML above the body, which stays put while the body scrolls.
        /// Empty hides it.
        void set_header( const std::string &rml );
        void set_body( const std::string &rml );
        /// The RML below the body; empty hides it.
        void set_footer( const std::string &rml );
        /// Whether the body scrolls as a whole, the default, or lays out its
        /// content to fill it, for panes that scroll on their own.
        void set_body_scrolls( bool scrolls );
        /// The action the close button gives, QUIT unless set; empty hides it.
        void set_close_action( const std::string &action );

        /// Fill the element with this id, if that changes it. Returns whether
        /// it did.
        bool show( const std::string &id, const std::string &rml );

        /// If `action` is a click over an element with a data-action, that
        /// action; otherwise `action` itself.
        std::string resolve_click( const std::string &action ) const;
        /// The number in the attribute with this name, on the element the mouse
        /// is over or its nearest ancestor that has one; -1 if there is none.
        int hovered_index( const std::string &attribute ) const;
        /// The tab the mouse is over, or -1.
        int hovered_tab() const;

        void scroll_body( rml_ui::scroll how );

        /// Show the filter field holding this text. It takes no typing until
        /// start_filter().
        void show_filter( const std::string &text );
        /**
         * Show the filter field holding this text and give it keyboard focus,
         * so it takes typing. Keys still reach the game too, so meanwhile the
         * screen should only act on confirm and cancel, and poll with a short
         * timeout, reading filter_text() as it changes.
         */
        void start_filter( const std::string &text );
        /// Stop the filter field taking typing, leaving it shown.
        void stop_filter();
        void hide_filter();
        /// What the filter field holds now.
        std::string filter_text() const;

    private:
        void set_filter_enabled( bool enabled );

        void apply_style();

        std::map<std::string, std::string> shown;
        std::string size_width;
        std::string size_height;
        std::string pos_left;
        std::string pos_top;
        std::string applied_window_style;
        std::string applied_root_style;
        std::string close_action;
};

/**
 * "[key] description" for an action, with the description in the active
 * hotkey color while `active`. An `action` of a single character is shown as
 * it is.
 */
std::string keybinding_markup( const input_context &ctxt, const std::string &action,
                               const std::string &description, bool active = false,
                               const nc_color &default_color = c_light_gray );

/**
 * A button that gives `action` when clicked; see window::resolve_click().
 * `label_rml` is RML, such as markup_to_rml() returns.
 */
std::string button_rml( const std::string &action, const std::string &label_rml,
                        bool enabled = true );

/**
 * A button labelled with an action's key and name, as input_context's
 * get_button_text() describes it.
 */
std::string action_button_rml( const input_context &ctxt, const std::string &action,
                               const std::string &label = std::string(), bool enabled = true );

/// Text, escaped and in this color, the plain counterpart of markup_to_rml().
std::string colored_rml( const std::string &text, const nc_color &color );

/// An RCSS length covering this many columns or rows of the game's terminal
/// grid, for placing windows the way the catacurses layout would.
std::string term_width( float columns );
std::string term_height( float rows );

/// An RCSS width holding about this many characters of the window's font.
std::string chars_width( int chars );
/// An RCSS height holding about this many lines of the window's font.
std::string lines_height( int lines );

/// The height of a `.line` in window.rcss, one row of text, for lists that
/// build only the rows that fit, as uilist does.
constexpr float line_dp = 22.f;
/// How many `.line` rows fit in this fraction of the screen's height, after
/// `reserved_dp` for everything else in the window. At least one.
int lines_fitting( float height_fraction, float reserved_dp );

} // namespace rml_ui

#endif // CATA_SRC_RML_WINDOW_H

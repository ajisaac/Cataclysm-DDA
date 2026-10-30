#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "catacharset.h"
#include "color.h"
#include "cursesdef.h"
#include "diary.h"
#include "input_context.h"
#include "output.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "scores_ui.h"
#include "string_editor_window.h"
#include "string_formatter.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"

namespace
{
/** The diary, from data/raw/rmlui/diary.rml; show_diary_ui() fills it. */
class diary_window : public rml_ui::window
{
    public:
        diary_window( const std::string &title,
                      const std::function<void( diary_window & )> &update ) :
            rml_ui::window( title, "diary.rml" ), update( update ) {
            set_body_scrolls( false );
        }

        void scroll_pane( const std::string &id, rml_ui::scroll how ) {
            scroll( id, how );
        }
        void reveal( const std::string &id ) {
            scroll_into_view( id );
        }

    protected:
        void on_redraw() override {
            update( *this );
        }

    private:
        std::function<void( diary_window & )> update;
};

/** A list of lines, each a row with an id of `prefix` and its index. */
std::string diary_list_rml( const std::vector<std::string> &list, int selection,
                            const std::string &prefix, int pane, const report_color_error color_error )
{
    std::string rml;
    for( int i = 0; i < static_cast<int>( list.size() ); i++ ) {
        rml += string_format( "<div id=\"%s%d\" class=\"item line%s\" data-pane=\"%d\" data-row=\"%d\">",
                              prefix, i, selection == i ? " selected" : "", pane, i ) +
               rml_ui::markup_to_rml( list[i], c_white, color_error ) + "</div>";
    }
    return rml;
}
} // namespace

static std::pair<point, point> diary_window_position()
{
    return {
        point( TERMX / 4, TERMY / 4 ),
        point( TERMX / 2, TERMY / 2 )
    };
}

void diary::show_diary_ui( diary *c_diary )
{
    enum class window_mode : int {PAGE_WIN = 0, CHANGE_WIN, TEXT_WIN, NUM_WIN, FIRST_WIN = 0, LAST_WIN = NUM_WIN - 1};
    window_mode currwin = window_mode::PAGE_WIN;

    const int diary_last_page = c_diary->pages.empty() ? 0 : ( c_diary->pages.size() - 1 );
    std::map<window_mode, int> selected = { {window_mode::PAGE_WIN, diary_last_page}, {window_mode::CHANGE_WIN, 0} };

    input_context ctxt( "DIARY" );
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "NEW_PAGE" );
    ctxt.register_action( "DELETE PAGE" );
    ctxt.register_action( "EXPORT_DIARY" );
    ctxt.register_action( "VIEW_SCORES" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    // Clicks on the pages, the changes and the close button
    ctxt.register_action( "SELECT" );

    // What was last shown, to keep the selections in view and new text at its top
    std::pair<int, int> shown_selection( -1, -1 );

    diary_window window( string_format( _( "%s's Diary" ), c_diary->owner ),
    [&]( diary_window & win ) {
        const point &max = diary_window_position().second;
        win.set_size( rml_ui::term_width( max.x * 3 / 10 + max.x + 10 ), "90%" );

        std::string desc = string_format( _( "%s, %s, %s, %s" ),
                                          ctxt.get_desc( "NEW_PAGE", _( "New page" ), input_context::allow_all_keys ),
                                          ctxt.get_desc( "CONFIRM", _( "Edit text" ), input_context::allow_all_keys ),
                                          ctxt.get_desc( "DELETE PAGE", _( "Delete page" ), input_context::allow_all_keys ),
                                          ctxt.get_desc( "EXPORT_DIARY", _( "Export diary" ), input_context::allow_all_keys )
                                        );
        win.set_header( "<div class=\"center\">" + rml_ui::markup_to_rml( desc, c_white ) +
                        "</div><div class=\"center\">" + rml_ui::markup_to_rml( ctxt.get_desc( "VIEW_SCORES",
                                _( "View achievements, scores, and kills" ), input_context::allow_all_keys ), c_white ) +
                        "</div>" );

        const auto pane_class = [&]( window_mode mode ) {
            return currwin == mode ? "pane diary-pane active" : "pane diary-pane";
        };
        win.set_body( std::string( "<div class=\"panes\"><div class=\"" ) +
                      pane_class( window_mode::PAGE_WIN ) + "\"><div class=\"diary-head\">" +
                      rml_ui::colored_rml( string_format( _( "pages: %d" ), c_diary->get_pages_list().size() ),
                                           c_light_gray ) + "</div><div id=\"diary-pages\" class=\"diary-scroll\">" +
                      diary_list_rml( c_diary->get_pages_list(), selected[window_mode::PAGE_WIN], "diary-page-", 0,
                                      report_color_error::yes ) + "</div></div><div class=\"" + pane_class( window_mode::CHANGE_WIN ) +
                      "\"><div class=\"diary-head\">" + rml_ui::markup_to_rml( c_diary->get_head_text(
                                  c_diary->get_page_ptr()->is_summary() ), c_white ) +
                      "</div><div id=\"diary-changes\" class=\"diary-scroll\">" +
                      diary_list_rml( c_diary->get_change_list(), selected[window_mode::CHANGE_WIN], "diary-change-", 1,
                                      report_color_error::yes ) + "</div></div><div class=\"" + pane_class( window_mode::TEXT_WIN ) +
                      "\"><div id=\"diary-text\" class=\"diary-scroll pre\">" +
                      rml_ui::markup_to_rml( c_diary->get_desc_or_page_text( selected[window_mode::CHANGE_WIN] ),
                                             c_white, report_color_error::no ) + "</div></div></div>" );

        std::string info;
        if( currwin == window_mode::CHANGE_WIN || currwin == window_mode::TEXT_WIN ) {
            if( !c_diary->get_page_ptr()->is_summary() ) {
                info = rml_ui::markup_to_rml( c_diary->get_desc_map()[ selected[window_mode::CHANGE_WIN] ],
                                              c_white );
            }
        }
        win.set_footer( "<div class=\"diary-info\"><div class=\"muted\">" +
                        rml_ui::escape_rml( _( "Info" ) ) + "</div>" + info + "</div>" );

        const std::pair<int, int> selection( selected[window_mode::PAGE_WIN],
                                             selected[window_mode::CHANGE_WIN] );
        if( selection != shown_selection ) {
            shown_selection = selection;
            win.reveal( "diary-page-" + std::to_string( selection.first ) );
            win.reveal( "diary-change-" + std::to_string( selection.second ) );
            win.scroll_pane( "diary-text", rml_ui::scroll::top );
        }
    } );

    while( true ) {

        if( ( !c_diary->pages.empty() &&
              selected[window_mode::PAGE_WIN] >= static_cast<int>( c_diary->pages.size() ) ) ||
            ( c_diary->pages.empty() && selected[window_mode::PAGE_WIN] != 0 ) ) {
            selected[window_mode::PAGE_WIN] = 0;
        }
        selected[window_mode::PAGE_WIN] = c_diary->set_opened_page( selected[window_mode::PAGE_WIN] );
        const int changes = static_cast<int>( c_diary->change_list.size() );
        if( selected[window_mode::CHANGE_WIN] >= changes ) {
            selected[window_mode::CHANGE_WIN] = std::max( 0, changes - 1 );
        }
        ui_manager::redraw_invalidated();
        std::string action = ctxt.handle_input();
        if( action == "SELECT" ) {
            const std::string clicked = window.resolve_click( action );
            const int pane = window.hovered_index( "data-pane" );
            const int row = window.hovered_index( "data-row" );
            if( clicked != action ) {
                // the close button
                action = clicked;
            } else if( pane == 0 && row >= 0 ) {
                currwin = window_mode::PAGE_WIN;
                selected[window_mode::PAGE_WIN] = row;
                selected[window_mode::CHANGE_WIN] = 0;
            } else if( pane == 1 && row >= 0 ) {
                currwin = window_mode::CHANGE_WIN;
                selected[window_mode::CHANGE_WIN] = row;
            }
        }
        if( action == "LEFT" || action == "PREV_TAB" || action == "RIGHT" || action == "NEXT_TAB" ) {
            // necessary to use inc_clamp_wrap()
            static_assert( static_cast<int>( window_mode::FIRST_WIN ) == 0 );
            currwin = inc_clamp_wrap( currwin, action == "RIGHT" || action == "NEXT_TAB",
                                      window_mode::NUM_WIN );
        } else if( currwin == window_mode::TEXT_WIN &&
                   ( action == "UP" || action == "DOWN" || action == "PAGE_UP" || action == "PAGE_DOWN" ) ) {
            // The text scrolls
            window.scroll_pane( "diary-text", action == "UP" ? rml_ui::scroll::line_up :
                                action == "DOWN" ? rml_ui::scroll::line_down :
                                action == "PAGE_UP" ? rml_ui::scroll::page_up : rml_ui::scroll::page_down );
        } else if( currwin != window_mode::TEXT_WIN &&
                   navigate_ui_list( action, selected[currwin], 10,
                                     currwin == window_mode::PAGE_WIN ? c_diary->pages.size() :
                                     c_diary->change_list.size(), true ) ) {
            if( currwin == window_mode::PAGE_WIN ) {
                selected[window_mode::CHANGE_WIN] = 0;
            }
        } else if( action == "CONFIRM" ) {
            if( !c_diary->pages.empty() ) {
                // The editor takes the place of the text
                c_diary->edit_page_ui( []() {
                    const std::pair<point, point> beg_and_max = diary_window_position();
                    const point &beg = beg_and_max.first;
                    const point &max = beg_and_max.second;
                    const int midx = max.x / 2;
                    return catacurses::newwin( max.y - 3, max.x - midx - 1, beg + point( 2 + midx, 3 ) );
                } );
            }
        } else if( action == "NEW_PAGE" ) {
            c_diary->new_page();
            selected[window_mode::PAGE_WIN] = c_diary->pages.size() - 1;
            currwin = window_mode::PAGE_WIN;
        } else if( action == "VIEW_SCORES" ) {
            show_scores_ui();
        } else if( action == "DELETE PAGE" ) {
            if( c_diary->pages.size() > 1 ) {
                if( query_yn( _( "Really delete Page?" ) ) ) {
                    c_diary->delete_page();
                    if( selected[window_mode::PAGE_WIN] >= static_cast<int>( c_diary->pages.size() ) ) {
                        selected[window_mode::PAGE_WIN] --;
                    }
                }
            }
        } else if( action == "EXPORT_DIARY" ) {
            if( query_yn( _( "Export Diary as .txt?" ) ) ) {
                c_diary->export_to_txt();
            }

        } else if( action == "QUIT" ) {
            break;
        }

    }
}

void diary::edit_page_ui( const std::function<catacurses::window()> &create_window )
{
    if( get_page_ptr()->is_summary() ) {
        return;
    }
    // Modify the stored text so the new text is displayed after exiting from
    // the editor window and before confirming or canceling the y/n query.
    std::string &new_text = get_page_ptr()->m_text;
    const std::string old_text = new_text;

    string_editor_window ed( create_window, new_text );

    do {
        const std::pair<bool, std::string> result = ed.query_string();
        new_text = result.second;

        // Confirmed or unchanged
        if( result.first || old_text == new_text ) {
            break;
        }

        const query_ynq_result res = query_ynq( _( "Save entry?" ) );
        if( res == query_ynq_result::yes ) {
            break;
        } else if( res == query_ynq_result::no ) {
            new_text = old_text;
            break;
        }
    } while( true );
}

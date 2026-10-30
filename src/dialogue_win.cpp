#include "dialogue_win.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "debug.h"
#include "input_context.h"
#include "output.h"
#include "rml_markup.h"
#include "string_formatter.h"
#include "translations.h"

// How far PAGE_UP and PAGE_DOWN move the selected response
static const int RESPONSES_PAGE = 5;

/**
 * The dialogue window, from data/raw/rmlui/dialogue.rml: the conversation so
 * far above, and the responses to pick from below, beside the actions that
 * need no response.
 */
class dialogue_view : public rml_ui::window
{
    public:
        dialogue_view( dialogue_window &dialogue, const std::function<std::string()> &npc_name ) :
            rml_ui::window( std::string(), "dialogue.rml" ), dialogue( dialogue ), npc_name( npc_name ) {
            set_body_scrolls( false );
            dialogue.view = this;
            dialogue.history_changed = true;
            dialogue.reveal_response = true;
        }
        ~dialogue_view() override {
            dialogue.view = nullptr;
        }

        void scroll_history( rml_ui::scroll how ) {
            scroll( "dlg-history", how );
        }

    protected:
        void on_redraw() override;

    private:
        dialogue_window &dialogue;
        std::function<std::string()> npc_name;
};

void dialogue_view::on_redraw()
{
    // The window takes the middle of the screen, as the terminal one did
    const int win_beginy = TERMY > FULL_SCREEN_HEIGHT ? ( TERMY - FULL_SCREEN_HEIGHT ) / 4 : 0;
    const int win_beginx = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - FULL_SCREEN_WIDTH ) / 4 : 0;
    const int maxy = win_beginy ? TERMY - 2 * win_beginy : FULL_SCREEN_HEIGHT;
    const int maxx = win_beginx ? TERMX - 2 * win_beginx : FULL_SCREEN_WIDTH;
    set_size( rml_ui::term_width( maxx ), rml_ui::term_height( maxy ) );

    set_title( dialogue.title_markup( npc_name() ) );
    set_header( dialogue.debug_flags_rml() );
    show( "body", dialogue.body_rml() );

    if( dialogue.history_changed ) {
        dialogue.history_changed = false;
        update_layout();
        scroll( "dlg-history", rml_ui::scroll::bottom );
    }
    if( dialogue.reveal_response ) {
        dialogue.reveal_response = false;
        scroll_into_view( "dlg-resp-" + std::to_string( dialogue.sel_response ) );
    }
}

std::unique_ptr<rml_ui::window> dialogue_window::open( const std::function<std::string()>
        &npc_name )
{
    return std::make_unique<dialogue_view>( *this, npc_name );
}

std::string dialogue_window::title_markup( const std::string &name ) const
{
    if( is_computer ) {
        return colorize( string_format( _( "Interaction: %s" ), name ), default_color() );
    } else if( !is_not_conversation ) {
        return colorize( string_format( _( "Dialogue: %s" ), name ), default_color() );
    }
    return std::string();
}

std::string dialogue_window::debug_flags_rml() const
{
    if( !debug_mode ) {
        return std::string();
    }
    const int flag_count = 5;
    std::array<bool, flag_count> debug_flags = { show_dynamic_line_conditionals, show_response_conditionals, show_dynamic_line_effects, show_response_effects, show_all_responses };
    std::array<std::string, flag_count> debug_show_toggle = { "DL_COND", "RESP_COND", "DL_EFF", "RESP_EFF", "ALL_RESP" };
    std::string rml;
    for( int i = 0; i < flag_count; i++ ) {
        rml += rml_ui::colored_rml( debug_show_toggle[i], debug_flags[i] ? c_yellow : c_brown ) + " ";
    }
    return rml + " " + rml_ui::colored_rml( "talk_topic: " + debug_topic_name, default_color() );
}

std::string dialogue_window::body_rml() const
{
    // The conversation so far, the latest lines highlighted
    std::string rml = "<div id=\"dlg-history\">";
    const int newindex = history.size() - num_lines_highlighted;
    for( int i = 0; i < static_cast<int>( history.size() ); ++i ) {
        const nc_color col = ( i >= newindex ) ? history[i].color : c_light_gray;
        rml += history[i].text.empty() ? "<div class=\"dlg-gap\"></div>" :
               "<div class=\"dlg-line\">" + rml_ui::markup_to_rml( history[i].text, col ) + "</div>";
    }
    rml += "</div>";

    std::string prompt;
    if( is_computer ) {
        prompt = _( "Your input:" );
    } else if( is_not_conversation ) {
        prompt = _( "What do you do?" );
    } else {
        prompt = _( "Your response:" );
    }
    rml += "<div id=\"dlg-prompt\">" + rml_ui::colored_rml( prompt, default_color() ) + "</div>";

    // The responses, each clickable
    rml += "<div id=\"dlg-bottom\"><div id=\"dlg-responses\">";
    for( int i = 0; i < static_cast<int>( responses.size() ); ++i ) {
        const talk_data &response = responses[i];
        rml += string_format( "<div id=\"dlg-resp-%d\" class=\"item dlg-resp%s\" data-row=\"%d\">", i,
                              i == sel_response ? " selected" : "", i ) +
               rml_ui::markup_to_rml( formatted_hotkey( response.hotkey_desc, response.color ) +
                                      colorize( response.text, response.color ) ) + "</div>";
    }
    rml += "</div><div id=\"dlg-actions\">";

    // Actions go on the right column; they're unaffected by scrolling.
    if( debug_mode ) {
        for( const std::string &line : responses_debug ) {
            rml += "<div>" + rml_ui::markup_to_rml( line, c_yellow ) + "</div>";
        }
    } else if( !is_computer && !is_not_conversation && !is_remote ) {
        input_context ctxt( "DIALOGUE_CHOOSE_RESPONSE" );
        const std::array<std::pair<std::string, std::string>, 5> actions = { {
                { "LOOK_AT", _( "Look at" ) },
                { "SIZE_UP_STATS", _( "Size up stats" ) },
                { "ASSESS_PERSONALITY", _( "Assess personality" ) },
                { "YELL", _( "Yell" ) },
                { "CHECK_OPINION", _( "Check opinion" ) }
            }
        };
        for( const std::pair<std::string, std::string> &action : actions ) {
            rml += "<div class=\"item\" data-action=\"" + action.first + "\">" + rml_ui::markup_to_rml(
                       formatted_hotkey( ctxt.get_desc( action.first, 1 ), c_magenta ) + action.second,
                       c_magenta ) + "</div>";
        }
    }
    return rml + "</div></div>";
}

void dialogue_window::handle_scrolling( std::string &action, input_context & )
{
    const int count = static_cast<int>( responses.size() );
    const auto select = [&]( int index ) {
        sel_response = std::clamp( index, 0, std::max( 0, count - 1 ) );
        reveal_response = true;
    };
    if( count > 0 && action == "UP" ) {
        select( ( sel_response + count - 1 ) % count );
    } else if( count > 0 && action == "DOWN" ) {
        select( ( sel_response + 1 ) % count );
    } else if( action == "HOME" ) {
        select( 0 );
    } else if( action == "END" ) {
        select( count - 1 );
    } else if( action == "PAGE_UP" ) {
        select( sel_response - RESPONSES_PAGE );
    } else if( action == "PAGE_DOWN" ) {
        select( sel_response + RESPONSES_PAGE );
    } else if( view != nullptr && action == "SCROLL_INFOBOX_UP" ) {
        view->scroll_history( rml_ui::scroll::line_up );
    } else if( view != nullptr && action == "SCROLL_INFOBOX_DOWN" ) {
        view->scroll_history( rml_ui::scroll::line_down );
    } else if( view != nullptr && action == "MOUSE_MOVE" ) {
        // Hovering picks a response, without scrolling to it
        const int row = view->hovered_index( "data-row" );
        if( row >= 0 && row < count ) {
            sel_response = row;
        }
    } else if( view != nullptr && action == "SELECT" ) {
        const std::string clicked = view->resolve_click( action );
        const int row = view->hovered_index( "data-row" );
        if( clicked != action ) {
            // An action, or the close button
            action = clicked;
        } else if( row >= 0 && row < count ) {
            sel_response = row;
            action = "CONFIRM";
        }
    }
    // The mouse wheel scrolls the history or the responses by itself
}

void dialogue_window::set_up_scrolling( input_context &ctxt ) const
{
    if( !is_computer && !is_not_conversation ) {
        ctxt.register_action( "LOOK_AT" );
        ctxt.register_action( "SIZE_UP_STATS" );
        ctxt.register_action( "ASSESS_PERSONALITY" );
        ctxt.register_action( "YELL" );
        ctxt.register_action( "CHECK_OPINION" );
    }
    ctxt.register_action( "SCROLL_INFOBOX_UP" );
    ctxt.register_action( "SCROLL_INFOBOX_DOWN" );
    ctxt.register_updown();
    ctxt.register_action( "END" );
    ctxt.register_action( "HOME" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SCROLL_DOWN" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SELECT" );
}

void dialogue_window::add_to_history( const std::string &text, const std::string &speaker_name,
                                      nc_color speaker_color )
{
    add_to_history( speaker_name, speaker_color );
    add_to_history( text );
}

void dialogue_window::add_to_history( const std::string &text )
{
    add_to_history( text, default_color() );
}

void dialogue_window::add_to_history( const std::string &text, nc_color color )
{
    history.emplace_back( color, text );
    ++num_lines_highlighted;
    history_changed = true;
}

void dialogue_window::add_history_separator()
{
    add_to_history( "", default_color() );
}

void dialogue_window::clear_history_highlights()
{
    num_lines_highlighted = 0;
}

nc_color dialogue_window::default_color() const
{
    return is_computer ? c_green : c_white;
}

void dialogue_window::set_responses( const std::vector<talk_data> &new_responses )
{
    responses = new_responses;
    sel_response = std::clamp( sel_response, 0, std::max( 0,
                               static_cast<int>( responses.size() ) - 1 ) );
    reveal_response = true;
}

void dialogue_window::set_responses_debug( const std::vector<std::string> &new_responses )
{
    responses_debug = new_responses;
}

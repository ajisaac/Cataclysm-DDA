#include "messages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>

#include "cached_options.h"
#include "calendar.h"
#include "catacharset.h"
#include "character.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "enums.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "input_context.h"
#include "json.h"
#include "map.h"
#include "options.h"
#include "output.h"
#include "panels.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_ui.h"
#include "rng.h"
#include "sdltiles.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "viewer.h"

#if defined(__ANDROID__)
#include <SDL_keyboard.h>
#endif

static const efftype_id effect_weed_high( "weed_high" );

namespace
{

struct game_message {
    std::string       message;
    time_point timestamp_in_turns = calendar::turn_zero;
    int               timestamp_in_user_actions = 0; // NOLINT(cata-serialize)
    int               count = 1;
    // number of times this message has been seen while it was in cooldown.
    unsigned cooldown_seen = 1; // NOLINT(cata-serialize)
    // hide the message, because at some point it was in cooldown period.
    bool cooldown_hidden = false; // NOLINT(cata-serialize)
    game_message_type type = m_neutral;

    game_message() = default;
    game_message( std::string &&msg, game_message_type const t ) :
        message( std::move( msg ) ),
        timestamp_in_turns( calendar::turn ),
        timestamp_in_user_actions( g->get_user_action_counter() ),
        type( t ) {
    }

    const time_point &turn() const {
        return timestamp_in_turns;
    }

    std::string get_with_count() const {
        if( count <= 1 ) {
            return message;
        }
        //~ Message %s on the message log was repeated %d times, e.g. "You hear a whack! x 12"
        return string_format( _( "%s x %d" ), message, count );
    }

    /** Get whether or not a message should not be displayed (hidden) in the side bar because it's in a cooldown period.
     * @returns `true` if the message should **not** be displayed, `false` otherwise.
     */
    bool is_in_cooldown() const {
        return cooldown_hidden;
    }

    bool is_new( const time_point &current ) const {
        return turn() >= current;
    }

    bool is_recent( const time_point &current ) const {
        return turn() + 5_turns >= current;
    }

    nc_color get_color( const time_point &current ) const {
        if( is_new( current ) ) {
            // color for new messages
            return msgtype_to_color( type, false );

        } else if( is_recent( current ) ) {
            // color for slightly old messages
            return msgtype_to_color( type, true );
        }

        // color for old messages
        return c_dark_gray;
    }

    void deserialize( const JsonObject &obj ) {
        obj.read( "turn", timestamp_in_turns );
        message = obj.get_string( "message" );
        count = obj.get_int( "count" );
        type = static_cast<game_message_type>( obj.get_int( "type" ) );
    }

    void serialize( JsonOut &jsout ) const {
        jsout.start_object();
        jsout.member( "turn", timestamp_in_turns );
        jsout.member( "message", message );
        jsout.member( "count", count );
        jsout.member( "type", static_cast<int>( type ) );
        jsout.end_object();
    }
};

class messages_impl
{
    public:
        std::deque<game_message> messages;   // Messages to be printed
        std::vector<game_message> cooldown_templates; // Message cooldown
        time_point curmes = calendar::turn_zero; // The last-seen message.
        bool active = true;

        bool has_undisplayed_messages() const {
            return !messages.empty() && messages.back().turn() > curmes;
        }

        const game_message &history( const int i ) const {
            return messages[messages.size() - i - 1];
        }

        // coalesce recent like messages
        bool coalesce_messages( const game_message &m ) {
            if( messages.empty() ) {
                return false;
            }

            auto &last_msg = messages.back();
            if( last_msg.turn() + 3_turns < calendar::turn ) {
                return false;
            }

            if( m.type != last_msg.type || m.message != last_msg.message ) {
                return false;
            }

            // update the cooldown message timer due to coalescing
            const auto cooldown_it = std::find_if( cooldown_templates.begin(), cooldown_templates.end(),
            [&m]( game_message & am ) -> bool {
                return m.message == am.message;
            } );
            if( cooldown_it != cooldown_templates.end() ) {
                cooldown_it->timestamp_in_turns = calendar::turn;
            }

            // coalesce messages
            last_msg.count++;
            last_msg.timestamp_in_turns = calendar::turn;
            last_msg.timestamp_in_user_actions = g->get_user_action_counter();
            last_msg.type = m.type;

            return true;
        }

        void add_msg_string( std::string &&msg ) {
            add_msg_string( std::move( msg ), m_neutral, gmf_none );
        }

        void add_msg_string( std::string &&msg, const game_message_params &params ) {
            add_msg_string( std::move( msg ), params.type, params.flags );
        }

        void add_msg_string( std::string &&msg, game_message_type const type,
                             const game_message_flags flags ) {
            if( msg.empty() || !active ) {
                return;
            }

            if( type == m_debug && !debug_mode ) {
                return;
            }

            modify_msg_with_exclamations( msg, type );

            game_message m = game_message( std::move( msg ), type );

            refresh_cooldown( m, flags );
            hide_message_in_cooldown( m );

            if( coalesce_messages( m ) ) {
                return;
            }

            unsigned int message_limit = get_option<int>( "MESSAGE_LIMIT" );
            while( messages.size() > message_limit ) {
                messages.pop_front();
            }

            messages.emplace_back( m );
        }

        /** Check if the current message needs to be prevented (hidden) or not from being displayed in the side bar.
         * @param message The message to be checked.
         */
        void hide_message_in_cooldown( game_message &message ) {
            message.cooldown_hidden = false;

            if( message_cooldown <= 0 || message.turn() <= calendar::turn_zero ) {
                return;
            }

            // We look for **exactly the same** message string in the cooldown templates
            // If there is one, this means the same message was already displayed.
            const auto cooldown_it = std::find_if( cooldown_templates.begin(), cooldown_templates.end(),
            [&message]( game_message & m_cooldown ) -> bool {
                return m_cooldown.message == message.message;
            } );
            if( cooldown_it == cooldown_templates.end() ) {
                // nothing found, not in cooldown.
                return;
            }

            // note: from this point the current message (`message`) has the same string than one of the active cooldown template messages (`cooldown_it`).

            // check how much times this message has been seen during its cooldown.
            // If it's only one time, then no need to hide it.
            if( cooldown_it->cooldown_seen == 1 ) {
                return;
            }

            // check if it's the message that started the cooldown timer.
            if( message.turn() == cooldown_it->turn() ) {
                return;
            }

            // current message turn.
            const int cm_turn = to_turn<int>( message.turn() );
            // maximum range of the cooldown timer.
            const int max_cooldown_range = to_turn<int>( cooldown_it->turn() ) + message_cooldown;
            // If the current message is in the cooldown range then hide it.
            if( cm_turn <= max_cooldown_range ) {
                message.cooldown_hidden = true;
            }
        }

        void modify_msg_with_exclamations( std::string &msg, game_message_type const type ) {
            // add smileys to messages if the player is high on weed
            Character *p_char = &get_player_character();
            if( !p_char->has_effect( effect_weed_high ) ) {
                return;
            }

            const time_duration weed_dur = p_char->get_effect_dur( effect_weed_high );
            const float in_minutes = to_minutes<float>( weed_dur );
            // we will now calculate how often smiley faces should appear in messages.
            // Slowly rises. 0 at 0, 1 at 22.5, 2 at 45 mins. Asymptote at 3.
            const float max_per_msg = 3 - 3 / ( 1 + 0.001 * in_minutes * in_minutes );
            //possible values for smiles_for_this_msg are 0,1,2,3.
            int smiles_for_this_msg = static_cast<int>( round( rng_normal( 0, max_per_msg ) ) );

            // now insert the smiley face somewhere in the msg string. We want to avoid breaking up words, so we will only replace spaces.
            // find all suitable locations for smiley. That is, end of message + all spaces (except after certain words)
            std::vector<size_t> smiley_locations;
            // we also need to track space locations to extract words properly. Annoying but necessary.
            std::vector<size_t> space_locations;

            // end of msg
            smiley_locations.push_back( msg.size() );
            // now all spaces
            for( size_t j = 0; j < msg.size(); j++ ) {
                if( msg[j] == ' ' ) {
                    bool wordskip = false;
                    // fancy logic. It's weird if the smiley comes after "to" or "that" or "a". Let's try and skip words like those.
                    // We skip all short words (<=3 letters), and all words in the forbidden_words array.
                    if( !space_locations.empty() ) {
                        const size_t prev_space = space_locations.back();
                        // if prev_space is the previous char, skip
                        if( prev_space == j - 1 ) {
                            wordskip = true;
                        }
                        // check that smiley_locations.back() did not return none
                        if( prev_space != std::string::npos ) {
                            const std::string word = msg.substr( prev_space, j - prev_space );
                            const std::array<std::string, 6> forbidden_words = { " that", " with", " this", " over", " your", " onto" };
                            for( const std::string &forbidden_word : forbidden_words ) {
                                if( word == forbidden_word ) {
                                    wordskip = true;
                                    break;
                                }
                            }
                            if( word.size() > 1 && word.size() <= 4 ) {
                                wordskip = true;
                            }
                        }
                    } else {
                        // skip very first word
                        wordskip = true;
                    }
                    space_locations.push_back( j );
                    if( !wordskip ) {
                        smiley_locations.push_back( j );
                    }
                }
            }
            // first, check if smiles_for_this_msg == len(smiley_locations)
            if( smiles_for_this_msg >= static_cast<int>( smiley_locations.size() ) ) {
                smiles_for_this_msg = static_cast<int>( smiley_locations.size() ) - 1;
            }
            // then pick one at random
            for( int i = 0; i < smiles_for_this_msg; i++ ) {
                const size_t smiley_location = random_entry_removed( smiley_locations );
                std::string smiley_string = " :)";
                if( type == m_good || type == m_critical ) {
                    if( one_in( 5 ) ) {
                        smiley_string = " :D";
                    }
                }
                msg.insert( smiley_location, smiley_string );
                //for( size_t j = 0; j < smiley_locations.size(); j++ ) {
                // modernize-loop-convert. Use range-based for loop instead.
                for( size_t &smil_loc : smiley_locations ) {
                    if( smil_loc > smiley_location ) {
                        // we increment to account for the insertion of the smiley string
                        smil_loc += 3;
                    }
                }
            }

            // if we are *very* high, there will be a chance to finish a long message with an exclamation
            if( smiles_for_this_msg > 1 ) {
                int woah_chance = static_cast<int>( 10 / smiles_for_this_msg );
                if( msg.size() > 16 && one_in( woah_chance ) ) {
                    std::vector<std::string> exclamations = {
                        "Woah.", "Dude."
                    };
                    std::vector<std::string> good_exclamations = {
                        "Cool!", "Wicked!", "Awesome!"
                    };
                    std::vector<std::string> bad_exclamations = {
                        "Not cool.", "Oof.", "Bummer."
                    };
                    if( type == m_good || type == m_critical || type == m_headshot ) {
                        msg += " " + random_entry( good_exclamations );
                    } else if( type == m_bad || type == m_grazing ) {
                        msg += " " + random_entry( bad_exclamations );
                    } else {
                        msg += " " + random_entry( exclamations );
                    }
                }
            }
        }

        std::vector<std::pair<std::string, std::string>> recent_messages( size_t count ) const {
            count = std::min( count, messages.size() );

            std::vector<std::pair<std::string, std::string>> result;
            result.reserve( count );

            const int offset = static_cast<std::ptrdiff_t>( messages.size() - count );

            std::transform( begin( messages ) + offset, end( messages ), back_inserter( result ),
            []( const game_message & msg ) {
                return std::make_pair( to_string_time_of_day( msg.timestamp_in_turns ),
                                       msg.get_with_count() );
            } );

            return result;
        }

        /** Refresh the cooldown timers, removing elapsed ones and making new ones if needed.
         * @param message The current message that needs to be checked.
         * @param flags Flags pertaining to the message.
         */
        void refresh_cooldown( const game_message &message, const game_message_flags flags ) {
            // is cooldown used? (also checks for messages arriving here at game initialization: we don't care about them).
            if( message_cooldown <= 0 || message.turn() <= calendar::turn_zero ) {
                return;
            }

            // housekeeping: remove any cooldown message with an expired cooldown time from the cooldown queue.
            const time_point now = calendar::turn;
            for( auto it = cooldown_templates.begin(); it != cooldown_templates.end(); ) {
                // number of turns elapsed since the cooldown started.
                const int turns = to_turns<int>( now - it->turn() );
                if( turns >= message_cooldown ) {
                    // time elapsed! remove it.
                    it = cooldown_templates.erase( it );
                } else {
                    ++it;
                }
            }

            // do not hide messages which bypasses cooldown.
            if( ( flags & gmf_bypass_cooldown ) != 0 ) {
                return;
            }

            // Is the message string already in the cooldown queue?
            // If it's not we must put it in the cooldown queue now, otherwise just increment the number of times we have seen it.
            const auto cooldown_message_it = std::find_if( cooldown_templates.begin(),
            cooldown_templates.end(), [&message]( game_message & cooldown_message ) -> bool {
                return cooldown_message.message == message.message;
            } );
            if( cooldown_message_it == cooldown_templates.end() ) {
                // push current message to cooldown message templates.
                cooldown_templates.emplace_back( message );
            } else {
                // increment the number of time we have seen this message.
                cooldown_message_it->cooldown_seen++;
            }
        }
};

// Messages object.
messages_impl player_messages;

bool message_exceeds_ttl( const game_message &message )
{
    return message_ttl > 0 &&
           message.timestamp_in_user_actions + message_ttl <= g->get_user_action_counter();
}

} //namespace

std::vector<std::pair<std::string, std::string>> Messages::recent_messages( const size_t count )
{
    return player_messages.recent_messages( count );
}

bool Messages::has_debug_filter( debugmode::debug_filter type )
{
    return debug_mode && debugmode::enabled_filters.count( type ) == 1;
}

void Messages::serialize( JsonOut &json )
{
    json.member( "player_messages" );
    json.start_object();
    json.member( "messages", player_messages.messages );
    json.member( "curmes", player_messages.curmes );
    json.end_object();
}

void Messages::deserialize( const JsonObject &json )
{
    if( !json.has_member( "player_messages" ) ) {
        return;
    }

    JsonObject obj = json.get_object( "player_messages" );
    obj.read( "messages", player_messages.messages );
    obj.read( "curmes", player_messages.curmes );
}

void Messages::add_msg( std::string msg )
{
    player_messages.add_msg_string( std::move( msg ) );
}

void Messages::add_msg( const game_message_params &params, std::string msg )
{
    player_messages.add_msg_string( std::move( msg ), params );
}

void Messages::clear_messages()
{
    player_messages.messages.clear();
    player_messages.active = true;
}

void Messages::deactivate()
{
    player_messages.active = false;
}

size_t Messages::size()
{
    return player_messages.messages.size();
}

bool Messages::has_undisplayed_messages()
{
    return player_messages.has_undisplayed_messages();
}

// Returns pairs of message log type id and untranslated name
static const std::vector<std::pair<game_message_type, const char *>> &msg_type_and_names()
{
    static const std::vector<std::pair<game_message_type, const char *>> type_n_names = {
        { m_good, translate_marker_context( "message type", "good" ) },
        { m_bad, translate_marker_context( "message type", "bad" ) },
        { m_mixed, translate_marker_context( "message type", "mixed" ) },
        { m_warning, translate_marker_context( "message type", "warning" ) },
        { m_info, translate_marker_context( "message type", "info" ) },
        { m_neutral, translate_marker_context( "message type", "neutral" ) },
        { m_debug, translate_marker_context( "message type", "debug" ) },
    };
    return type_n_names;
}

// Get message type from translated name, returns true if name is a valid translated name
static bool msg_type_from_name( game_message_type &type, const std::string &name )
{
    for( const auto &p : msg_type_and_names() ) {
        if( name == pgettext( "message type", p.second ) ) {
            type = p.first;
            return true;
        }
    }
    return false;
}

namespace Messages
{
namespace
{
// The message log screen's document. The dialog keeps the state and handles
// the input, and pushes what to show here before each redraw.
class message_log_rml : public rml_ui::document
{
    public:
        message_log_rml() : rml_ui::document( "message_log.rml" ) {}

        // Show this content in the element with this id; returns whether it changed
        bool show( const std::string &id, const std::string &rml ) {
            std::string &last = shown[id];
            if( last == rml ) {
                return false;
            }
            last = rml;
            set_inner_rml( id, rml );
            return true;
        }

        void mark( const std::string &id, const std::string &name, bool active ) {
            set_class( id, name, active );
        }

        void place( const std::string &style ) {
            if( style != panel_style ) {
                panel_style = style;
                set_attribute( "panel", "style", style );
            }
        }

        // Scroll the list, after laying it out if its content was just set
        void scroll_list( rml_ui::scroll how, bool after_layout = false ) {
            if( after_layout ) {
                update_layout();
            }
            scroll( "list", how );
        }

        void start_filter( const std::string &text ) {
            set_value( "filter", text );
            focus( "filter" );
        }

        void stop_filter() {
            blur( "filter" );
        }

        std::optional<std::string> filter_text() const {
            return get_value( "filter" );
        }

    private:
        std::map<std::string, std::string> shown;
        std::string panel_style;
};

// NOLINTNEXTLINE(cata-xy)
class dialog
{
    public:
        dialog();
        void run();
    private:
        void show( message_log_rml &doc ) const;
        std::string list_rml() const;
        void input( message_log_rml &doc );
        void do_filter( const std::string &filter_str );
        static std::string filter_help_text();

        const nc_color time_color;
        const nc_color filter_help_color;

        bool filtering = false;
        std::string filter_str;

        input_context ctxt;
        // For typing a filter, which the document's text field edits
        input_context filter_ctxt;

        // Indices into the history of the messages shown, in display order,
        // and their RML, rebuilt only when the filter changes
        std::vector<size_t> filtered;
        std::string list;

        bool canceled = false;
};
} // namespace
} // namespace Messages

Messages::dialog::dialog()
    : time_color( c_light_blue ), filter_help_color( c_cyan ),
      ctxt( "MESSAGE_LOG" ), filter_ctxt( "STRING_INPUT", keyboard_mode::keychar )
{
    ctxt.register_action( "UP", to_translation( "Scroll up" ) );
    ctxt.register_action( "DOWN", to_translation( "Scroll down" ) );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );

    filter_ctxt.register_action( "TEXT.CONFIRM" );
    filter_ctxt.register_action( "TEXT.QUIT" );
}

std::string Messages::dialog::list_rml() const
{
    std::vector<std::string> times;
    times.reserve( filtered.size() );
    for( const size_t msg_ind : filtered ) {
        const time_point msg_time = player_messages.history( msg_ind ).timestamp_in_turns;
        times.push_back( to_string_clipped( calendar::turn - msg_time, clipped_align::right ) );
    }

    // A time shows on the newest message of a run at that time, as the old
    // log printed from new to old; the rest of the run are marked beside it
    std::string rml;
    for( size_t i = 0; i < filtered.size(); ++i ) {
        // Wrapping below 0 lands past the end, which the size checks reject
        const size_t newer = log_from_top ? i - 1 : i + 1;
        const size_t older = log_from_top ? i + 1 : i - 1;
        const bool has_newer = newer < filtered.size() && times[newer] == times[i];
        const bool has_older = older < filtered.size() && times[older] == times[i];

        const game_message &msg = player_messages.history( filtered[i] );
        rml += has_newer || has_older ? "<div class=\"msg in-run\"><div class=\"time\">" :
               "<div class=\"msg\"><div class=\"time\">";
        if( !has_newer ) {
            rml += rml_ui::markup_to_rml( times[i], time_color );
        }
        rml += "</div><div class=\"text\">" +
               rml_ui::markup_to_rml( msg.get_with_count(), msgtype_to_color( msg.type, false ) ) +
               "</div></div>";
    }
    return rml;
}

void Messages::dialog::show( message_log_rml &doc ) const
{
    // Between the sidebars where there is room, as the old window was
    const int left_panel_width = panel_manager::get_manager().get_width_left();
    const int right_panel_width = panel_manager::get_manager().get_width_right();
    int w_x = std::max( left_panel_width, right_panel_width );
    int w_width = TERMX - 2 * w_x;
    if( w_width < TERMY * 3 ) {
        // try not to obscure sidebar
        w_x = left_panel_width;
        w_width = TERMX - left_panel_width - right_panel_width;
    }
    const float scale = rml_ui::display_buffer_scale();
    const window_dimensions dim = get_window_dimensions( point( w_x, 0 ), point( w_width, TERMY ) );
    doc.place( string_format( "left: %.0fpx; top: %.0fpx; width: %.0fpx; height: %.0fpx;",
                              dim.window_pos_pixel.x * scale, dim.window_pos_pixel.y * scale,
                              dim.window_size_pixel.x * scale, dim.window_size_pixel.y * scale ) );

    // A new list starts at its newest end
    if( doc.show( "list", list ) ) {
        doc.scroll_list( log_from_top ? rml_ui::scroll::top : rml_ui::scroll::bottom, true );
    }

    doc.mark( "help", "hidden", !filtering );
    doc.mark( "filter-row", "hidden", !filtering );
    if( filtering ) {
        doc.show( "help", rml_ui::markup_to_rml( filter_help_text(), filter_help_color ) );
        doc.show( "footer", rml_ui::markup_to_rml( string_format(
                      _( "%s or %s to finish filtering" ), filter_ctxt.get_desc( "TEXT.CONFIRM" ),
                      filter_ctxt.get_desc( "TEXT.QUIT" ) ) ) );
    } else if( filter_str.empty() ) {
        doc.show( "footer", rml_ui::markup_to_rml( string_format(
                      _( "< Press %s to filter, %s to reset >" ),
                      ctxt.get_desc( "FILTER" ), ctxt.get_desc( "RESET_FILTER" ) ) ) );
    } else {
        doc.show( "footer", "&lt; " + rml_ui::escape_rml( filter_str ) + " &gt;" );
    }
}

void Messages::dialog::do_filter( const std::string &filter_str )
{
    // Split the search string into type and text
    bool has_type_filter = false;
    game_message_type filter_type = m_neutral;
    std::string filter_text;
    const size_t colon = filter_str.find( ':' );
    if( colon != std::string::npos ) {
        has_type_filter = msg_type_from_name( filter_type, filter_str.substr( 0, colon ) );
        filter_text = filter_str.substr( colon + 1 );
    } else {
        filter_text = filter_str;
    }

    // The newest first if the log runs from the top, else the oldest
    filtered.clear();
    const size_t msg_count = size();
    for( size_t ind = 0; ind < msg_count; ++ind ) {
        const size_t msg_ind = log_from_top ? ind : msg_count - 1 - ind;
        const game_message &msg = player_messages.history( msg_ind );
        if( ( !has_type_filter || filter_type == msg.type ) &&
            ci_find_substr( remove_color_tags( msg.get_with_count() ), filter_text ) >= 0 ) {
            filtered.push_back( msg_ind );
        }
    }
    list = list_rml();
}

void Messages::dialog::input( message_log_rml &doc )
{
    canceled = false;
    if( filtering ) {
        const std::string action = filter_ctxt.handle_input();
        // RmlUi has already applied any typing to the field, and the list
        // follows it as it changes
        const std::optional<std::string> text = doc.filter_text();
        if( text && *text != filter_str ) {
            filter_str = *text;
            do_filter( filter_str );
        }
        if( action == "TEXT.CONFIRM" || action == "TEXT.QUIT" ) {
            filtering = false;
            doc.stop_filter();
        }
    } else {
        const std::string &action = ctxt.handle_input();
        // The wheel scrolls the list by itself, in RmlUi
        if( action == "DOWN" ) {
            doc.scroll_list( rml_ui::scroll::line_down );
        } else if( action == "UP" ) {
            doc.scroll_list( rml_ui::scroll::line_up );
        } else if( action == "PAGE_DOWN" ) {
            doc.scroll_list( rml_ui::scroll::page_down );
        } else if( action == "PAGE_UP" ) {
            doc.scroll_list( rml_ui::scroll::page_up );
        } else if( action == "FILTER" ) {
            filtering = true;
            doc.start_filter( filter_str );
        } else if( action == "RESET_FILTER" ) {
            filter_str.clear();
            do_filter( filter_str );
        } else if( action == "QUIT" ) {
            canceled = true;
        }
    }
}

void Messages::dialog::run()
{
    message_log_rml doc;
    do_filter( filter_str );
    while( !canceled ) {
        show( doc );
        ui_manager::redraw();
        input( doc );
    }
}

std::string Messages::dialog::filter_help_text()
{
    const char *const &help_fmt = _(
                                      "<color_light_gray>The default is to search the entire message log.  "
                                      "Use message-types as prefixes followed by (:) to filter more specific.\n"
                                      "Valid message-type values are:</color> %s\n"
                                      "\n"
                                      "<color_white>Examples:</color>\n"
                                      "  <color_light_green>good</color><color_white>:mutation\n"
                                      "  :you pick up: 1</color>\n"
                                      "  <color_light_red>bad</color><color_white>:</color>\n"
                                      "\n"
                                  );
    std::string type_text;
    const auto &type_list = msg_type_and_names();
    for( auto it = type_list.begin(); it != type_list.end(); ++it ) {
        // Skip m_debug outside debug mode (but allow searching for it)
        if( debug_mode || it->first != m_debug ) {
            const std::string &col_name = get_all_colors().get_name( msgtype_to_color( it->first ) );
            auto next_it = std::next( it );
            // Skip m_debug outside debug mode
            if( !debug_mode && next_it != type_list.end() && next_it->first == m_debug ) {
                next_it = std::next( next_it );
            }
            if( next_it != type_list.end() ) {
                //~ the 2nd %s is a type name, this is used to format a list of type names
                type_text += string_format( pgettext( "message log", "<color_%1$s>%2$s</color>, " ),
                                            col_name, pgettext( "message type", it->second ) );
            } else {
                //~ the 2nd %s is a type name, this is used to format the last type name in a list of type names
                type_text += string_format( pgettext( "message log", "<color_%1$s>%2$s</color>." ),
                                            col_name, pgettext( "message type", it->second ) );
            }
        }
    }
    return string_format( help_fmt, type_text );
}

void Messages::display_messages()
{
    dialog dlg;
    dlg.run();
    player_messages.curmes = calendar::turn;
}

std::vector<std::string> Messages::sidebar_markup( const size_t max_messages )
{
    std::vector<std::string> lines;
    for( int i = static_cast<int>( size() ) - 1; i >= 0 && lines.size() < max_messages; --i ) {
        const game_message &m = player_messages.messages[i];
        if( message_exceeds_ttl( m ) ) {
            break;
        }
        // As in display_messages(), which only skips these when drawing upwards
        if( !log_from_top && m.is_in_cooldown() ) {
            continue;
        }
        std::string message_text = m.get_with_count();
        if( !m.is_recent( player_messages.curmes ) ) {
            message_text = remove_color_tags( message_text );
        }
        lines.push_back( colorize( message_text, m.get_color( player_messages.curmes ) ) );
    }
    return lines;
}

void Messages::display_messages( const catacurses::window &ipk_target, const int left,
                                 const int top, const int right, const int bottom )
{
    if( !size() ) {
        return;
    }

    const int maxlength = right - left;
    int line = log_from_top ? top : bottom;

    if( log_from_top ) {
        for( int i = size() - 1; i >= 0; --i ) {
            if( line > bottom ) {
                break;
            }

            const game_message &m = player_messages.messages[i];
            if( message_exceeds_ttl( m ) ) {
                break;
            }

            const nc_color col = m.get_color( player_messages.curmes );
            std::string message_text = m.get_with_count();
            if( !m.is_recent( player_messages.curmes ) ) {
                message_text = remove_color_tags( message_text );
            }

            for( const std::string &folded : foldstring( message_text, maxlength ) ) {
                if( line > bottom ) {
                    break;
                }
                // Redrawing line to ensure new messages similar to previous
                // messages will not be missed by screen readers
                wredrawln( ipk_target, line, 1 );
                nc_color col_out = col;
                print_colored_text( ipk_target, point( left, line++ ), col_out, col, folded );
            }
        }
    } else {
        for( int i = size() - 1; i >= 0; --i ) {
            if( line < top ) {
                break;
            }

            const game_message &m = player_messages.messages[i];
            if( message_exceeds_ttl( m ) ) {
                break;
            }

            if( m.is_in_cooldown() ) {
                // message is still (or was at some point) into a cooldown period.
                continue;
            }

            const nc_color col = m.get_color( player_messages.curmes );
            std::string message_text = m.get_with_count();
            if( !m.is_recent( player_messages.curmes ) ) {
                message_text = remove_color_tags( message_text );
            }

            const auto folded_strings = foldstring( message_text, maxlength );
            const auto folded_rend = folded_strings.rend();
            for( auto string_iter = folded_strings.rbegin();
                 string_iter != folded_rend && line >= top; ++string_iter, line-- ) {
                // Redrawing line to ensure new messages similar to previous
                // messages will not be missed by screen readers
                wredrawln( ipk_target, line, 1 );
                nc_color col_out = col;
                print_colored_text( ipk_target, point( left, line ), col_out, col, *string_iter );
            }
        }
    }

    player_messages.curmes = calendar::turn;
}

void add_msg( std::string msg )
{
    Messages::add_msg( std::move( msg ) );
}

void add_msg( const game_message_params &params, std::string msg )
{
    Messages::add_msg( params, std::move( msg ) );
}

// msg argument will not expand name place holders
void add_msg_if_player_sees( const tripoint_bub_ms &target, std::string msg )
{
    const map &here = get_map();

    if( get_player_view().sees( here, target ) ) {
        Messages::add_msg( std::move( msg ) );
    }
}

void add_msg_if_player_sees( const Creature &target, std::string msg )
{
    const map &here = get_map();

    if( get_player_view().sees( here, target ) ) {
        Messages::add_msg( std::move( msg ) );
    }
}

void add_msg_if_player_sees( const tripoint_bub_ms &target, const game_message_params &params,
                             std::string msg )
{
    const map &here = get_map();

    if( get_player_view().sees( here, target ) ) {
        Messages::add_msg( params, std::move( msg ) );
    }
}

void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                             std::string msg )
{
    const map &here = get_map();

    if( get_player_view().sees( here, target ) ) {
        Messages::add_msg( params, std::move( msg ) );
    }
}

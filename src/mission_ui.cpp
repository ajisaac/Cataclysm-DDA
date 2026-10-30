#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "color.h"
#include "dialogue.h"
#include "dialogue_helpers.h"
#include "display.h"
#include "game_constants.h"
#include "input_context.h"
#include "line.h"
#include "mission.h"
#include "npc.h"
#include "faction.h"
#include "output.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "talker.h"
#include "timed_event.h"
#include "translation.h"
#include "translations.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"

static const dimension_id dimension_world_default( "default" );

static const faction_id faction_no_faction( "no_faction" );

namespace
{
enum class mission_ui_tab_enum : int {
    ACTIVE = 0,
    COMPLETED,
    FAILED,
    POINTS_OF_INTEREST,
    num_tabs
};
} // namespace

static mission_ui_tab_enum &operator++( mission_ui_tab_enum &c )
{
    c = static_cast<mission_ui_tab_enum>( static_cast<int>( c ) + 1 );
    if( c == mission_ui_tab_enum::num_tabs ) {
        c = static_cast<mission_ui_tab_enum>( 0 );
    }
    return c;
}

static mission_ui_tab_enum &operator--( mission_ui_tab_enum &c )
{
    if( c == static_cast<mission_ui_tab_enum>( 0 ) ) {
        c = mission_ui_tab_enum::num_tabs;
    }
    c = static_cast<mission_ui_tab_enum>( static_cast<int>( c ) - 1 );
    return c;
}

namespace
{
// Kept between visits to the screen, as it always was
int selected_mission = 0;

class mission_ui_impl : public rml_ui::window
{
    public:
        mission_ui_impl();
        void run();

    protected:
        void on_redraw() override;

    private:
        // Fill the entries of the selected tab from the avatar
        void refresh_entries();
        int entry_count() const;
        void switch_tab( mission_ui_tab_enum tab );

        std::string list_rml() const;
        std::string mission_description_rml( mission *miss );
        std::string point_of_interest_description_rml( const point_of_interest &poi ) const;
        std::string label_with_value_rml( const std::string &label, const std::string &value ) const;
        std::string location_rml( const std::string &label, const tripoint_abs_omt &loc ) const;

        input_context ctxt;
        mission_ui_tab_enum selected_tab = mission_ui_tab_enum::ACTIVE;
        std::vector<mission *> umissions;
        std::vector<point_of_interest> upoints_of_interest;
        // Bring the selected entry into view, and the description to its top
        bool reveal_selected = true;

        // We do this to avoid replacing expanded snippets with other valid snippet text on redraw. The raw description includes un-parsed snippet tags.
        // The parsed description is, obviously, after they've been parsed. So "<zombie_name>" becomes "undead thing" or "zed" or whatever.
        // Whenever the user changes which mission they're looking at, this will update to parse the description *once* and keep displaying that parsed version
        // until the mission changes again.
        std::string raw_description;
        std::string parsed_description;
};

mission_ui_impl::mission_ui_impl() :
    rml_ui::window( _( "Your missions" ) ),
    ctxt( "MISSION_UI" )
{
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "CONFIRM",
                          to_translation( "Set selected mission/point of interest as current objective" ) );
    ctxt.register_action( "DELETE_POINT_OF_INTEREST", to_translation( "Delete Point of Interest" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    // Smooths out our handling, makes tabs load immediately after input instead of waiting for next.
    ctxt.set_timeout( 10 );
}

void mission_ui_impl::refresh_entries()
{
    umissions.clear();
    upoints_of_interest.clear();
    switch( selected_tab ) {
        case mission_ui_tab_enum::ACTIVE:
            umissions = get_avatar().get_active_missions();
            break;
        case mission_ui_tab_enum::COMPLETED:
            umissions = get_avatar().get_completed_missions();
            break;
        case mission_ui_tab_enum::FAILED:
            umissions = get_avatar().get_failed_missions();
            break;
        case mission_ui_tab_enum::POINTS_OF_INTEREST:
            upoints_of_interest = get_avatar().get_points_of_interest();
            break;
        case mission_ui_tab_enum::num_tabs:
            break;
    }
    const int last_entry = std::max( 0, entry_count() - 1 );
    selected_mission = std::clamp( selected_mission, 0, last_entry );
}

int mission_ui_impl::entry_count() const
{
    return static_cast<int>( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ?
                             upoints_of_interest.size() : umissions.size() );
}

void mission_ui_impl::switch_tab( mission_ui_tab_enum tab )
{
    selected_tab = tab;
    selected_mission = 0;
    reveal_selected = true;
}

void mission_ui_impl::run()
{
    while( true ) {
        ui_manager::redraw_invalidated();

        std::string action = ctxt.handle_input();
        refresh_entries();
        if( action == "SELECT" ) {
            const int tab = hovered_tab();
            const int row = hovered_index( "data-row" );
            if( tab >= 0 ) {
                switch_tab( static_cast<mission_ui_tab_enum>( tab ) );
                continue;
            } else if( row >= 0 && row < entry_count() ) {
                selected_mission = row;
                continue;
            }
            action = resolve_click( action );
        }

        const int last_entry = std::max( 0, entry_count() - 1 );
        if( action == "QUIT" ) {
            break;
        } else if( action == "NEXT_TAB" || action == "RIGHT" ) {
            mission_ui_tab_enum tab = selected_tab;
            switch_tab( ++tab );
        } else if( action == "PREV_TAB" || action == "LEFT" ) {
            mission_ui_tab_enum tab = selected_tab;
            switch_tab( --tab );
        } else if( action == "PAGE_UP" ) {
            scroll( "description", rml_ui::scroll::page_up );
        } else if( action == "PAGE_DOWN" ) {
            scroll( "description", rml_ui::scroll::page_down );
        } else if( action == "UP" ) {
            selected_mission = selected_mission > 0 ? selected_mission - 1 : last_entry;
            reveal_selected = true;
        } else if( action == "DOWN" ) {
            selected_mission = selected_mission < last_entry ? selected_mission + 1 : 0;
            reveal_selected = true;
        } else if( action == "HOME" ) {
            selected_mission = 0;
            reveal_selected = true;
        } else if( action == "END" ) {
            selected_mission = last_entry;
            reveal_selected = true;
        } else if( action == "CONFIRM" ) {
            if( selected_tab == mission_ui_tab_enum::ACTIVE && !umissions.empty() ) {
                get_avatar().set_active_mission( *umissions[selected_mission] );
            } else if( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST &&
                       !upoints_of_interest.empty() ) {
                get_avatar().set_active_point_of_interest( upoints_of_interest[selected_mission] );
            }
        } else if( action == "DELETE_POINT_OF_INTEREST" &&
                   selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST && !upoints_of_interest.empty() ) {
            get_avatar().delete_point_of_interest( upoints_of_interest[selected_mission].pos );
        }
    }
}

std::string mission_ui_impl::list_rml() const
{
    std::string rml;
    for( int i = 0; i < entry_count(); i++ ) {
        const std::string name = selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ?
                                 upoints_of_interest[i].text : umissions[i]->name();
        rml += string_format( "<div id=\"entry-%d\" class=\"item%s\" data-row=\"%d\">", i,
                              i == selected_mission ? " selected" : "", i ) + rml_ui::escape_rml( name ) + "</div>";
    }
    return rml;
}

void mission_ui_impl::on_redraw()
{
    set_size( "70%", "80%" );
    refresh_entries();

    set_tabs( { _( "ACTIVE" ), _( "COMPLETED" ), _( "FAILED" ), _( "POINTS OF INTEREST" ) },
              static_cast<size_t>( selected_tab ) );

    if( entry_count() == 0 ) {
        static const std::map< mission_ui_tab_enum, translation > nope = {
            { mission_ui_tab_enum::ACTIVE, to_translation( "You have no active missions!" ) },
            { mission_ui_tab_enum::COMPLETED, to_translation( "You haven't completed any missions!" ) },
            { mission_ui_tab_enum::FAILED, to_translation( "You haven't failed any missions!" ) },
            { mission_ui_tab_enum::POINTS_OF_INTEREST, to_translation( "You don't have any points of interest.  Add those from the overmap." ) }
        };
        set_header( std::string() );
        set_body_scrolls( true );
        set_body( "<p>" + rml_ui::escape_rml( nope.at( selected_tab ).translated() ) + "</p>" );
        return;
    }

    if( get_avatar().get_active_mission() ) {
        set_header( "<p>" + rml_ui::escape_rml( string_format( _( "Current objective: %s" ),
                    get_avatar().get_active_mission()->name() ) ) + "</p>" );
    } else if( get_avatar().get_active_point_of_interest().pos != tripoint_abs_omt::invalid ) {
        set_header( "<p>" + rml_ui::escape_rml( string_format( _( "Current point of interest: %s" ),
                    get_avatar().get_active_point_of_interest().text ) ) + "</p>" );
    } else {
        set_header( std::string() );
    }

    // Missions selection is purposefully thinner than the description, it has less to convey.
    const std::string description = selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ?
                                    point_of_interest_description_rml( upoints_of_interest[selected_mission] ) :
                                    mission_description_rml( umissions[selected_mission] );
    set_body_scrolls( false );
    set_body( "<div class=\"panes\"><div id=\"entries\" class=\"pane\" style=\"flex: 2 1 0;\"><h3>" +
              rml_ui::escape_rml( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ?
                                  _( "Points of Interest" ) : _( "Missions" ) ) + "</h3>" + list_rml() +
              "</div><div id=\"description\" class=\"pane\" style=\"flex: 3 1 0;\"><h3>" +
              rml_ui::escape_rml( _( "Description" ) ) + "</h3>" + description + "</div></div>" );
    if( reveal_selected ) {
        reveal_selected = false;
        scroll_into_view( "entry-" + std::to_string( selected_mission ) );
        scroll( "description", rml_ui::scroll::top );
    }
}

std::string mission_ui_impl::label_with_value_rml( const std::string &label,
        const std::string &value ) const
{
    return "<div class=\"kv\"><div class=\"k\">" + rml_ui::escape_rml( label ) +
           "</div><div class=\"v\">" + rml_ui::colored_rml( value, c_light_gray ) + "</div></div>";
}

std::string mission_ui_impl::mission_description_rml( mission *miss )
{
    std::string rml = "<p>" + rml_ui::escape_rml( string_format( _( "Mission: %s" ),
                      miss->name() ) ) + "</p>";
    npc *mission_giver = nullptr;
    if( miss->get_npc_id().is_valid() ) {
        mission_giver = g->find_npc( miss->get_npc_id() );
        if( mission_giver ) {
            rml += label_with_value_rml( _( "Given by:" ), mission_giver->disp_name() );
            if( mission_giver->get_faction() && mission_giver->get_fac_id() != faction_no_faction ) {
                rml += label_with_value_rml( _( "Faction:" ), mission_giver->get_faction()->get_name() );
            }
            const tripoint_abs_omt npc_location = mission_giver->pos_abs_omt();
            rml += location_rml( _( "Map location:" ), npc_location );
        }
    }
    rml += "<div class=\"sep\"></div>";
    if( raw_description != miss->get_description() ) {
        raw_description = miss->get_description();
        parsed_description = raw_description;
        // Handles(example)  <reward_count:item>   in description
        // Not handled in parse_tags for some reason!
        dialogue d( get_talker_for( get_avatar() ), nullptr, {} );
        const talk_effect_fun_t::likely_rewards_t &rewards = miss->get_likely_rewards();
        for( const auto &reward : rewards ) {
            std::string token = "<reward_count:" + itype_id( reward.second.evaluate( d ) ).str() + ">";
            parsed_description = string_replace( parsed_description, token, string_format( "%g",
                                                 reward.first.evaluate( d ) ) );
        }
        const Character &other_talker = mission_giver ? *mission_giver : get_player_character();
        // parse_tags() modifies the argument string directly, no return value.
        parse_tags( parsed_description, get_player_character(), other_talker );
    }
    rml += "<div class=\"pre\">" + rml_ui::markup_to_rml( parsed_description ) + "</div>";
    if( miss->has_deadline() ) {
        const time_point deadline = miss->get_deadline();
        if( selected_tab == mission_ui_tab_enum::ACTIVE ) {
            rml += "<p>" + rml_ui::escape_rml( string_format( _( "Deadline: %s" ),
                                               to_string( deadline ) ) ) + "</p>";
            const time_duration remaining = deadline - calendar::turn;
            std::string remaining_time;
            if( remaining <= 0_turns ) {
                remaining_time = _( "None!" );
            } else if( get_player_character().has_watch() ) {
                remaining_time = to_string( remaining );
            } else {
                remaining_time = to_string_approx( remaining );
            }
            rml += "<p>" + rml_ui::escape_rml( string_format( _( "Time remaining: %s" ),
                                               remaining_time ) ) + "</p>";
        } else {
            const time_duration time_in_past = calendar::turn - deadline;
            std::string time_in_past_string;
            if( get_player_character().has_watch() ) {
                time_in_past_string = to_string( time_in_past );
            } else {
                time_in_past_string = to_string_approx( time_in_past );
            }
            if( deadline != calendar::turn_zero ) {
                if( selected_tab == mission_ui_tab_enum::COMPLETED ) {
                    //~The replaced string is a calendar date, such as "Year 1, May 12, 08:04:32"
                    rml += "<p>" + rml_ui::colored_rml( string_format( _( "Completed: %s" ),
                                                        to_string( deadline ) ), c_green ) + "</p>";
                } else if( selected_tab == mission_ui_tab_enum::FAILED ) {
                    //~The replaced string is a calendar date, such as "Year 1, May 12, 08:04:32"
                    rml += "<p>" + rml_ui::colored_rml( string_format( _( "Failed at: %s" ),
                                                        to_string( deadline ) ), c_red ) + "</p>";
                }
                //~The replaced string is a time duration, such as "12 hours", or "5 minutes"
                rml += "<p>" + rml_ui::escape_rml( string_format( _( "%s ago" ),
                                                   time_in_past_string ) ) + "</p>";
            }
        }
    }
    if( miss->has_target() ) {
        // TODO: target does not contain a z-component, targets are assumed to be on z=0
        rml += location_rml( _( "Target:" ), miss->get_target() );
    }
    dimension_id mission_dimension = miss->get_dimension();
    // If dimension isn't the default one
    if( mission_dimension != dimension_world_default ) {
        rml += label_with_value_rml( _( "Dimension:" ), mission_dimension.str() );
    }
    if( selected_tab == mission_ui_tab_enum::ACTIVE ) {
        rml += "<div class=\"gap\"></div>" + rml_ui::action_button_rml( ctxt, "CONFIRM",
                _( "Set as current objective" ) );
    }
    return rml;
}

std::string mission_ui_impl::point_of_interest_description_rml( const point_of_interest &poi )
const
{
    return "<p>" + rml_ui::escape_rml( string_format( _( "Point of Interest: %s" ), poi.text ) ) +
           "</p><div class=\"sep\"></div>" + location_rml( _( "Target:" ), poi.pos ) +
           "<div class=\"gap\"></div>" +
           rml_ui::action_button_rml( ctxt, "CONFIRM", _( "Set as current objective" ) ) +
           rml_ui::action_button_rml( ctxt, "DELETE_POINT_OF_INTEREST" );
}

std::string mission_ui_impl::location_rml( const std::string &label,
        const tripoint_abs_omt &loc ) const
{
    const tripoint_abs_omt pos = get_player_character().pos_abs_omt();
    std::string rml = label_with_value_rml( label, display::overmap_position_text( loc ) );
    if( !you_know_where_you_are() ) {
        // Don't display "Distance:" or direction arrow if we don't know where we are
        return rml;
    }
    int omt_distance = rl_dist( pos, loc );
    if( omt_distance > 0 ) {
        // One OMT is 24 tiles across, at 1x1 meters each, so we can simply do number of OMTs * 24
        units::length actual_distance = omt_distance * 24_meter;
        const std::string dir_arrow = direction_arrow( direction_from( pos.xy(), loc.xy() ) );
        //~Parenthesis is a real-world value for distance. Example string: "223 tiles (5.35km) ⇗"
        const std::string distance_str = string_format( _( "%1$d tiles (%2$s) %3$s" ),
                                         omt_distance, length_to_string_approx( actual_distance ), dir_arrow );
        rml += label_with_value_rml( _( "Distance:" ), distance_str );
    }
    return rml;
}
} // namespace

void game::list_missions()
{
    mission_ui_impl ui;
    ui.run();
}

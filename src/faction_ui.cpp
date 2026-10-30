#include "faction_ui.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "basecamp.h"
#include "calendar.h"
#include "character.h"
#include "character_id.h"
#include "color.h"
#include "coordinates.h"
#include "dialogue_chatbin.h"
#include "display.h"
#include "enum_traits.h"
#include "faction.h"
#include "faction_camp.h"
#include "game.h"
#include "input_context.h"
#include "line.h"
#include "localized_comparator.h"
#include "map.h"
#include "mission_companion.h"
#include "mod_manager.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "point.h"
#include "rml_markup.h"
#include "skill.h"
#include "stomach.h"
#include "string_formatter.h"
#include "talker.h" // IWYU pragma: keep
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "uilist.h"
#include "vitamin.h"

template <typename T> struct enum_traits;

static const faction_id faction_your_followers( "your_followers" );

static const flag_id json_flag_TWO_WAY_RADIO( "TWO_WAY_RADIO" );

template<>
struct enum_traits<tab_mode> {
    static constexpr tab_mode last = tab_mode::NUM_TABS;
    static constexpr tab_mode first = tab_mode::TAB_MYFACTION;
};

void faction_manager::display() const
{
    faction_ui ui;
    ui.execute();
}

static bool has_radio( const Character &guy )
{
    return guy.cache_has_item_with_flag( json_flag_TWO_WAY_RADIO, true );
}

// is physically close enough to contact the beta without much hassle
// mainly for followers
static bool can_contact( const Character &alpha, const Character &beta )
{
    const map &here = get_map();

    const bool too_far_omt = rl_dist( alpha.pos_abs_omt(), beta.pos_abs_omt() ) > 3;
    const bool see_each_other = alpha.sees( here, beta );
    return !too_far_omt || see_each_other ;
}

// opposite to previouis one, far enough to check for radio contact
static radio_contact_result can_radio_contact( const Character &alpha, const Character &beta )
{
    bool u_has_radio = has_radio( alpha );
    bool guy_has_radio = has_radio( beta );
    if( u_has_radio && guy_has_radio ) {
        if( !( alpha.posz() >= 0 && beta.posz() >= 0 ) &&
            !( alpha.posz() == beta.posz() ) ) {
            //Early exit
            return radio_contact_result::TOO_FAR;
        } else {
            // TODO: better range calculation than just elevation.
            const int base_range = 200;
            float send_elev_boost = ( 1 + ( alpha.posz() * 0.1 ) );
            float recv_elev_boost = ( 1 + ( beta.posz() * 0.1 ) );
            if( ( square_dist( alpha.pos_abs_sm(),
                               beta.pos_abs_sm() ) <= base_range * send_elev_boost * recv_elev_boost ) ) {
                //Direct radio contact, both of their elevation are in effect
                return radio_contact_result::YES;
            } else {
                //contact via camp radio tower
                int recv_range = base_range * recv_elev_boost;
                int send_range = base_range * send_elev_boost;
                const int radio_tower_boost = 5;
                // find camps that are near player or npc
                const std::vector<camp_reference> &camps_near_player = overmap_buffer.get_camps_near(
                            alpha.pos_abs_sm(), send_range * radio_tower_boost );
                const std::vector<camp_reference> &camps_near_npc = overmap_buffer.get_camps_near(
                            beta.pos_abs_sm(), recv_range * radio_tower_boost );
                bool camp_to_npc = false;
                bool camp_to_camp = false;
                for( const camp_reference &i : camps_near_player ) {
                    if( !i.camp->has_provides( "radio" ) ) {
                        continue;
                    }
                    if( camp_to_camp ||
                        square_dist( i.abs_sm_pos, beta.pos_abs_sm() ) <= recv_range * radio_tower_boost ) {
                        //one radio tower relay
                        camp_to_npc = true;
                        break;
                    }
                    for( const camp_reference &j : camps_near_npc ) {
                        //two radio tower relays
                        if( ( j.camp )->has_provides( "radio" ) &&
                            ( square_dist( i.abs_sm_pos, j.abs_sm_pos ) <= base_range * radio_tower_boost *
                              radio_tower_boost ) ) {
                            camp_to_camp = true;
                            break;
                        }
                    }
                }
                if( camp_to_npc || camp_to_camp ) {
                    return radio_contact_result::YES;
                } else {
                    return radio_contact_result::TOO_FAR;
                }
            }
        }
    }

    if( guy_has_radio && !u_has_radio ) {
        return radio_contact_result::ALPHA_NO_RADIO;
    }

    if( !guy_has_radio && u_has_radio ) {
        return radio_contact_result::BETA_NO_RADIO;
    }

    return radio_contact_result::BOTH_NO_RADIO;
}

static std::vector<npc *> get_known_faction_radio_representative( const faction *fac )
{
    const std::set<character_id> &known_fac_repres = get_avatar().faction_representatives;

    std::vector<npc *> applicable_repres;
    for( const character_id &guy : known_fac_repres ) {
        npc *npc = g->find_npc( guy );
        if( npc != nullptr &&
            !npc->is_dead() &&
            npc->faction_representative &&
            npc->get_faction_id() == fac->id ) {
            applicable_repres.emplace_back( npc );
        }
    }

    return applicable_repres;
}

bool faction_ui::execute()
{
    ctxt.register_action( "UP", to_translation( "Move cursor up" ) );
    ctxt.register_action( "DOWN", to_translation( "Move cursor down" ) );
    ctxt.register_action( "SCROLL_UP", to_translation( "Move cursor up" ) );
    ctxt.register_action( "SCROLL_DOWN", to_translation( "Move cursor down" ) );
    ctxt.register_action( "PAGE_UP", to_translation( "Scroll description up" ) );
    ctxt.register_action( "PAGE_DOWN", to_translation( "Scroll description down" ) );
    ctxt.register_action( "HOME", to_translation( "Scroll description to top" ) );
    ctxt.register_action( "END", to_translation( "Scroll description to bottom" ) );
    ctxt.register_leftright();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.set_timeout( 16 );

    while( true ) {
        ui_manager::redraw_invalidated();
        std::string action = ctxt.handle_input();
        if( action == "SELECT" ) {
            const int tab = hovered_tab();
            const int row = hovered_index( "data-row" );
            if( tab >= 0 ) {
                selected_tab = static_cast<tab_mode>( tab );
                continue;
            } else if( row >= 0 ) {
                pending_click = row;
                continue;
            }
            action = resolve_click( action );
        }

        if( action == "QUIT" ) {
            break;
        } else if( action == "NEXT_TAB" || action == "RIGHT" ) {
            ++selected_tab;
            reveal_selected = true;
        } else if( action == "PREV_TAB" || action == "LEFT" ) {
            --selected_tab;
            reveal_selected = true;
        } else if( action == "UP" ) {
            pending_move = -1;
            reveal_selected = true;
        } else if( action == "DOWN" ) {
            pending_move = 1;
            reveal_selected = true;
        } else if( action == "PAGE_UP" ) {
            scroll( "info", rml_ui::scroll::page_up );
        } else if( action == "PAGE_DOWN" ) {
            scroll( "info", rml_ui::scroll::page_down );
        } else if( action == "HOME" ) {
            scroll( "info", rml_ui::scroll::top );
        } else if( action == "END" ) {
            scroll( "info", rml_ui::scroll::bottom );
        }

        if( action == "CONFIRM" ) {

            if( selected_tab == tab_mode::TAB_FOLLOWERS && picked_follower != nullptr ) {
                const bool interactable = can_contact( get_avatar(), *picked_follower );
                const bool radio_interactable =
                    can_radio_contact( get_avatar(), *picked_follower ) == radio_contact_result::YES;
                if( interactable || radio_interactable ) {
                    hide_ui = true;

                    if( picked_follower->has_companion_mission() ) {
                        talk_function::basecamp_mission( *picked_follower );
                        return true;
                    } else {
                        get_avatar().talk_to( get_talker_for( *picked_follower ), true, false, false,
                                              picked_follower->chatbin.talk_radio );
                        return true;
                    }
                } else {
                    popup( string_format( _( "You cannot contact %s right now." ), picked_follower->get_name() ) );
                }
            }
            if( selected_tab == tab_mode::TAB_MYFACTION && picked_camp != nullptr ) {
                picked_camp->query_new_name();
                return false;
            }

            if( selected_tab == tab_mode::TAB_OTHERFACTIONS && picked_faction != nullptr ) {
                hide_ui = true;
                radio_the_faction();
                return true;
            }
            return true;
        }
    }
    return false;
}

// A line of the info pane
static std::string text_rml( const std::string &text )
{
    return "<div class=\"pre\">" + rml_ui::markup_to_rml( text ) + "</div>";
}

static std::string text_rml( const std::string &text, const nc_color &color )
{
    return "<div class=\"pre\">" + rml_ui::markup_to_rml( text, color ) + "</div>";
}

// A list beside the info about what it has selected, each scrolling on its own
static std::string panes_rml( const std::string &list, const std::string &info )
{
    return "<div class=\"panes\"><div class=\"pane\" style=\"flex: 0 0 35%;\">" + list +
           "</div><div id=\"info\" class=\"pane\">" + info + "</div></div>";
}

int faction_ui::apply_pending( int selected, int count )
{
    const int last_entry = count == 0 ? 0 : count - 1;
    selected += pending_move;
    pending_move = 0;
    if( pending_click >= 0 && pending_click < count ) {
        selected = pending_click;
    }
    pending_click = -1;
    if( selected < 0 ) {
        selected = last_entry;
    } else if( selected > last_entry ) {
        selected = 0;
    }
    return selected;
}

std::string faction_ui::list_rml( const std::vector<std::string> &names, int selected )
{
    selected_row = selected;
    std::string rml;
    for( size_t i = 0; i < names.size(); i++ ) {
        rml += string_format( "<div id=\"entry-%d\" class=\"item%s\" data-row=\"%d\">", i,
                              static_cast<int>( i ) == selected ? " selected" : "", i ) +
               rml_ui::markup_to_rml( names[i] ) + "</div>";
    }
    return rml;
}

std::string faction_ui::hint_rml() const
{
    const std::string desc = string_format(
                                 _( "[<color_yellow>%s</color>] Keybindings" ), ctxt.get_desc( "HELP_KEYBINDINGS" ) );
    return rml_ui::markup_to_rml( desc );
}

std::string faction_ui::your_faction_tab_rml()
{
    const std::string list = your_factions_list_rml();
    return panes_rml( list, your_faction_display() );
}

std::string faction_ui::your_factions_list_rml()
{
    Character &player_character = get_player_character();
    std::vector<basecamp *> camps;
    g->validate_camps();
    for( tripoint_abs_omt elem : player_character.camps ) {
        std::optional<basecamp *> p = overmap_buffer.find_camp( elem.xy() );
        if( !p ) {
            continue;
        }
        basecamp *temp_camp = *p;
        if( temp_camp->get_owner() != player_character.get_faction()->id ) {
            // Don't display NPC camps as ours
            continue;
        }
        camps.push_back( temp_camp );
    }

    size_t num_entries = camps.size();

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( camps.begin(), camps.end(), picked_camp );
    int selected_camp = ( it != camps.end() ) ? std::distance( camps.begin(), it ) : 0;
    selected_camp = apply_pending( selected_camp, static_cast<int>( num_entries ) );

    if( !camps.empty() ) {
        picked_camp = camps[selected_camp];
    }

    std::vector<std::string> names;
    names.reserve( camps.size() );
    for( const auto *it_camp : camps ) {
        names.emplace_back( it_camp->camp_name() );
    }
    return list_rml( names, camps.empty() ? -1 : selected_camp );
}

std::string faction_ui::your_faction_display() const
{
    std::string rml;

    if( picked_camp == nullptr ) {
        rml += text_rml( _( "You have no camps" ), c_red );
        return rml;
    }

    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();
    tripoint_abs_omt camp_pos = picked_camp->camp_omt_pos();
    std::string direction = direction_name( direction_from( player_abspos, camp_pos ) );
    faction *yours = player_character.get_faction();

    // Hint
    const std::string hint_desc = string_format(
                                      _( "Press [%s] to rename this camp" ), ctxt.get_desc( "CONFIRM" ) );
    rml += text_rml( hint_desc, c_light_gray );

    if( direction != "center" ) {
        const std::string desc_dir = string_format( _( "Direction: to the %s" ), direction );
        rml += text_rml( desc_dir );
    }

    const std::string desc_loc = string_format( _( "Location: %s" ), camp_pos.to_string() );
    rml += text_rml( desc_loc );

    const std::string food_text = string_format( _( "Food Supply: %s (%d kcal)" ),
                                  yours->food_supply_text(), yours->food_supply().kcal() );
    const nc_color food_col = yours->food_supply_color();
    rml += text_rml( food_text, food_col );

    const auto &[vit_color, vit] = yours->vitamin_stores( vitamin_type::VITAMIN );
    const std::string worst_vitamin = string_format( "%s: %s", _( "Worst vitamin" ), vit );
    rml += text_rml( worst_vitamin, vit_color );

    const auto &[toxin_color, toxin] = yours->vitamin_stores( vitamin_type::TOXIN );
    const std::string worst_toxin = string_format( "%s: %s", _( "Worst toxin" ), toxin );
    rml += text_rml( worst_toxin, toxin_color );

    const std::string bldg = picked_camp->next_upgrade( base_camps::base_dir, 1 );
    const std::string bldg_full = string_format( "%s: %s", _( "Next Upgrade" ), bldg );
    rml += text_rml( bldg_full );
    return rml;
}

std::string faction_ui::your_followers_tab_rml()
{
    const std::string list = your_followers_list_rml();
    return panes_rml( list, your_follower_display() );
}

std::string faction_ui::your_followers_list_rml()
{
    std::vector<npc *> followers;
    overmap_buffer.populate_followers_vec( followers );

    size_t num_entries = followers.size();

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( followers.begin(), followers.end(), picked_follower );
    int selected_npc = ( it != followers.end() ) ? std::distance( followers.begin(), it ) : 0;
    selected_npc = apply_pending( selected_npc, static_cast<int>( num_entries ) );

    if( !followers.empty() ) {
        picked_follower = followers[selected_npc];
    }

    std::vector<std::string> names;
    names.reserve( followers.size() );
    for( const auto *it_npc : followers ) {
        names.emplace_back( it_npc->get_name() );
    }
    return list_rml( names, followers.empty() ? -1 : selected_npc );
}

std::string faction_ui::your_follower_display()
{
    std::string rml;

    if( picked_follower == nullptr ) {
        rml += text_rml( _( "You have no followers." ), c_red );
        return rml;
    }

    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();

    // Mission
    std::string mission_string;
    if( picked_follower->has_companion_mission() ) {
        std::string dest_string;
        std::optional<tripoint_abs_omt> dest = picked_follower->get_mission_destination();
        if( dest ) {
            basecamp *dest_camp;
            std::optional<basecamp *> temp_camp = overmap_buffer.find_camp( dest->xy() );
            if( temp_camp ) {
                dest_camp = *temp_camp;
                dest_string = _( "traveling to: " ) + dest_camp->camp_name();
            } else {
                dest_string = string_format( _( "traveling to: %s" ), dest->to_string() );
            }
            mission_string = _( "Current Mission: " ) + dest_string;
        } else {
            npc_companion_mission c_mission = picked_follower->get_companion_mission();
            mission_string = _( "Current Mission: " ) + action_of( c_mission.miss_id.id );
        }

        rml += text_rml( mission_string );

        // Determine remaining time in mission, and display it
        std::string mission_eta;
        if( picked_follower->companion_mission_time_ret < calendar::turn ) {
            mission_eta = _( "JOB COMPLETED" );
        } else {
            mission_eta = _( "ETA: " ) + to_string( picked_follower->companion_mission_time_ret -
                                                    calendar::turn );
        }

        rml += text_rml( mission_eta );
    }

    tripoint_abs_omt guy_abspos = picked_follower->pos_abs_omt();
    basecamp *temp_camp = nullptr;
    if( picked_follower->assigned_camp ) {
        std::optional<basecamp *> bcp = overmap_buffer.find_camp( (
                                            *picked_follower->assigned_camp ).xy() );
        if( bcp ) {
            temp_camp = *bcp;
        }
    }

    // Direction
    const bool is_stationed = picked_follower->assigned_camp && temp_camp;
    std::string direction = direction_name( direction_from( player_abspos, guy_abspos ) );
    std::string dir_text;
    if( direction != "center" ) {
        dir_text = string_format( _( "Direction: to the %s" ), direction );
    } else {
        dir_text = _( "Direction: Nearby" );
    }
    rml += text_rml( dir_text );

    // Location
    std::string stationed_text;
    if( is_stationed ) {
        stationed_text = string_format( _( "Location: %s, at camp: %s" ), guy_abspos.to_string(),
                                        temp_camp->camp_name() );
    } else {
        stationed_text = string_format( _( "Location: %s" ), guy_abspos.to_string() );
    }
    rml += text_rml( stationed_text );

    // Can be radio-ed
    std::string can_see;
    nc_color see_color;
    if( can_contact( get_avatar(), *picked_follower ) ) {
        if( picked_follower->has_companion_mission() ) {
            can_see = _( "Press enter to recall from their mission." );
            see_color = c_light_red;
        } else {
            can_see = _( "Within interaction range" );
            see_color = c_light_green;
        }
    } else {
        const radio_contact_result rad = can_radio_contact( get_avatar(), *picked_follower );

        switch( rad ) {
            case radio_contact_result::ALPHA_NO_RADIO:
                can_see = _( "You do not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BETA_NO_RADIO:
                can_see = _( "Follower does not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BOTH_NO_RADIO:
                can_see = _( "Both you and follower need a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::TOO_FAR:
                can_see = _( "Not within radio range" );
                see_color = c_light_red;
                break;
            default:
                can_see = _( "Within radio range" );
                see_color = c_light_green;
                break;
        }
    }

    rml += text_rml( colorize( can_see, see_color ) );
    if( see_color == c_light_green ) {
        const std::string hint_desc = string_format(
                                          _( "Press [%s] to talk to this follower" ), ctxt.get_desc( "CONFIRM" ) );
        rml += text_rml( hint_desc, c_light_gray );
    }


    // Status/activity
    nc_color status_col = c_white;
    if( picked_follower->current_target() != nullptr ) {
        status_col = c_light_red;
    }
    const std::string status_text = string_format( _( "Status: %s" ),
                                    picked_follower->get_current_status() );
    rml += text_rml( colorize( status_text, status_col ) );

    // Job and station
    if( is_stationed && picked_follower->has_job() ) {
        rml += text_rml( _( "Working at camp" ) );
    } else if( is_stationed ) {
        rml += text_rml( _( "Idling at camp" ) );
    }

    // HP description
    const auto &[hp_desc, hp_color] = picked_follower->hp_description();
    const std::string cond_text = string_format( _( "%s: %s" ), _( "Condition" ), hp_desc );
    rml += text_rml( cond_text, hp_color );

    // Hunger, thirst, sleepiness
    const auto &[hunger, hunger_color] = display::hunger_text_color( *picked_follower );
    const auto &[thirst, thirst_color] = display::thirst_text_color( *picked_follower );
    const auto &[sleepiness, sleepiness_color] = display::sleepiness_text_color( *picked_follower );
    const std::string nominal = pgettext( "needs", "Nominal" );
    const std::string hunger_text =
        string_format( "%s: %s", _( "Hunger" ), hunger.empty() ? nominal : hunger );
    const std::string thirst_text =
        string_format( "%s: %s", _( "Thirst" ), thirst.empty() ? nominal : thirst );
    const std::string sleepiness_text =
        string_format( _( "%s: %s" ), _( "Sleepiness" ), sleepiness.empty() ? nominal : sleepiness );
    rml += text_rml( hunger_text, hunger_color );
    rml += text_rml( thirst_text, thirst_color );
    rml += text_rml( sleepiness_text, sleepiness_color );

    // Wield
    const std::string wielding_text = string_format( _( "Wielding: %s" ),
                                      picked_follower->weapname_simple() );
    rml += text_rml( wielding_text );

    const auto skillslist = Skill::get_skills_sorted_by( [&]( const Skill & a, const Skill & b ) {
        const int level_a = picked_follower->get_skill_level( a.ident() );
        const int level_b = picked_follower->get_skill_level( b.ident() );
        return localized_compare( std::make_pair( -level_a, a.name() ),
                                  std::make_pair( -level_b, b.name() ) );
    } );
    size_t count = 0;
    std::vector<std::string> skill_strs;
    for( size_t i = 0; i < skillslist.size() && count < 3; i++ ) {
        if( !skillslist[ i ]->is_combat_skill() ) {
            std::string skill_str = string_format( "%s: %d", skillslist[i]->name(),
                                                   static_cast<int>( picked_follower->get_skill_level( skillslist[i]->ident() ) ) );
            skill_strs.push_back( skill_str );
            count += 1;
        }
    }
    const std::string best_combat_skill_text = string_format( "%s: %d",
            picked_follower->best_combat_skill( combat_skills::NO_GENERAL ).first.obj().name(),
            picked_follower->best_combat_skill( combat_skills::NO_GENERAL ).second );

    const std::string space = std::string( "  " );

    rml += text_rml( _( "Best combat skill" ) + std::string( ": " ) );
    rml += text_rml( space + best_combat_skill_text );

    rml += text_rml( _( "Best other skills" ) + std::string( ": " ) );
    rml += text_rml( space + skill_strs[0] );
    rml += text_rml( space + skill_strs[1] );
    rml += text_rml( space + skill_strs[2] );
    return rml;
}

std::string faction_ui::other_factions_tab_rml()
{
    const std::string list = other_factions_list_rml();
    return panes_rml( list, other_faction_display() );
}

std::string faction_ui::other_factions_list_rml()
{
    std::vector<const faction *> factions;
    for( const auto &elem : g->faction_manager_ptr->all() ) {
        if( elem.second.known_by_u &&
            elem.second.id != faction_your_followers &&
            !elem.second.lone_wolf_faction ) {
            factions.emplace_back( &elem.second );
        }
    }

    size_t num_entries = factions.size();

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( factions.begin(), factions.end(), picked_faction );
    int selected_faction = ( it != factions.end() ) ? std::distance( factions.begin(), it ) : 0;
    selected_faction = apply_pending( selected_faction, static_cast<int>( num_entries ) );

    if( !factions.empty() ) {
        picked_faction = factions[selected_faction];
    }

    std::vector<std::string> names;
    names.reserve( factions.size() );
    for( const auto *it_faction : factions ) {
        names.emplace_back( it_faction->get_name() );
    }
    return list_rml( names, factions.empty() ? -1 : selected_faction );
}

std::string faction_ui::other_faction_display()
{
    std::string rml;

    if( picked_faction == nullptr ) {
        rml += text_rml( _( "You don't know of any factions." ) );
        return rml;
    }

    // Attitude towards you
    const std::string attitude_text = string_format( _( "Attitude to you: %s" ),
                                      fac_ranking_text( picked_faction->likes_u ) );
    rml += text_rml( attitude_text );


    // Can you contact them by radio
    const std::vector<npc *> applicable_repres = get_known_faction_radio_representative(
                picked_faction );
    if( !applicable_repres.empty() ) {
        rml += text_rml( _( "You know someone from this faction you can contact by radio." ),
                         c_green );
        if( !has_radio( get_avatar() ) ) {
            rml += text_rml( _( "Even if you do not have a radio right now." ),
                             c_red );
        } else {
            const std::string hint_desc = string_format(
                                              _( "Press [%s] to radio the faction representative" ), ctxt.get_desc( "CONFIRM" ) );
            rml += text_rml( hint_desc, c_light_gray );
        }
    }

    // Description
    rml += text_rml( picked_faction->desc.translated() );
    return rml;
}

std::string faction_ui::creatures_tab_rml()
{
    const std::string list = creature_list_rml();
    return panes_rml( list, creature_display() );
}

std::string faction_ui::creature_list_rml()
{
    Character &player_character = get_player_character();
    std::vector<const mtype_id *> creatures;
    const std::set<mtype_id> &known_monsters = player_character.get_known_monsters();
    creatures.reserve( known_monsters.size() );
    for( const mtype_id &m : known_monsters ) {
        creatures.emplace_back( &m );
    }
    std::sort( creatures.begin(), creatures.end(), []( const mtype_id * a, const mtype_id * b ) {
        return localized_compare( a->obj().nname(), b->obj().nname() );
    } );

    size_t num_entries = creatures.size();

    // initialize selected_creature either from already set camp, or from zero if not set
    auto it = std::find( creatures.begin(), creatures.end(), picked_creature );
    int selected_creature = ( it != creatures.end() ) ? std::distance( creatures.begin(), it ) : 0;
    selected_creature = apply_pending( selected_creature, static_cast<int>( num_entries ) );

    if( !creatures.empty() ) {
        picked_creature = creatures[selected_creature];
    }

    std::vector<std::string> names;
    names.reserve( creatures.size() );
    for( const auto *it_creature : creatures ) {
        names.emplace_back( it_creature->obj().nname() );
    }
    return list_rml( names, creatures.empty() ? -1 : selected_creature );
}

std::string faction_ui::creature_display() const
{
    std::string rml;

    if( picked_creature == nullptr ) {
        rml += text_rml( _( "You haven't recorded sightings of any creatures.  Taking photos can be a good way to keep track of them." ) );
        return rml;
    }

    const mtype &cr = picked_creature->obj();

    // Name & symbol
    rml += text_rml( string_format( "%s  %s", colorize( cr.sym, cr.color ), cr.nname() ) );

    // Difficulty
    const std::string diff_str = cr.get_difficulty_description();
    rml += text_rml( string_format( "%s: %s", _( "Difficulty" ), diff_str ) );

    // Origin
    const std::string origin_list = string_format( "%s: %s", _( "Origin" ),
                                    enumerate_as_string( cr.src.begin(), cr.src.end(),
    []( const std::pair<mtype_id, mod_id> &source ) {
        return string_format( "'%s'", source.second->name() );
    }, enumeration_conjunction::arrow ) );
    rml += text_rml( origin_list, c_light_gray );

    // Size
    rml += text_rml( string_format( "%s: %s", _( "Size" ), colorize( cr.get_size_name(),
                     c_light_gray ) ) );

    // Species
    const std::string species_list = string_format( "%s: %s", colorize( _( "Species" ), c_white ),
    enumerate_as_string( cr.species_descriptions(), []( const std::string & sp ) {
        return colorize( sp, c_yellow );
    } ) );
    rml += text_rml( species_list, c_light_gray );

    // Senses
    std::vector<std::string> senses_str;
    if( cr.has_flag( mon_flag_HEARS ) ) {
        senses_str.emplace_back( colorize( _( "hearing" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SEES ) ) {
        senses_str.emplace_back( colorize( _( "sight" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SMELLS ) ) {
        senses_str.emplace_back( colorize( _( "smell" ), c_yellow ) );
    }
    const std::string senses_text =
        string_format( "%s: %s", colorize( _( "Senses" ), c_white ),
                       enumerate_as_string( senses_str ) );
    rml += text_rml( senses_text, c_light_gray );

    // Abilities
    if( cr.has_flag( mon_flag_SWIMS ) || cr.move_skills.swim.has_value() ) {
        rml += text_rml( _( "It can swim." ) );
    }
    if( cr.has_flag( mon_flag_FLIES ) ) {
        rml += text_rml( _( "It can fly." ) );
    }
    if( cr.has_flag( mon_flag_DIGS ) || cr.move_skills.dig.has_value() ) {
        rml += text_rml( _( "It can burrow underground." ) );
    }
    if( cr.has_flag( mon_flag_CLIMBS ) || cr.move_skills.climb.has_value() ) {
        rml += text_rml( _( "It can climb." ) );
    }
    if( cr.has_flag( mon_flag_GRABS ) ) {
        rml += text_rml( _( "It can grab." ) );
    }
    if( cr.has_flag( mon_flag_VENOM ) ) {
        rml += text_rml( _( "It can inflict poison." ) );
    }
    if( cr.has_flag( mon_flag_PARALYZEVENOM ) ) {
        rml += text_rml( _( "It can inflict paralysis." ) );
    }

    rml += "<br/>";
    // Description
    rml += text_rml( cr.get_description(), c_light_gray );
    return rml;
}

void faction_ui::on_redraw()
{
    // At least half the screen, as the old window was
    set_size( "70%", "80%" );
    set_class( "window", "hidden", hide_ui );

    set_header( hint_rml() );
    set_tabs( { _( "Your Faction" ), _( "Your Followers" ), _( "Other Factions" ), _( "Creatures" ) },
              static_cast<size_t>( selected_tab ) );
    set_body_scrolls( false );
    switch( selected_tab ) {
        case tab_mode::TAB_MYFACTION:
            set_body( your_faction_tab_rml() );
            break;
        case tab_mode::TAB_FOLLOWERS:
            set_body( your_followers_tab_rml() );
            break;
        case tab_mode::TAB_OTHERFACTIONS:
            set_body( other_factions_tab_rml() );
            break;
        case tab_mode::TAB_CREATURES:
            set_body( creatures_tab_rml() );
            break;
        case tab_mode::NUM_TABS:
            break;
    }
    if( reveal_selected ) {
        reveal_selected = false;
        scroll_into_view( "entry-" + std::to_string( selected_row ) );
        scroll( "info", rml_ui::scroll::top );
    } else if( shown_tab != selected_tab ) {
        scroll( "info", rml_ui::scroll::top );
    }
    shown_tab = selected_tab;
}

void faction_ui::radio_the_faction()
{
    if( !has_radio( get_avatar() ) ) {
        popup( _( "You do not have a radio to contact anyone." ) );
        return;
    }

    std::vector<npc *> applicable_repres = get_known_faction_radio_representative( picked_faction );

    if( applicable_repres.empty() ) {
        popup( string_format( _( "You do not know anyone you can contact." ) ) );
        return;
    }

    uilist call_npc_query;
    call_npc_query.text = _( "Contact who?" );
    for( const npc *npc : applicable_repres ) {
        const radio_contact_result res = can_radio_contact( get_avatar(), *npc );
        std::string desc = npc->disp_name();
        if( res == radio_contact_result::YES ) {
            call_npc_query.addentry( MENU_AUTOASSIGN, true, MENU_AUTOASSIGN, npc->disp_name() );
        } else {
            // ALPHA_NO_RADIO & BOTH_NO_RADIO are excluded by check above
            // so this fires only for BETA_NO_RADIO and TOO_FAR
            const std::string desc = string_format( "%s (%s)", npc->disp_name(), _( "No response" ) );
            call_npc_query.addentry( MENU_AUTOASSIGN, false, MENU_AUTOASSIGN, desc );
        }
    }
    call_npc_query.query();

    if( call_npc_query.ret < 0 ) {
        return;
    }
    npc *n = applicable_repres[call_npc_query.ret];
    get_avatar().talk_to( get_talker_for( n ), true, false, false,
                          n->chatbin.talk_radio );
}

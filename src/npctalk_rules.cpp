#include "npctalk_rules.h"

#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auto_pickup.h"
#include "character.h"
#include "character_id.h"
#include "color.h"
#include "debug.h"
#include "game.h"
#include "input_context.h"
#include "input_enums.h"
#include "npc.h"
#include "pimpl.h"
#include "rml_markup.h"
#include "string_formatter.h"
#include "translation.h"
#include "ui_manager.h"

static std::map<cbm_recharge_rule, std::string> recharge_map = {
    {cbm_recharge_rule::CBM_RECHARGE_ALL, "<ally_rule_cbm_recharge_all_text>" },
    {cbm_recharge_rule::CBM_RECHARGE_MOST, "<ally_rule_cbm_recharge_most_text>" },
    {cbm_recharge_rule::CBM_RECHARGE_SOME, "<ally_rule_cbm_recharge_some_text>" },
    {cbm_recharge_rule::CBM_RECHARGE_LITTLE, "<ally_rule_cbm_recharge_little_text>" },
    {cbm_recharge_rule::CBM_RECHARGE_NONE, "<ally_rule_cbm_recharge_none_text>" },
};

static std::map<cbm_reserve_rule, std::string> reserve_map = {
    {cbm_reserve_rule::CBM_RESERVE_ALL, "<ally_rule_cbm_reserve_all_text>" },
    {cbm_reserve_rule::CBM_RESERVE_MOST, "<ally_rule_cbm_reserve_most_text>" },
    {cbm_reserve_rule::CBM_RESERVE_SOME, "<ally_rule_cbm_reserve_some_text>" },
    {cbm_reserve_rule::CBM_RESERVE_LITTLE, "<ally_rule_cbm_reserve_little_text>" },
    {cbm_reserve_rule::CBM_RESERVE_NONE, "<ally_rule_cbm_reserve_none_text>" },
};

static std::map<combat_engagement, std::string> engagement_rules = {
    {combat_engagement::NONE, "<ally_rule_engagement_none>" },
    {combat_engagement::CLOSE, "<ally_rule_engagement_close>" },
    {combat_engagement::WEAK, "<ally_rule_engagement_weak>" },
    {combat_engagement::HIT, "<ally_rule_engagement_hit>" },
    {combat_engagement::ALL, "<ally_rule_engagement_all>" },
    {combat_engagement::FREE_FIRE, "<ally_rule_engagement_free_fire>" },
    {combat_engagement::NO_MOVE, "<ally_rule_engagement_no_move>" },
};

static std::map<aim_rule, std::string> aim_rule_map = {
    {aim_rule::WHEN_CONVENIENT, "<ally_rule_aim_when_convenient>" },
    {aim_rule::SPRAY, "<ally_rule_aim_spray>" },
    {aim_rule::PRECISE, "<ally_rule_aim_precise>" },
    {aim_rule::STRICTLY_PRECISE, "<ally_rule_aim_strictly_precise>" },
};

void follower_rules_ui::draw_follower_rules_ui( npc *guy )
{
    follower_rules_ui_impl p_impl;
    p_impl.set_npc_pointer_to( guy );
    p_impl.run();
}

follower_rules_ui_impl::follower_rules_ui_impl() :
    rml_ui::window( _( "Rules for your follower" ) )
{
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "CONFIRM", to_translation( "Set, toggle, or reset selected rule" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "ANY_INPUT" );
    ctxt.set_timeout( 10 );
}

void follower_rules_ui_impl::set_npc_pointer_to( npc *new_guy )
{
    guy = new_guy;
}

std::string follower_rules_ui_impl::get_parsed( std::string initial_string )
{
    std::string string_reference = std::move( initial_string );
    parse_tags( string_reference, get_player_character(), *guy );
    return string_reference;

}

std::string follower_rules_ui_impl::hotkey_rml( const input_event &hotkey ) const
{
    const std::string key = hotkey.sequence.empty() ? std::string() :
                            std::string( 1, static_cast<char>( hotkey.sequence.front() ) );
    return "<span class=\"hotkey\">" + rml_ui::colored_rml( key, c_green ) + "</span>";
}

template<typename T>
void follower_rules_ui_impl::checkbox( const T &this_rule, const input_event &assigned_hotkey,
                                       std::vector<rule_row> &rows, std::vector<rule_hotkey> &hotkeys )
{
    const ally_rule rule = this_rule.rule;
    const bool rule_enabled = guy->rules.has_flag( rule );
    const std::string rules_text = get_parsed( rule_enabled ? this_rule.rule_true_text :
                                   this_rule.rule_false_text );
    const auto toggle = [this, rule]() {
        const bool enabled = !guy->rules.has_flag( rule );
        guy->rules.toggle_flag( rule );
        guy->rules.toggle_specific_override_state( rule, enabled );
    };
    const auto reset = [this, rule]() {
        guy->rules.clear_flag( rule );
        guy->rules.clear_override( rule );
    };
    rows.push_back( { hotkey_rml( assigned_hotkey ) + "<span class=\"check" +
                      std::string( rule_enabled ? " on" : "" ) + "\"></span>" +
                      rml_ui::markup_to_rml( rules_text ), toggle, reset } );
    hotkeys.emplace_back( assigned_hotkey, toggle );
}

template<typename T>
void follower_rules_ui_impl::radio_group( const char *title, T *rule,
        std::map<T, std::string> &values, const input_event &assigned_hotkey,
        std::vector<rule_row> &rows, std::vector<rule_hotkey> &hotkeys )
{
    rows.push_back( { "<div class=\"sep\"></div>" + hotkey_rml( assigned_hotkey ) +
                      rml_ui::colored_rml( title, c_white ), nullptr, nullptr } );
    for( const std::pair<const T, std::string> &value : values ) {
        const T option = value.first;
        rows.push_back( { "<span class=\"hotkey\"></span><span class=\"radio" +
                          std::string( *rule == option ? " on" : "" ) + "\"></span>" +
                          rml_ui::markup_to_rml( get_parsed( value.second ) ),
        [rule, option]() {
            *rule = option;
        }, nullptr } );
    }
    // The hotkey advances and wraps through the options
    hotkeys.emplace_back( assigned_hotkey, [rule, &values]() {
        if( values.upper_bound( *rule ) == values.end() ) {
            // Then we have the *last* entry the map, so wrap to the first element
            *rule = values.begin()->first;
        } else {
            // Assign the next possible value contained in the map.
            *rule = values.upper_bound( *rule )->first;
        }
    } );
}

void follower_rules_ui_impl::build_rules( std::vector<rule_row> &rows,
        std::vector<rule_hotkey> &hotkeys )
{
    const hotkey_queue &queue = hotkey_queue::alphabets();
    input_event assigned_hotkey = ctxt.first_unassigned_hotkey( queue );

    const auto default_all = [this]() {
        // TODO: use query_yn here as a safeguard against fatfingering.
        npc_follower_rules default_values; //Call the constructor and let *it* tell us what the default is
        guy->rules = default_values;
    };
    rows.push_back( { hotkey_rml( assigned_hotkey ) + rml_ui::escape_rml( _( "Default ALL" ) ),
                      default_all, nullptr } );
    hotkeys.emplace_back( assigned_hotkey, default_all );

    /* Handle all of our regular, boolean rules */
    for( const std::pair<const std::string, ally_rule_data> &rule_data : ally_rule_strs ) {
        assigned_hotkey = ctxt.next_unassigned_hotkey( queue, assigned_hotkey );
        checkbox( rule_data.second, assigned_hotkey, rows, hotkeys );
    }

    // Engagement rules require their own set of buttons, each instruction is unique
    assigned_hotkey = ctxt.next_unassigned_hotkey( queue, assigned_hotkey );
    radio_group( _( "Engagement rules:" ), &guy->rules.engagement, engagement_rules,
                 assigned_hotkey, rows, hotkeys );

    // Aiming rule also has a non-boolean set of values
    assigned_hotkey = ctxt.next_unassigned_hotkey( queue, assigned_hotkey );
    radio_group( _( "Aiming rules:" ), &guy->rules.aim, aim_rule_map, assigned_hotkey, rows, hotkeys );

    /* Shows CBM rules, but only if the character has bionics. Must be last because it will
    only appear sometimes and we don't want hotkeys to be different depending on whether
    the character has bionics. That's bad for muscle memory! */
    if( !guy->get_bionics().empty() ) {
        assigned_hotkey = ctxt.next_unassigned_hotkey( queue, assigned_hotkey );
        radio_group( _( "CBM recharging rules:" ), &guy->rules.cbm_recharge, recharge_map,
                     assigned_hotkey, rows, hotkeys );

        assigned_hotkey = ctxt.next_unassigned_hotkey( queue, assigned_hotkey );
        radio_group( _( "CBM reserve rules" ), &guy->rules.cbm_reserve, reserve_map,
                     assigned_hotkey, rows, hotkeys );
    }
}

std::string follower_rules_ui_impl::rules_rml( const std::vector<rule_row> &rows ) const
{
    std::string rml = "<p>" + rml_ui::markup_to_rml( string_format( _( "Rules for your follower, %s" ),
                      guy->disp_name() ) ) + "</p><div class=\"sep\"></div>" +
                      rml_ui::button_rml( "IMPORT", rml_ui::escape_rml( _( "Import settings from follower" ) ) ) +
                      rml_ui::button_rml( "EXPORT", rml_ui::escape_rml( _( "Export settings to follower" ) ) ) +
                      "<p>" + rml_ui::escape_rml( _( "Hotkey:" ) ) + "</p>";
    for( size_t i = 0; i < rows.size(); i++ ) {
        const rule_row &row = rows[i];
        if( !row.activate ) {
            rml += "<div>" + row.rml + "</div>";
            continue;
        }
        rml += string_format( "<div id=\"rule-%d\" class=\"item%s\" data-row=\"%d\">"
                              "<div class=\"hbox\" style=\"align-items: center;\"><div class=\"grow\">",
                              i, static_cast<int>( i ) == cursor ? " selected" : "", i ) + row.rml + "</div>";
        if( row.reset ) {
            rml += string_format( "<div class=\"button\" data-reset=\"%d\">", i ) +
                   rml_ui::escape_rml( _( "Default" ) ) + "</div>";
        }
        rml += "</div></div>";
    }
    return rml;
}

std::vector<npc *> follower_rules_ui_impl::other_allies() const
{
    std::vector<npc *> allies;
    for( npc &role_model : g->all_npcs() ) {
        // No copying settings to/from yourself
        if( &role_model != guy && role_model.is_player_ally() ) {
            allies.push_back( &role_model );
        }
    }
    return allies;
}

std::string follower_rules_ui_impl::transfer_rml() const
{
    std::string toggle_label;
    std::string rml = "<div class=\"hbox\" style=\"align-items: center;\"><div class=\"grow\">";
    if( exporting_rules ) {
        rml += rml_ui::markup_to_rml( string_format( _( "Exporting rules from <color_white>%s</color>" ),
                                      guy->disp_name() ), c_blue );
        //~Label for a button used to copy NPC follower rules to another NPC
        toggle_label = _( "Export" );
    } else {
        rml += rml_ui::markup_to_rml( string_format( _( "Importing rules to <color_white>%s</color>" ),
                                      guy->disp_name() ), c_yellow );
        //~Label for a button used to copy NPC follower rules from another NPC
        toggle_label = _( "Import" );
    }
    rml += "</div>" + rml_ui::button_rml( "RETURN",
                                          rml_ui::escape_rml( _( "Return to rules settings" ) ) ) + "</div>";
    if( g->get_follower_list().size() < 2 ) {
        return rml + "<p>" + rml_ui::markup_to_rml( string_format(
                    _( "Functions unavailable.  %s is your only follower." ), guy->disp_name() ), c_red ) + "</p>";
    }
    rml += rml_ui::button_rml( "TOGGLE", rml_ui::escape_rml( _( "Toggle export/import" ) ) );
    rml += "<p>" + rml_ui::markup_to_rml( string_format(
            _( "Individual settings are colored if they already match to %s." ), guy->disp_name() ) ) + "</p>";

    rml += "<table class=\"grid\"><thead><tr>";
    for( const char *heading : {
             _( "Name" ), _( "Behaviors" ), _( "Aiming" ), _( "Engagement" ), _( "CBM recharge" ),
             _( "CBM reserve" ), _( "Pickup list" ), _( "ALL" )
         } ) {
        rml += "<th>" + rml_ui::escape_rml( heading ) + "</th>";
    }
    rml += "</tr></thead><tbody>";
    const std::vector<npc *> allies = other_allies();
    for( size_t i = 0; i < allies.size(); i++ ) {
        const npc &role_model = *allies[i];
        // This is not correct if both have rules and both have *different* rules, but comparison is expensive
        const bool same_pickup = role_model.rules.pickup_whitelist->empty() ==
                                 guy->rules.pickup_whitelist->empty();
        const std::vector<bool> matches = {
            role_model.rules.flags == guy->rules.flags,
            role_model.rules.aim == guy->rules.aim,
            role_model.rules.engagement == guy->rules.engagement,
            role_model.rules.cbm_recharge == guy->rules.cbm_recharge,
            role_model.rules.cbm_reserve == guy->rules.cbm_reserve,
            same_pickup,
            role_model.rules.flags == guy->rules.flags &&
            role_model.rules.aim == guy->rules.aim &&
            role_model.rules.engagement == guy->rules.engagement &&
            role_model.rules.cbm_recharge == guy->rules.cbm_recharge &&
            role_model.rules.cbm_reserve == guy->rules.cbm_reserve &&
            same_pickup
        };
        rml += "<tr><td>" + rml_ui::markup_to_rml( role_model.disp_name() ) + "</td>";
        for( size_t column = 0; column < matches.size(); column++ ) {
            rml += string_format( "<td><div class=\"button%s\" data-npc=\"%d\" data-rule=\"%d\">",
                                  matches[column] ? " good" : "", i, column ) +
                   rml_ui::escape_rml( toggle_label ) + "</div></td>";
        }
        rml += "</tr>";
    }
    return rml + "</tbody></table>";
}

void follower_rules_ui_impl::transfer_rules( npc &role_model, int column )
{
    // The last column is ALL
    const bool do_all = column == 6;
    // To make this slightly easier...
    npc *guy_with_same = exporting_rules ? guy : &role_model;
    npc *guy_with_new = exporting_rules ? &role_model : guy;
    if( column == 0 || do_all ) {
        guy_with_new->rules.flags = guy_with_same->rules.flags;
        guy_with_new->rules.overrides = guy_with_same->rules.overrides;
        guy_with_new->rules.override_enable = guy_with_same->rules.override_enable;
    }
    if( column == 1 || do_all ) {
        guy_with_new->rules.aim = guy_with_same->rules.aim;
    }
    if( column == 2 || do_all ) {
        guy_with_new->rules.engagement = guy_with_same->rules.engagement;
    }
    if( column == 3 || do_all ) {
        guy_with_new->rules.cbm_recharge = guy_with_same->rules.cbm_recharge;
    }
    if( column == 4 || do_all ) {
        guy_with_new->rules.cbm_reserve = guy_with_same->rules.cbm_reserve;
    }
    if( column == 5 || do_all ) {
        guy_with_new->rules.pickup_whitelist = guy_with_same->rules.pickup_whitelist;
    }
}

bool follower_rules_ui_impl::transfer_input( const std::string &action )
{
    if( action == "SELECT" ) {
        const int ally = hovered_index( "data-npc" );
        const int column = hovered_index( "data-rule" );
        const std::vector<npc *> allies = other_allies();
        if( ally >= 0 && column >= 0 && static_cast<size_t>( ally ) < allies.size() ) {
            transfer_rules( *allies[ally], column );
            return true;
        }
    }
    const std::string clicked = resolve_click( action );
    if( clicked == "RETURN" ) {
        in_popup = false;
    } else if( clicked == "TOGGLE" ) {
        exporting_rules = !exporting_rules;
    }
    return clicked != "QUIT";
}

void follower_rules_ui_impl::run()
{
    if( !guy ) {
        debugmsg( "Something has gone very wrong, can't find NPC to set rules for.  Aborting." );
        return;
    }

    while( true ) {
        ui_manager::redraw_invalidated();

        std::string action = ctxt.handle_input();
        if( in_popup ) {
            if( !transfer_input( action ) ) {
                break;
            }
            continue;
        }

        std::vector<rule_row> rows;
        std::vector<rule_hotkey> hotkeys;
        build_rules( rows, hotkeys );
        const int count = static_cast<int>( rows.size() );
        if( action == "SELECT" ) {
            const int reset = hovered_index( "data-reset" );
            const int row = hovered_index( "data-row" );
            if( reset >= 0 && reset < count && rows[reset].reset ) {
                cursor = reset;
                rows[reset].reset();
                continue;
            } else if( row >= 0 && row < count && rows[row].activate ) {
                cursor = row;
                rows[row].activate();
                continue;
            }
            action = resolve_click( action );
        }

        if( action == "QUIT" ) {
            break;
        } else if( action == "IMPORT" || action == "EXPORT" ) {
            exporting_rules = action == "EXPORT";
            in_popup = true;
        } else if( ( action == "UP" || action == "DOWN" ) && count > 0 ) {
            // To the next row that does something, past the headings
            const int step = action == "UP" ? count - 1 : 1;
            int next = cursor;
            do {
                next = ( next + step ) % count;
            } while( !rows[next].activate && next != cursor );
            cursor = next;
            reveal_cursor = true;
        } else if( action == "CONFIRM" ) {
            if( cursor >= 0 && cursor < count && rows[cursor].activate ) {
                rows[cursor].activate();
            }
        } else if( action == "ANY_INPUT" ) {
            const input_event pressed_key = ctxt.get_raw_input();
            for( const rule_hotkey &hotkey : hotkeys ) {
                if( !hotkey.first.sequence.empty() && pressed_key == hotkey.first ) {
                    hotkey.second();
                    break;
                }
            }
        }
    }
}

void follower_rules_ui_impl::on_redraw()
{
    // At least half the screen, as the old window was
    set_size( "70%", "80%" );
    if( !guy ) {
        return;
    }
    if( in_popup ) {
        set_body( transfer_rml() );
        return;
    }
    std::vector<rule_row> rows;
    std::vector<rule_hotkey> hotkeys;
    build_rules( rows, hotkeys );
    set_body( rules_rml( rows ) );
    if( reveal_cursor ) {
        reveal_cursor = false;
        scroll_into_view( "rule-" + std::to_string( cursor ) );
    }
}

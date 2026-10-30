#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "bionics.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character.h"
#include "color.h"
#include "cursesdef.h"
#include "enums.h"
#include "flat_set.h"
#include "game.h"
#include "input.h"
#include "input_context.h"
#include "input_enums.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "localized_comparator.h"
#include "map.h"
#include "options.h"
#include "output.h"
#include "pimpl.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "uilist.h"
#include "uistate.h"
#include "units.h"
#include "vehicle.h"

static const itype_id itype_battery( "battery" );

static const json_character_flag json_flag_BIONIC_GUN( "BIONIC_GUN" );
static const json_character_flag json_flag_BIONIC_POWER_SOURCE( "BIONIC_POWER_SOURCE" );
static const json_character_flag json_flag_BIONIC_TOGGLED( "BIONIC_TOGGLED" );

// '!', '-' and '=' are uses as default bindings in the menu
static const invlet_wrapper
bionic_chars( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\"#&()*+./:;@[\\]^_{|}" );

namespace
{
enum bionic_tab_mode {
    TAB_ACTIVE,
    TAB_PASSIVE
};
enum bionic_menu_mode {
    ACTIVATING,
    EXAMINING,
    REASSIGNING
};

std::string sort_mode_str( bionic_ui_sort_mode mode )
{
    switch( mode ) {
        case bionic_ui_sort_mode::nsort:
        case bionic_ui_sort_mode::NONE:
            return _( "None" );
        case bionic_ui_sort_mode::POWER:
            return _( "Power usage" );
        case bionic_ui_sort_mode::NAME:
            return _( "Name" );
        case bionic_ui_sort_mode::INVLET:
            return _( "Manual (shortcut)" );
    }
    return "error";
}

bool is_power_cbm( const bionic_data &data )
{
    return !data.fuel_opts.empty() || data.is_remote_fueled;
}

units::energy bionic_sort_power( const bionic_data &lbd )
{
    return is_power_cbm( lbd ) ? units::energy( -1_kJ * lbd.fuel_efficiency ) :
           lbd.power_activate + lbd.power_over_time;
}

struct bionic_sort_less {
    bool operator()( const bionic *lhs, const bionic *rhs ) const {
        const bionic_data &lbd = lhs->info();
        const bionic_data &rbd = rhs->info();

        switch( uistate.bionic_sort_mode ) {
            case bionic_ui_sort_mode::nsort:
            case bionic_ui_sort_mode::NONE:
                //use installation order
                return true;
            case bionic_ui_sort_mode::INVLET:
                return lhs->invlet < rhs->invlet;
            case bionic_ui_sort_mode::POWER: {
                units::energy lbd_sort_power = bionic_sort_power( lbd );
                units::energy rbd_sort_power = bionic_sort_power( rbd );
                if( lbd_sort_power != rbd_sort_power ) {
                    return lbd_sort_power < rbd_sort_power;
                }
            }
            [[fallthrough]];
            case bionic_ui_sort_mode::NAME:
                return localized_compare( lbd.name.translated(), rbd.name.translated() );
        }
        return false;
    }
};

using sorted_bionics = cata::flat_set<bionic *, bionic_sort_less>;

sorted_bionics filtered_bionics( bionic_collection &all_bionics,
                                 bionic_tab_mode mode )
{
    sorted_bionics filtered_entries;
    for( bionic &elem : all_bionics ) {
        if( ( mode == TAB_ACTIVE ) == elem.id->activated ) {
            filtered_entries.insert( &elem );
        }
    }
    return filtered_entries;
}

bionic_ui_sort_mode pick_sort_mode()
{
    uilist tmenu;
    tmenu.text = _( "Sort bionics by:" );
    tmenu.addentry( 1, true, 'p', sort_mode_str( bionic_ui_sort_mode::POWER ) );
    tmenu.addentry( 2, true, 'n', sort_mode_str( bionic_ui_sort_mode::NAME ) );
    tmenu.addentry( 3, true, 'i', sort_mode_str( bionic_ui_sort_mode::INVLET ) );
    tmenu.addentry( 4, true, 'o', sort_mode_str( bionic_ui_sort_mode::NONE ) );

    tmenu.query();
    switch( tmenu.ret ) {
        case 1:
            return bionic_ui_sort_mode::POWER;
        case 2:
            return bionic_ui_sort_mode::NAME;
        case 3:
            return bionic_ui_sort_mode::INVLET;
        case 4:
            return bionic_ui_sort_mode::NONE;
    }

    return bionic_ui_sort_mode::NONE;
}

} // namespace

namespace io
{
template<>
std::string enum_to_string<bionic_ui_sort_mode>( bionic_ui_sort_mode mode )
{
    switch( mode ) {
        case bionic_ui_sort_mode::nsort:
        case bionic_ui_sort_mode::NONE:
            return "none";
        case bionic_ui_sort_mode::POWER:
            return "power";
        case bionic_ui_sort_mode::NAME:
            return "name";
        case bionic_ui_sort_mode::INVLET:
            return "invlet";
    }

    return "error";
}
} // namespace io

bionic *avatar::bionic_by_invlet( const int ch )
{
    // space is a special case for unassigned
    if( ch == ' ' ) {
        return nullptr;
    }

    for( bionic &elem : *my_bionics ) {
        if( elem.invlet == ch ) {
            return &elem;
        }
    }
    return nullptr;
}

static std::string action_bound_to_key( const input_context &ctxt, char key )
{
    return ctxt.input_to_action( input_event( key, input_event_t::keyboard_char ) );
}

char get_free_invlet( Character &p )
{
    if( p.is_npc() ) {
        // npcs don't need an invlet
        return ' ';
    }

    input_context ctxt( "BIONICS", keyboard_mode::keychar );
    // Register standard uilist actions that might be bound to keys
    ctxt.register_updown();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    for( const char &inv_char : bionic_chars ) {
        if( action_bound_to_key( ctxt, inv_char ) != "ERROR" ) {
            continue;
        }
        if( p.as_avatar()->bionic_by_invlet( inv_char ) == nullptr ) {
            return inv_char;
        }
    }
    return ' ';
}

/** The header: what keys do in this mode, the fuel at hand, and the power. */
static std::string bionics_titlebar_rml( avatar *p, bionic_menu_mode mode )
{
    map &here = get_map();

    input_context ctxt( "BIONICS", keyboard_mode::keychar );

    std::string fuel_string;
    bool found_fuel = false;
    fuel_string = _( "Available Fuel: " );
    for( const bionic &bio : *p->my_bionics ) {
        for( const item *fuel_source : p->get_bionic_fuels( bio.id ) ) {
            const item *fuel;
            if( fuel_source->ammo_remaining( ) ) {
                fuel = &fuel_source->first_ammo();
            } else {
                fuel = *fuel_source->all_items_top().begin();
            }
            found_fuel = true;
            fuel_string += fuel->tname() + ": " + colorize( std::to_string( fuel->charges ), c_green ) + " ";
        }
    }
    for( const item *ups : p->get_cable_ups() ) {
        found_fuel = true;
        const item *fuel = &ups->first_ammo();
        fuel_string += fuel->tname() + ": " + colorize( std::to_string( fuel->charges ), c_green ) + " ";
    }
    for( vehicle *veh : p->get_cable_vehicle() ) {
        int64_t charges = veh->connected_battery_power_level( here ).first;
        if( charges > 0 ) {
            found_fuel = true;
            fuel_string += item( itype_battery ).tname() + ": " + colorize( std::to_string( charges ),
                           c_green ) + " ";
        }
    }

    if( !found_fuel ) {
        fuel_string.clear();
    }
    std::string power_string;
    const units::energy curr_power = p->get_power_level();
    const int kilo = units::to_kilojoule( curr_power );
    const int joule = units::to_joule( curr_power ) % units::to_joule( 1_kJ );
    const int milli = kilo > 0 ? units::to_millijoule( curr_power ) % units::to_millijoule( 1_J ) : 0;
    if( kilo > 0 ) {
        power_string = std::to_string( kilo );
        if( joule > 0 ) {
            power_string += pgettext( "decimal separator", "." ) + std::to_string( joule );
        }
        power_string += pgettext( "energy unit: kilojoule", "kJ" );
    } else if( joule > 0 ) {
        power_string = std::to_string( joule );
        if( milli > 0 ) {
            power_string += pgettext( "decimal separator", "." ) + std::to_string( milli );
        }
        power_string += pgettext( "energy unit: joule", "J" );
    } else {
        power_string = std::to_string( milli ) + pgettext( "energy unit: millijoule", "mJ" );
    }

    const std::string power_rml = rml_ui::markup_to_rml( string_format(
                                      _( "Bionic Power: <color_light_blue>%s</color>/<color_light_blue>%ikJ</color>" ),
                                      power_string, units::to_kilojoule( p->get_max_power_level() ) ), c_white );

    std::string desc_append = string_format(
                                  _( "[<color_yellow>%s</color>] Reassign, [<color_yellow>%s</color>] Switch tabs, "
                                     "[<color_yellow>%s</color>] Toggle fuel saving mode, [<color_yellow>%s</color>] Toggle sprite visibility, "
                                     "[<color_yellow>%s</color>] Toggle shutdown on empty, " ),
                                  ctxt.get_desc( "REASSIGN" ), ctxt.get_desc( "NEXT_TAB" ), ctxt.get_desc( "TOGGLE_SAFE_FUEL" ),
                                  ctxt.get_desc( "TOGGLE_SPRITE" ), ctxt.get_desc( "TOGGLE_SHUTDOWN" ) );
    desc_append += string_format( _( " [<color_yellow>%s</color>] Sort: %s" ), ctxt.get_desc( "SORT" ),
                                  sort_mode_str( uistate.bionic_sort_mode ) );
    std::string desc;
    if( mode == REASSIGNING ) {
        desc = _( "Reassigning.  Select a bionic to reassign or press [<color_yellow>SPACE</color>] to cancel." );
        fuel_string.clear();
    } else if( mode == ACTIVATING ) {
        desc = string_format( _( "<color_green>Activating</color>  "
                                 "[<color_yellow>%s</color>] Examine, %s" ),
                              ctxt.get_desc( "TOGGLE_EXAMINE" ), desc_append );
    } else if( mode == EXAMINING ) {
        desc = string_format( _( "<color_light_blue>Examining</color>  "
                                 "[<color_yellow>%s</color>] Activate, %s" ),
                              ctxt.get_desc( "TOGGLE_EXAMINE" ), desc_append );
    }

    return "<div class=\"hbox\"><div class=\"grow\"><div>" + rml_ui::markup_to_rml( desc, c_white ) +
           "</div><div>" + rml_ui::markup_to_rml( fuel_string, c_white ) +
           "</div></div><div class=\"fixed bio-power\">" + power_rml + "</div></div>";
}

//builds the power usage string of a given bionic
static std::string build_bionic_poweronly_string( const bionic &bio, avatar *p )
{
    const bionic_data &bio_data = bio.id.obj();
    std::vector<std::string> properties;

    if( bio_data.has_flag( json_flag_BIONIC_GUN ) && bio.get_weapon().get_gun_bionic_drain() > 0_kJ ) {
        properties.push_back( string_format( _( "%s act" ),
                                             units::display( bio.get_weapon().get_gun_bionic_drain() ) ) );
    } else if( bio_data.power_activate > 0_kJ ) {
        properties.push_back( string_format( _( "%s act" ),
                                             units::display( bio_data.power_activate ) ) );
    }
    if( bio_data.power_deactivate > 0_kJ ) {
        properties.push_back( string_format( _( "%s deact" ),
                                             units::display( bio_data.power_deactivate ) ) );
    }
    if( bio_data.power_trigger > 0_kJ ) {
        properties.push_back( string_format( _( "%s trigger" ),
                                             units::display( bio_data.power_trigger ) ) );
    }
    if( bio_data.charge_time > 0_turns && bio_data.power_over_time > 0_kJ ) {
        properties.push_back( bio_data.charge_time == 1_turns
                              ? string_format( _( "%s/turn" ), units::display( bio_data.power_over_time ) )
                              : string_format( _( "%s/%d turns" ), units::display( bio_data.power_over_time ),
                                               to_turns<int>( bio_data.charge_time ) ) );
    }
    if( bio_data.has_flag( json_flag_BIONIC_TOGGLED ) ) {
        properties.emplace_back( bio.powered ? _( "ON" ) : _( "OFF" ) );
    }
    if( bio.incapacitated_time > 0_turns ) {
        properties.emplace_back( _( "(incapacitated)" ) );
    }
    if( !bio.auto_shutdown ) {
        properties.emplace_back( _( "(shutdown off)" ) );
    }
    if( !bio.show_sprite ) {
        properties.emplace_back( _( "(hidden)" ) );
    }

    if( bio.is_safe_fuel_on() ) {
        const std::string label = string_format( _( "(fuel saving ON > %d %%)" ),
                                  static_cast<int>( bio.get_safe_fuel_thresh() * 100 ) );
        properties.push_back( label );

        if( bio.powered &&
            bio.get_safe_fuel_thresh() * p->get_max_power_level() - 1_kJ <= p->get_power_level() ) {
            properties.emplace_back( _( "(inactive)" ) );
        }
    }

    return enumerate_as_string( properties, enumeration_conjunction::none );
}

//generates the string that show how much power a bionic uses
static std::string build_bionic_powerdesc_string( const bionic &bio, avatar *p )
{
    std::string power_desc;
    const std::string power_string = build_bionic_poweronly_string( bio, p );
    power_desc += bio.id->name.translated();
    if( !power_string.empty() ) {
        power_desc += ", " + power_string;
    }
    return power_desc;
}

static std::vector<std::string> bionics_tab_names( const size_t active_num,
        const size_t passive_num )
{
    return {
        string_format( _( "ACTIVE (%i)" ), active_num ),
        string_format( _( "PASSIVE (%i)" ), passive_num )
    };
}

static std::string bionic_description_rml( const bionic &bio, avatar *p )
{
    const std::string poweronly_string = build_bionic_poweronly_string( bio, p );
    std::string rml = "<div>" + rml_ui::colored_rml( bio.id->name.translated(), c_white ) + "</div>";
    if( !poweronly_string.empty() ) {
        rml += "<div>" + rml_ui::markup_to_rml( string_format( _( "Power usage: %s" ), poweronly_string ),
                                                c_light_gray ) + "</div>";
    }
    rml += "<div class=\"bio-desc\">" + rml_ui::markup_to_rml( bio.id->description.translated(),
            c_light_blue ) + "</div>";

    // TODO: Unhide when enforcing limits
    if( get_option < bool >( "CBM_SLOTS_ENABLED" ) ) {
        rml += "<div>" + rml_ui::markup_to_rml( list_occupied_bps( bio.id,
                                                _( "This bionic occupies the following body parts:" ), true ), c_light_gray ) + "</div>";
    }

    if( bio.has_weapon() ) {
        rml += "<div>" + rml_ui::markup_to_rml( string_format( _( "Installed weapon: %s" ),
                                                bio.get_weapon().tname() ), c_light_gray ) + "</div>";
    }
    return rml;
}

namespace
{
/** The bionics menu, from data/raw/rmlui/bionics.rml; power_bionics() fills it. */
class bionics_window : public rml_ui::window
{
    public:
        explicit bionics_window( const std::function<void( bionics_window & )> &update ) :
            rml_ui::window( _( "Bionics" ), "bionics.rml" ), update( update ) {}

        void reveal( const std::string &id ) {
            scroll_into_view( id );
        }
        void set_hidden( bool hidden ) {
            set_class( "window", "hidden", hidden );
        }

    protected:
        void on_redraw() override {
            update( *this );
        }

    private:
        std::function<void( bionics_window & )> update;
};
} // namespace

//get a text color depending on the power/powering state of the bionic
static nc_color get_bionic_text_color( const bionic &bio, const bool isHighlightedBionic )
{
    nc_color type = c_white;
    bool is_power_source = bio.id->has_flag( json_flag_BIONIC_POWER_SOURCE );
    if( bio.id->activated ) {
        if( isHighlightedBionic ) {
            if( bio.powered && !is_power_source ) {
                type = h_red;
            } else if( is_power_source && !bio.powered ) {
                type = h_light_cyan;
            } else if( is_power_source && bio.powered ) {
                type = h_light_green;
            } else {
                type = h_light_red;
            }
        } else {
            if( bio.powered && !is_power_source ) {
                type = c_red;
            } else if( is_power_source && !bio.powered ) {
                type = c_light_cyan;
            } else if( is_power_source && bio.powered ) {
                type = c_light_green;
            } else {
                type = c_light_red;
            }
        }
    } else {
        if( isHighlightedBionic ) {
            if( is_power_source ) {
                type = h_light_cyan;
            } else {
                type = h_cyan;
            }
        } else {
            if( is_power_source ) {
                type = c_light_cyan;
            } else {
                type = c_cyan;
            }
        }
    }
    return type;
}

void avatar::power_bionics()
{
    // Required because available power includes electricity via cables.
    const map &here = get_map();

    sorted_bionics passive = filtered_bionics( *my_bionics, TAB_PASSIVE );
    sorted_bionics active = filtered_bionics( *my_bionics, TAB_ACTIVE );
    bionic *bio_last = nullptr;
    bionic_tab_mode tab_mode = TAB_ACTIVE;

    bool hide = false;
    int cursor = 0;

    //generate the tab title string and a count of the bionics owned
    bionic_menu_mode menu_mode = ACTIVATING;

    input_context ctxt( "BIONICS", keyboard_mode::keychar );
    ctxt.register_updown();
    ctxt.register_action( "ANY_INPUT" );
    ctxt.register_action( "TOGGLE_EXAMINE" );
    ctxt.register_action( "REASSIGN" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "TOGGLE_SAFE_FUEL" );
    ctxt.register_action( "TOGGLE_SHUTDOWN" );
    ctxt.register_action( "TOGGLE_SPRITE" );
    ctxt.register_action( "SORT" );
    // Clicks on the tabs, the bionics and the close button
    ctxt.register_action( "SELECT" );

    // The cursor as last shown, to keep it in view
    std::pair<bionic_tab_mode, int> shown_cursor( tab_mode, -1 );

    std::unique_ptr<bionics_window> window = std::make_unique<bionics_window>(
    [&]( bionics_window & win ) {
        win.set_hidden( hide );
        if( hide ) {
            return;
        }
        win.set_size( rml_ui::term_width( FULL_SCREEN_WIDTH + ( TERMX - FULL_SCREEN_WIDTH ) / 2 ), "90%" );
        win.set_body_scrolls( false );
        win.set_tabs( bionics_tab_names( active.size(), passive.size() ), tab_mode == TAB_ACTIVE ? 0 : 1 );
        win.set_header( bionics_titlebar_rml( this, menu_mode ) );

        sorted_bionics *current_bionic_list = tab_mode == TAB_ACTIVE ? &active : &passive;

        // The bionics in this tab
        std::string list;
        if( current_bionic_list->empty() ) {
            std::string msg;
            switch( tab_mode ) {
                case TAB_ACTIVE:
                    msg = _( "No activatable bionics installed." );
                    break;
                case TAB_PASSIVE:
                    msg = _( "No passive bionics installed." );
                    break;
            }
            list = rml_ui::colored_rml( msg, c_light_gray );
        } else {
            for( size_t i = 0; i < current_bionic_list->size(); i++ ) {
                const bionic &bio = *( *current_bionic_list )[i];
                const std::string desc = string_format( "%c %s", bio.invlet,
                                                        build_bionic_powerdesc_string( bio, this ) );
                list += string_format( "<div id=\"bio-row-%d\" class=\"item line%s\" data-row=\"%d\">",
                                       static_cast<int>( i ), cursor == static_cast<int>( i ) ? " selected" : "",
                                       static_cast<int>( i ) ) +
                        rml_ui::colored_rml( desc, get_bionic_text_color( bio, false ) ) + "</div>";
            }
        }

        // Beside it, the selected bionic's description, or the body parts'
        // slots with those it takes marked
        std::string side;
        const bionic *selected = current_bionic_list->empty() ? nullptr :
                                 ( *current_bionic_list )[std::min<size_t>( cursor, current_bionic_list->size() - 1 )];
        if( menu_mode == EXAMINING && selected != nullptr ) {
            side = bionic_description_rml( *selected, this );
        } else if( get_option < bool >( "CBM_SLOTS_ENABLED" ) ) {
            for( const bodypart_id &bp : get_all_body_parts() ) {
                const int total = get_total_bionics_slots( bp );
                std::string slots = string_format( "%s: %d/%d", body_part_name_as_heading( bp, 1 ),
                                                   total - get_free_bionics_slots( bp ), total );
                nc_color color = c_light_gray;
                if( selected != nullptr ) {
                    const auto occupied = selected->id->occupied_bodyparts.find( bp.id() );
                    if( occupied != selected->id->occupied_bodyparts.end() ) {
                        slots += string_format( " (%d)", occupied->second );
                        color = c_yellow;
                    }
                }
                side += "<div class=\"line\">" + rml_ui::colored_rml( slots, color ) + "</div>";
            }
        }

        win.set_body( "<div class=\"panes\"><div class=\"pane\">" + list +
                      "</div><div class=\"pane bio-side\">" + side + "</div></div>" );

        if( shown_cursor != std::make_pair( tab_mode, cursor ) ) {
            shown_cursor = std::make_pair( tab_mode, cursor );
            win.reveal( "bio-row-" + std::to_string( cursor ) );
        }
    } );

    for( ;; ) {
        ui_manager::redraw();

        //track which list we are looking at
        ::sorted_bionics *current_bionic_list = tab_mode == TAB_ACTIVE ? &active : &passive;
        cursor = clamp<int>( cursor, 0, current_bionic_list->size() );

#if defined(__ANDROID__)
        ctxt.get_registered_manual_keys().clear();
        for( size_t i = 0; i < current_bionic_list->size(); i++ ) {
            ctxt.register_manual_key( ( *current_bionic_list )[i]->invlet,
                                      build_bionic_powerdesc_string( *( *current_bionic_list )[i], this ) );
        }
#endif

        std::string action = ctxt.handle_input();
        const int ch = ctxt.get_raw_input().get_first_input();
        bionic *tmp = nullptr;

        if( action == "SELECT" ) {
            const std::string clicked = window->resolve_click( action );
            const int tab = window->hovered_tab();
            const int row = window->hovered_index( "data-row" );
            if( clicked != action ) {
                // the close button
                action = clicked;
            } else if( tab >= 0 ) {
                if( ( tab == 0 ) != ( tab_mode == TAB_ACTIVE ) ) {
                    action = "NEXT_TAB";
                }
            } else if( row >= 0 && row < static_cast<int>( current_bionic_list->size() ) ) {
                // A click picks a bionic, and a click on the picked one uses it
                if( row == cursor ) {
                    action = "CONFIRM";
                } else {
                    cursor = row;
                    continue;
                }
            }
        }

        if( action == "DOWN" ) {
            if( static_cast<size_t>( cursor ) < current_bionic_list->size() - 1 ) {
                cursor++;
            } else {
                cursor = 0;
            }
        } else if( action == "UP" ) {
            if( cursor > 0 ) {
                cursor--;
            } else {
                cursor = current_bionic_list->size() - 1;
            }
        } else if( menu_mode == REASSIGNING ) {
            menu_mode = ACTIVATING;

            if( action == "CONFIRM" && !current_bionic_list->empty() ) {
                sorted_bionics &bio_list = tab_mode == TAB_ACTIVE ? active : passive;
                tmp = bio_list[cursor];
            } else {
                tmp = bionic_by_invlet( ch );
            }

            if( tmp == nullptr ) {
                // Selected an non-existing bionic (or Escape, or ...)
                continue;
            }
            const int newch = popup_getkey( _( "%s; enter new letter.  Space to clear.  Esc to cancel." ),
                                            tmp->id->name );
            if( newch == ch || newch == KEY_ESCAPE ) {
                continue;
            }
            if( newch == ' ' ) {
                tmp->invlet = ' ';
                continue;
            }
            if( !bionic_chars.valid( newch ) ) {
                popup( _( "Invalid bionic letter.  Only those characters are valid:\n\n%s" ),
                       bionic_chars.get_allowed_chars() );
                continue;
            }
            bionic *otmp = bionic_by_invlet( newch );
            if( otmp != nullptr ) {
                std::swap( tmp->invlet, otmp->invlet );
            } else {
                tmp->invlet = newch;
            }
            // TODO: show a message like when reassigning a key to an item?
        } else if( action == "NEXT_TAB" ) {
            cursor = 0;
            if( tab_mode == TAB_ACTIVE ) {
                tab_mode = TAB_PASSIVE;
            } else {
                tab_mode = TAB_ACTIVE;
            }
        } else if( action == "PREV_TAB" ) {
            cursor = 0;
            if( tab_mode == TAB_PASSIVE ) {
                tab_mode = TAB_ACTIVE;
            } else {
                tab_mode = TAB_PASSIVE;
            }
        } else if( action == "REASSIGN" ) {
            menu_mode = REASSIGNING;
        } else if( action == "TOGGLE_EXAMINE" ) {
            // switches between activation and examination
            menu_mode = menu_mode == ACTIVATING ? EXAMINING : ACTIVATING;
        } else if( action == "TOGGLE_SAFE_FUEL" ) {
            sorted_bionics &bio_list = tab_mode == TAB_ACTIVE ? active : passive;
            if( !current_bionic_list->empty() ) {
                tmp = bio_list[cursor];
                if( !tmp->info().fuel_opts.empty() || tmp->info().is_remote_fueled ) {
                    tmp->toggle_safe_fuel_mod();
                    g->invalidate_main_ui_adaptor();
                } else {
                    popup( _( "You can't toggle fuel saving mode on a non-fueled CBM." ) );
                }
            }
        } else if( action == "TOGGLE_SHUTDOWN" ) {
            sorted_bionics &bio_list = tab_mode == TAB_ACTIVE ? active : passive;
            if( !current_bionic_list->empty() ) {
                tmp = bio_list[cursor];
                if( !tmp->info().fuel_opts.empty() || tmp->info().is_remote_fueled ) {
                    tmp->auto_shutdown = !tmp->auto_shutdown;
                    g->invalidate_main_ui_adaptor();
                } else {
                    popup( _( "You can't toggle shutdown on empty mode on a non-fueled CBM." ) );
                }
            }
        } else if( action == "TOGGLE_SPRITE" ) {
            sorted_bionics &bio_list = tab_mode == TAB_ACTIVE ? active : passive;
            if( !current_bionic_list->empty() ) {
                tmp = bio_list[cursor];
                tmp->show_sprite = !tmp->show_sprite;
            }
        } else if( action == "SORT" ) {
            uistate.bionic_sort_mode = pick_sort_mode();
            // FIXME: is there a better way to resort?
            active = filtered_bionics( *my_bionics, TAB_ACTIVE );
            passive = filtered_bionics( *my_bionics, TAB_PASSIVE );
        } else if( action == "CONFIRM" || action == "ANY_INPUT" ) {
            sorted_bionics &bio_list = tab_mode == TAB_ACTIVE ? active : passive;
            if( action == "CONFIRM" && !current_bionic_list->empty() ) {
                tmp = bio_list[cursor];
            } else {
                tmp = bionic_by_invlet( ch );
                if( tmp && tmp != bio_last ) {
                    // new bionic selected, update cursor and scroll position
                    int temp_cursor = 0;
                    for( temp_cursor = 0; temp_cursor < static_cast<int>( bio_list.size() ); temp_cursor++ ) {
                        if( bio_list[temp_cursor] == tmp ) {
                            break;
                        }
                    }
                    // if bionic is not found in current list, ignore the attempt to view/activate
                    if( temp_cursor >= static_cast<int>( bio_list.size() ) ) {
                        continue;
                    }
                    //relocate cursor to the bionic that was found
                    cursor = temp_cursor;
                }
            }
            if( !tmp ) {
                // entered a key that is not mapped to any bionic
                continue;
            }
            bio_last = tmp;
            const bionic_id &bio_id = tmp->id;
            const bionic_data &bio_data = bio_id.obj();
            if( menu_mode == ACTIVATING ) {
                if( bio_data.activated ) {
                    int b = tmp - my_bionics->data();
                    bionic &bio = ( *my_bionics )[b];
                    hide = true;
                    if( tmp->powered ) {
                        deactivate_bionic( bio );
                        if( bio_data.deactivated_close_ui ) {
                            window.reset();
                            break;
                        }
                    } else {
                        bool activate = true;

                        if( bio.can_install_weapon() ) {
                            const bool has_weapon = bio.has_weapon();
                            activate = false;

                            uilist activate_action_menu;
                            activate_action_menu.title = _( "Select action" );
                            activate_action_menu.addentry( 0, has_weapon, 'a', _( "Activate" ) );
                            activate_action_menu.addentry( 1, has_weapon, 'u', _( "Uninstall weapon" ) );
                            activate_action_menu.addentry( 2, !has_weapon, 'i', _( "Install weapon" ) );

                            activate_action_menu.query();

                            switch( activate_action_menu.ret ) {
                                case 0:
                                    activate = true;
                                    break;
                                case 1:
                                    // TODO: Move to function, create activity and add tool requirements
                                    if( std::optional<item> weapon = bio.uninstall_weapon() ) {
                                        i_add_or_drop( *weapon );
                                    }
                                    break;
                                case 2: {
                                    // TODO: Move to activity, create activity, add tool requirements, improve UI
                                    uilist install_weapon_menu;
                                    install_weapon_menu.title = _( "Select weapon to install" );
                                    // TODO: Choose from items around, obtain items before installing
                                    std::vector<item *> valid_weapons = items_with( [&bio]( const item & it ) {
                                        return it.has_any_flag( bio.id->installable_weapon_flags );
                                    } );
                                    for( size_t i = 0; i < valid_weapons.size(); i++ ) {
                                        install_weapon_menu.addentry( i, true, MENU_AUTOASSIGN, valid_weapons[i]->tname() );
                                    }
                                    if( install_weapon_menu.entries.empty() ) {
                                        popup( _( "You don't have any items you can install in this bionic" ) );
                                    } else {
                                        install_weapon_menu.query();
                                        if( install_weapon_menu.ret >= 0 &&
                                            static_cast<size_t>( install_weapon_menu.ret ) < valid_weapons.size() ) {
                                            item &new_weapon = *valid_weapons[install_weapon_menu.ret];
                                            if( bio.can_install_weapon( new_weapon ) && bio.install_weapon( new_weapon ) ) {
                                                item_location loc( *this, &new_weapon );
                                                loc.remove_item();
                                            } else {
                                                popup( _( "Unable to install %s" ), new_weapon.tname() );
                                            }
                                        }
                                    }
                                    break;
                                }
                                default:
                                    break;
                            }
                        }

                        if( activate ) {
                            bool close_ui = false;
                            if( bio_data.activated_close_ui ) {
                                window.reset();
                                activate_bionic( bio, false, &close_ui );
                                break;
                            } else {
                                activate_bionic( bio, false, &close_ui );
                                // Exit this ui if we are firing a complex bionic
                                if( close_ui && tmp->get_weapon().shots_remaining( here, this ) > 0 ) {
                                    break;
                                }
                            }
                        }
                    }
                    hide = false;
                    g->invalidate_main_ui_adaptor();
                    if( moves < 0 ) {
                        return;
                    }
                    continue;
                } else {
                    popup( _( "You can not activate %s!\n"
                              "To read a description of %s, press '%s', then '%c'." ), bio_data.name,
                           bio_data.name, ctxt.get_desc( "TOGGLE_EXAMINE" ), tmp->invlet );
                }
            } else if( menu_mode == EXAMINING ) {
                // Describing bionics, allow user to jump to description key
                if( action != "CONFIRM" ) {
                    for( size_t i = 0; i < active.size(); i++ ) {
                        if( active[i] == tmp ) {
                            tab_mode = TAB_ACTIVE;
                            cursor = static_cast<int>( i );
                            break;
                        }
                    }
                    for( size_t i = 0; i < passive.size(); i++ ) {
                        if( passive[i] == tmp ) {
                            tab_mode = TAB_PASSIVE;
                            cursor = static_cast<int>( i );
                            break;
                        }
                    }
                }
            }
        } else if( action == "QUIT" ) {
            break;
        }
    }
}

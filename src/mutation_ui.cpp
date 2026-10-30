#include "character.h" // IWYU pragma: associated

#include <algorithm> //std::min
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "avatar.h"
#include "color.h"
#include "cursesdef.h"
#include "enums.h"
#include "input_context.h"
#include "inventory.h"
#include "magic.h"
#include "mutation.h"
#include "output.h"
#include "popup.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"
namespace
{
enum class mutation_menu_mode {
    activating,
    examining,
    reassigning,
    hiding
};
enum class mutation_tab_mode {
    active,
    passive,
    none
};
} // namespace
// '!' and '=' are uses as default bindings in the menu
static const invlet_wrapper
mutation_chars( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\"#&()*+./:;@[\\]^_{|}" );

namespace
{
/** The mutations menu, from data/raw/rmlui/mutations.rml; power_mutations() fills it. */
class mutations_window : public rml_ui::window
{
    public:
        explicit mutations_window( const std::function<void( mutations_window & )> &update ) :
            rml_ui::window( _( "Mutations" ), "mutations.rml" ), update( update ) {
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
        std::function<void( mutations_window & )> update;
};
} // namespace

static const auto shortcut_desc = []( std::string_view comment, const std::string &keys )
{
    return string_format( comment, string_format( "[<color_yellow>%s</color>]", keys ) );
};

// needs extensive improvement

static std::optional<trait_id> GetTrait( const std::vector<trait_id> &active,
        const std::vector<trait_id> &passive,
        int cursor, mutation_tab_mode tab_mode )
{
    std::optional<trait_id> mut_id;
    if( tab_mode == mutation_tab_mode::active ) {
        mut_id = active[cursor];
    } else if( tab_mode == mutation_tab_mode::passive ) {
        mut_id = passive[cursor];
    }
    return mut_id;
}

/** The header: what the keys do in this mode. */
static std::string mutations_titlebar_rml( const mutation_menu_mode menu_mode,
        const input_context &ctxt )
{
    std::string desc;
    if( menu_mode == mutation_menu_mode::reassigning ) {
        desc += std::string( _( "Reassigning." ) ) + "  " +
                _( "Select a mutation to reassign or press [<color_yellow>SPACE</color>] to cancel. " );
    }
    if( menu_mode == mutation_menu_mode::activating ) {
        desc += colorize( _( "Activating" ),
                          c_green ) + "  " + shortcut_desc( _( "%s Examine, " ),
                                  ctxt.get_desc( "TOGGLE_EXAMINE" ) );
    }
    if( menu_mode == mutation_menu_mode::examining ) {
        desc += colorize( _( "Examining" ),
                          c_light_blue ) + "  " + shortcut_desc( _( "%s Activate, " ),
                                  ctxt.get_desc( "TOGGLE_EXAMINE" ) );
    }
    if( menu_mode == mutation_menu_mode::hiding ) {
        desc += colorize( _( "Hiding" ), c_cyan ) + "  " + shortcut_desc( _( "%s Activate, " ),
                ctxt.get_desc( "TOGGLE_EXAMINE" ) );
    }
    if( menu_mode != mutation_menu_mode::reassigning ) {
        desc += shortcut_desc( _( "%s Reassign, " ), ctxt.get_desc( "REASSIGN" ) );
    }
    desc += shortcut_desc( _( "%s Toggle sprite visibility, " ), ctxt.get_desc( "TOGGLE_SPRITE" ) );
    desc += shortcut_desc( _( "%s Change keybindings." ), ctxt.get_desc( "HELP_KEYBINDINGS" ) );
    return rml_ui::markup_to_rml( desc, c_white );
}

void avatar::power_mutations()
{
    std::vector<trait_id> passive;
    std::vector<trait_id> active;
    for( std::pair<const trait_id, trait_data> &mut : cached_mutations ) {
        if( mut.second.corrupted > 0 ) {
            continue;
        }
        if( !mut.first->player_display ) {
            continue;
        }
        if( !mut.first->activated ) {
            passive.push_back( mut.first );
        } else {
            active.push_back( mut.first );
        }
        // New mutations are initialized with no key at all, so we have to do this here.
        if( mut.second.key == ' ' ) {
            for( const char &letter : mutation_chars ) {
                if( trait_by_invlet( letter ).is_null() && mut.first->activated ) {
                    mut.second.key = letter;
                    break;
                }
            }
        }
    }

    int cursor = 0;
    mutation_menu_mode menu_mode = mutation_menu_mode::activating;
    mutation_tab_mode tab_mode;
    if( !active.empty() ) {
        tab_mode = mutation_tab_mode::active;
    } else if( !passive.empty() ) {
        tab_mode = mutation_tab_mode::passive;
    } else {
        tab_mode = mutation_tab_mode::none;
    }

    input_context ctxt( "MUTATIONS", keyboard_mode::keychar );
    ctxt.register_updown();
    ctxt.register_action( "ANY_INPUT" );
    ctxt.register_action( "TOGGLE_EXAMINE" );
    ctxt.register_action( "TOGGLE_SPRITE" );
    ctxt.register_action( "REASSIGN" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "SCROLL_TRAIT_INFO_UP" );
    ctxt.register_action( "SCROLL_TRAIT_INFO_DOWN" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    // Clicks on the mutations and the close button
    ctxt.register_action( "SELECT" );
#if defined(__ANDROID__)
    for( const auto &p : passive ) {
        ctxt.register_manual_key( cached_mutations[p].key, p.obj().name() );
    }
    for( const auto &a : active ) {
        ctxt.register_manual_key( cached_mutations[a].key, a.obj().name() );
    }
#endif

    std::optional<trait_id> examine_id;

    // What was last shown, to keep the cursor in view and new descriptions at their top
    std::pair<mutation_tab_mode, int> shown_cursor( mutation_tab_mode::none, -1 );
    std::optional<trait_id> shown_examine;

    std::unique_ptr<mutations_window> window = std::make_unique<mutations_window>(
    [&]( mutations_window & win ) {
        win.set_size( rml_ui::term_width( FULL_SCREEN_WIDTH + ( TERMX - FULL_SCREEN_WIDTH ) / 2 ),
                      "90%" );
        win.set_header( mutations_titlebar_rml( menu_mode, ctxt ) );

        const auto row_rml = [&]( const char *side, int i, bool selected, const std::string & rml ) {
            return string_format( "<div id=\"mut-%s-%d\" class=\"item line%s\" data-side=\"%s\" data-row=\"%d\">",
                                  side, i, selected ? " selected" : "", side[0] == 'p' ? "0" : "1", i ) + rml + "</div>";
        };

        std::string passive_rml;
        if( passive.empty() ) {
            passive_rml = rml_ui::colored_rml( _( "None" ), c_light_gray );
        }
        for( int i = 0; static_cast<size_t>( i ) < passive.size(); i++ ) {
            const mutation_branch &md = passive[i].obj();
            const trait_data &td = cached_mutations[passive[i]];
            const nc_color type = has_base_trait( passive[i] ) ? c_cyan : c_light_cyan;
            std::string rml = rml_ui::colored_rml( string_format( "%c %s", td.key, mutation_name( md.id ) ),
                                                   type );
            if( !td.show_sprite ) {
                //~ Hint: Letter to show which mutation is Hidden in the mutation menu
                rml += rml_ui::colored_rml( _( " H" ), c_cyan );
            }
            passive_rml += row_rml( "p", i, cursor == i && tab_mode == mutation_tab_mode::passive, rml );
        }

        std::string active_rml;
        if( active.empty() ) {
            active_rml = rml_ui::colored_rml( _( "None" ), c_light_gray );
        }
        for( int i = 0; static_cast<size_t>( i ) < active.size(); i++ ) {
            const mutation_branch &md = active[i].obj();
            const trait_data &td = cached_mutations[active[i]];
            nc_color type;
            if( td.powered ) {
                type = has_base_trait( active[i] ) ? c_green : c_light_green;
            } else {
                type = has_base_trait( active[i] ) ? c_red : c_light_red;
            }
            // TODO: track resource(s) used and specify
            std::string mut_desc;
            std::string resource_unit;
            int number_of_resource = 0;
            if( md.hunger ) {
                resource_unit += _( " kcal" );
                number_of_resource++;
            }
            if( md.thirst ) {
                if( number_of_resource > 0 ) {
                    //~ Resources consumed by a mutation: "kcal & thirst & sleepiness & mana & stamina"
                    resource_unit += _( " &" );
                }
                resource_unit += _( " thirst" );
                number_of_resource++;
            }
            if( md.sleepiness ) {
                if( number_of_resource > 0 ) {
                    //~ Resources consumed by a mutation: "kcal & thirst & sleepiness & mana & stamina"
                    resource_unit += _( " &" );
                }
                resource_unit += _( " sleepiness" );
            }
            if( md.stamina ) {
                if( number_of_resource > 0 ) {
                    //~ Resources consumed by a mutation: "kcal & thirst & sleepiness & mana & stamina"
                    resource_unit += _( " &" );
                }
                resource_unit += _( " stamina" );
            }
            if( md.mana ) {
                resource_unit += _( " mana" );
                number_of_resource++;
            }
            mut_desc += mutation_name( md.id );
            if( md.cost > 0 && md.cooldown > 0_turns ) {
                mut_desc += string_format( _( " - %d%s / %s" ),
                                           md.cost, resource_unit, to_string_clipped( md.cooldown ) );
            } else if( md.cost > 0 ) {
                mut_desc += string_format( _( " - %d%s" ), md.cost, resource_unit );
            } else if( md.cooldown > 0_turns ) {
                mut_desc += string_format( _( " - %s" ), to_string_clipped( md.cooldown ) );
            }
            if( td.powered ) {
                mut_desc += _( " - Active" );
            }
            active_rml += row_rml( "a", i, cursor == i && tab_mode == mutation_tab_mode::active,
                                   rml_ui::colored_rml( string_format( "%c %s", td.key, mut_desc ), type ) );
        }

        std::string body = "<div class=\"panes\"><div class=\"pane\"><div class=\"mut-caption\">" +
                           rml_ui::colored_rml( _( "Passive:" ), c_light_blue ) + "</div>" + passive_rml +
                           "</div><div class=\"pane\"><div class=\"mut-caption\">" +
                           rml_ui::colored_rml( _( "Active:" ), c_light_blue ) + "</div>" + active_rml + "</div></div>";
        if( menu_mode == mutation_menu_mode::examining && examine_id.has_value() ) {
            std::string description = mutation_desc( examine_id.value() );
            if( !purifiable( examine_id.value() ) ) {
                description +=
                    _( "\n<color_yellow>This trait is an intrinsic part of you now, purifier won't be able to remove it.</color>" );
            }
            body += "<div id=\"mut-desc\">" + rml_ui::markup_to_rml( description, c_light_blue ) + "</div>";
        }
        win.set_body( body );

        if( shown_cursor != std::make_pair( tab_mode, cursor ) ) {
            shown_cursor = std::make_pair( tab_mode, cursor );
            win.reveal( string_format( "mut-%s-%d", tab_mode == mutation_tab_mode::passive ? "p" : "a",
                                       cursor ) );
        }
        if( shown_examine != examine_id ) {
            shown_examine = examine_id;
            win.scroll_pane( "mut-desc", rml_ui::scroll::top );
        }
    } );

    bool exit = false;
    while( !exit ) {
        ui_manager::redraw();
        bool handled = false;
        std::string action = ctxt.handle_input();
        const input_event evt = ctxt.get_raw_input();
        if( action == "SELECT" && window ) {
            const std::string clicked = window->resolve_click( action );
            const int side = window->hovered_index( "data-side" );
            const int row = window->hovered_index( "data-row" );
            if( clicked != action ) {
                // the close button
                action = clicked;
            } else if( side >= 0 && row >= 0 ) {
                // A click picks a mutation, and a click on the picked one acts on it
                const mutation_tab_mode clicked_tab = side == 0 ? mutation_tab_mode::passive :
                                                      mutation_tab_mode::active;
                if( clicked_tab == tab_mode && row == cursor ) {
                    action = "CONFIRM";
                } else {
                    tab_mode = clicked_tab;
                    cursor = row;
                    examine_id = GetTrait( active, passive, cursor, tab_mode );
                    continue;
                }
            }
        }
        if( evt.type == input_event_t::keyboard_char && !evt.sequence.empty() ) {
            const int ch = evt.get_first_input();
            if( ch == ' ' ) { //skip if space is pressed (space is used as an empty hotkey)
                continue;
            }
            const trait_id mut_id = trait_by_invlet( ch );
            if( !mut_id.is_null() ) {
                const mutation_branch &mut_data = mut_id.obj();
                switch( menu_mode ) {
                    case mutation_menu_mode::reassigning: {
                        query_popup pop;
                        pop.message( _( "%s; enter new letter." ), mutation_name( mut_id ) )
                        .preferred_keyboard_mode( keyboard_mode::keychar )
                        .context( "POPUP_WAIT" )
                        .allow_cancel( true )
                        .allow_anykey( true );

                        bool pop_exit = false;
                        while( !pop_exit ) {
                            const query_popup::result ret = pop.query();
                            bool pop_handled = false;
                            if( ret.evt.type == input_event_t::keyboard_char && !ret.evt.sequence.empty() ) {
                                const int newch = ret.evt.get_first_input();
                                if( mutation_chars.valid( newch ) ) {
                                    const trait_id other_mut_id = trait_by_invlet( newch );
                                    if( !other_mut_id.is_null() ) {
                                        std::swap( cached_mutations[mut_id].key, cached_mutations[other_mut_id].key );
                                    } else {
                                        cached_mutations[mut_id].key = newch;
                                    }
                                    pop_exit = true;
                                    pop_handled = true;
                                }
                            }
                            if( !pop_handled ) {
                                if( ret.action == "QUIT" ) {
                                    pop_exit = true;
                                } else if( ret.action != "HELP_KEYBINDINGS" &&
                                           ret.evt.type == input_event_t::keyboard_char ) {
                                    popup( _( "Invalid mutation letter.  Only those characters are valid:\n\n%s" ),
                                           mutation_chars.get_allowed_chars() );
                                }
                            }
                        }

                        menu_mode = mutation_menu_mode::activating;
                        examine_id = std::nullopt;
                        // TODO: show a message like when reassigning a key to an item?
                        break;
                    }
                    case mutation_menu_mode::activating: {
                        if( mut_data.activated ) {
                            if( cached_mutations[mut_id].powered ) {
                                add_msg_if_player( m_neutral, _( "You stop using your %s." ), mutation_name( mut_data.id ) );
                                // Reset menu in advance
                                window.reset();
                                deactivate_mutation( mut_id );
                                // Action done, leave screen
                                exit = true;
                            } else if( ( !mut_data.hunger || get_kcal_percent() >= 0.8f ) &&
                                       ( !mut_data.thirst || get_thirst() <= 400 ) &&
                                       ( !mut_data.sleepiness || get_sleepiness() <= 400 ) &&
                                       ( !mut_data.mana || magic->available_mana() >= mut_data.cost ) &&
                                       ( !mut_data.stamina || get_stamina() >= mut_data.cost ) ) {
                                add_msg_if_player( m_neutral,
                                                   string_format( mut_data.activation_msg.translated(), mutation_name( mut_data.id ) ) );
                                // Reset menu in advance
                                window.reset();
                                activate_mutation( mut_id );
                                // Action done, leave screen
                                exit = true;
                            } else {
                                popup( _( "You don't have enough in you to activate your %s!" ), mutation_name( mut_data.id ) );
                            }
                        } else {
                            popup( _( "You cannot activate %1$s!  To read a description of "
                                      "%1$s, press '%2$s', then '%3$c'." ),
                                   mutation_name( mut_data.id ), ctxt.get_desc( "TOGGLE_EXAMINE" ), cached_mutations[mut_id].key );
                        }
                        break;
                    }
                    case mutation_menu_mode::examining:
                        // Describing mutations, not activating them!
                        examine_id = mut_id;
                        break;
                    case mutation_menu_mode::hiding:
                        cached_mutations[mut_id].show_sprite = !cached_mutations[mut_id].show_sprite;
                        break;
                }
                handled = true;
            } else if( mutation_chars.valid( ch ) ) {
                handled = true;
            }
        }
        if( !handled ) {

            // Essentially, up-down navigation adapted from the bionics_ui.cpp, with a bunch of extra workarounds to keep functionality

            if( action == "DOWN" ) {

                int lowerlim;

                if( tab_mode == mutation_tab_mode::passive ) {
                    lowerlim = static_cast<int>( passive.size() ) - 1;
                } else if( tab_mode == mutation_tab_mode::active ) {
                    lowerlim = static_cast<int>( active.size() ) - 1;
                } else {
                    continue;
                }

                if( cursor < lowerlim ) {
                    cursor++;
                } else {
                    cursor = 0;
                }

                // Draw the description, shabby workaround
                examine_id = GetTrait( active, passive, cursor, tab_mode );

            } else if( action == "UP" ) {

                int lim;
                if( tab_mode == mutation_tab_mode::passive ) {
                    lim = passive.size() - 1;
                } else if( tab_mode == mutation_tab_mode::active ) {
                    lim = active.size() - 1;
                } else {
                    continue;
                }
                if( cursor > 0 ) {
                    cursor--;
                } else {
                    cursor = lim;
                }

                examine_id = GetTrait( active, passive, cursor, tab_mode );
            } else if( ( action == "SCROLL_TRAIT_INFO_UP" || action == "SCROLL_TRAIT_INFO_DOWN" ) &&
                       menu_mode == mutation_menu_mode::examining ) {
                window->scroll_pane( "mut-desc", action == "SCROLL_TRAIT_INFO_UP" ?
                                     rml_ui::scroll::line_up : rml_ui::scroll::line_down );
            } else if( action == "NEXT_TAB" || action == "PREV_TAB" ) {
                if( tab_mode == mutation_tab_mode::active && !passive.empty() ) {
                    tab_mode = mutation_tab_mode::passive;
                } else if( tab_mode == mutation_tab_mode::passive && !active.empty() ) {
                    tab_mode = mutation_tab_mode::active;
                } else {
                    continue;
                }
                cursor = 0;
                examine_id = GetTrait( active, passive, cursor, tab_mode );
            } else if( action == "CONFIRM" ) {
                trait_id mut_id;
                if( tab_mode == mutation_tab_mode::active ) {
                    mut_id = active[cursor];
                } else if( tab_mode == mutation_tab_mode::passive ) {
                    mut_id = passive[cursor];
                } else {
                    continue;
                }
                if( !mut_id.is_null() ) {
                    const mutation_branch &mut_data = mut_id.obj();
                    switch( menu_mode ) {
                        case mutation_menu_mode::reassigning: {
                            query_popup pop;
                            pop.message( _( "%s; enter new letter." ), mutation_name( mut_id ) )
                            .preferred_keyboard_mode( keyboard_mode::keychar )
                            .context( "POPUP_WAIT" )
                            .allow_cancel( true )
                            .allow_anykey( true );

                            bool pop_exit = false;
                            while( !pop_exit ) {
                                const query_popup::result ret = pop.query();
                                bool pop_handled = false;
                                if( ret.evt.type == input_event_t::keyboard_char && !ret.evt.sequence.empty() ) {
                                    const int newch = ret.evt.get_first_input();
                                    if( mutation_chars.valid( newch ) ) {
                                        const trait_id other_mut_id = trait_by_invlet( newch );
                                        if( !other_mut_id.is_null() ) {
                                            std::swap( cached_mutations[mut_id].key, cached_mutations[other_mut_id].key );
                                        } else {
                                            cached_mutations[mut_id].key = newch;
                                        }
                                        pop_exit = true;
                                        pop_handled = true;
                                    } else if( newch == ' ' ) {
                                        cached_mutations[mut_id].key = newch;
                                        pop_exit = true;
                                        pop_handled = true;
                                    }
                                }
                                if( !pop_handled ) {
                                    if( ret.action == "QUIT" ) {
                                        pop_exit = true;
                                    } else if( ret.action != "HELP_KEYBINDINGS" &&
                                               ret.evt.type == input_event_t::keyboard_char ) {
                                        popup( _( "Invalid mutation letter.  Only those characters are valid:\n\n%s" ),
                                               mutation_chars.get_allowed_chars() );
                                    }
                                }
                            }

                            menu_mode = mutation_menu_mode::activating;
                            examine_id = std::nullopt;
                            // TODO: show a message like when reassigning a key to an item?
                            break;
                        }
                        case mutation_menu_mode::activating: {
                            if( mut_data.activated ) {
                                if( cached_mutations[mut_id].powered ) {
                                    add_msg_if_player( m_neutral, _( "You stop using your %s." ), mutation_name( mut_data.id ) );
                                    // Reset menu in advance
                                    window.reset();
                                    deactivate_mutation( mut_id );
                                    // Action done, leave screen
                                    exit = true;
                                } else if( ( !mut_data.hunger || get_kcal_percent() >= 0.8f ) &&
                                           ( !mut_data.thirst || get_thirst() <= 400 ) &&
                                           ( !mut_data.sleepiness || get_sleepiness() <= 400 ) &&
                                           ( !mut_data.mana || magic->available_mana() >= mut_data.cost ) ) {
                                    add_msg_if_player( m_neutral,
                                                       string_format( mut_data.activation_msg.translated(), mutation_name( mut_data.id ) ) );
                                    // Reset menu in advance
                                    window.reset();
                                    activate_mutation( mut_id );
                                    // Action done, leave screen
                                    exit = true;
                                } else {
                                    popup( _( "You don't have enough in you to activate your %s!" ), mutation_name( mut_data.id ) );
                                }
                            } else {
                                popup( _( "You cannot activate %1$s!  To read a description of "
                                          "%1$s, press '%2$s'." ),
                                       mutation_name( mut_data.id ), ctxt.get_desc( "TOGGLE_EXAMINE" ) );
                            }
                            break;
                        }
                        case mutation_menu_mode::examining:
                            // Describing mutations, not activating them!
                            examine_id = mut_id;
                            break;
                        case mutation_menu_mode::hiding:
                            cached_mutations[mut_id].show_sprite = !cached_mutations[mut_id].show_sprite;
                            break;
                    }
                }
            } else if( action == "REASSIGN" ) {
                menu_mode = mutation_menu_mode::reassigning;
                examine_id = std::nullopt;
            } else if( action == "TOGGLE_EXAMINE" ) {
                // switches between activation and examination
                menu_mode = menu_mode == mutation_menu_mode::activating ?
                            mutation_menu_mode::examining : mutation_menu_mode::activating;
                if( menu_mode == mutation_menu_mode::examining ) {
                    examine_id = GetTrait( active, passive, cursor, tab_mode );
                } else {
                    examine_id = std::nullopt;
                }
            } else if( action == "TOGGLE_SPRITE" ) {
                menu_mode = mutation_menu_mode::hiding;
                examine_id = std::nullopt;
            } else if( action == "QUIT" ) {
                exit = true;
            }
        }
    }
}

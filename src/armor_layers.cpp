#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "body_part_set.h"
#include "bodypart.h"
#include "catacharset.h"
#include "character.h"
#include "character_attire.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "cursesdef.h"
#include "damage.h"
#include "debug.h"
#include "enums.h"
#include "flag.h"
#include "flat_set.h"
#include "game_inventory.h"
#include "input_context.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "output.h"
#include "pimpl.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "uistate.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"

static const flag_id json_flag_HIDDEN( "HIDDEN" );

namespace
{
std::string clothing_layer( const item &worn_item );
std::vector<std::string> clothing_properties(
    const item &worn_item, int width, const Character &, const bodypart_id &bp );
std::vector<std::string> clothing_protection( const item &worn_item, int width,
        const bodypart_id &bp );
std::vector<std::string> clothing_flags_description( const item &worn_item, int width );

std::string body_part_names( const std::vector<bodypart_id> &parts )
{
    if( parts.empty() ) {
        debugmsg( "Asked for names of empty list" );
        return {};
    }

    std::vector<std::string> names;
    names.reserve( parts.size() );
    for( bodypart_id part : parts ) {
        bool duplicate = false;
        bool can_be_consolidated = false;
        std::string current_part = body_part_name_accusative( part );
        std::string opposite_part = body_part_name_accusative( part->opposite_part );
        std::string part_group = body_part_name_accusative( part, 2 );
        for( const std::string &already_listed : names ) {
            if( already_listed == current_part || already_listed == part_group ) {
                duplicate = true;
                break;
            } else if( already_listed == opposite_part ) {
                can_be_consolidated = true;
                break;
            }
        }
        if( duplicate ) {
            //Do nothing
        } else if( can_be_consolidated ) {
            std::replace( names.begin(), names.end(), opposite_part, part_group );
        } else {
            names.push_back( current_part );
        }
    }

    return enumerate_as_string( names );
}

/**
 * The selected item's details, as RML: its name, then what it adds to
 * encumbrance, its layer and flags, and its properties and protection, the
 * names and values of those lined up for a fixed-width font `win_width` wide.
 */
std::string mid_pane_rml( std::list<item>::const_iterator const worn_item_it,
                          Character &c, const bodypart_id &bp, const int win_width )
{
    const item_penalties penalties = c.worn.get_item_penalties( worn_item_it, c, bp );
    std::vector<std::string> mid_pane_text;
    mid_pane_text.reserve(
        40 ); //Assume 10 for properties, 15 for protection, 5 for layering, 10 for encumbrance
    std::vector<std::string> temp_text;
    temp_text.reserve( 40 );

    if( !penalties.body_parts_with_stacking_penalty.empty() ) {
        std::string layer_description = [&]() -> std::string {
            std::string outstring;
            for( layer_level layer : worn_item_it->get_layer() )
            {
                switch( layer ) {
                    case layer_level::PERSONAL:
                        outstring.append( _( "in your <color_light_blue>personal aura</color>" ) );
                        break;
                    case layer_level::SKINTIGHT:
                        outstring.append( _( "<color_light_blue>close to your skin</color>" ) );
                        break;
                    case layer_level::NORMAL:
                        outstring.append( _( "of <color_light_blue>normal</color> clothing" ) );
                        break;
                    case layer_level::WAIST:
                        outstring.append( _( "on your <color_light_blue>waist</color>" ) );
                        break;
                    case layer_level::OUTER:
                        outstring.append( _( "of <color_light_blue>outer</color> clothing" ) );
                        break;
                    case layer_level::BELTED:
                        outstring.append( _( "<color_light_blue>strapped</color> to you" ) );
                        break;
                    case layer_level::AURA:
                        outstring.append( _( "an <color_light_blue>aura</color> around you" ) );
                        break;
                    default:
                        break;
                }
            }
            return outstring ;
        }
        ();
        std::string body_parts =
            body_part_names( penalties.body_parts_with_stacking_penalty );
        std::string message =
            string_format(
                n_gettext( "Wearing multiple items %s on your "
                           "<color_light_red>%s</color> is adding encumbrance there.",
                           "Wearing multiple items %s on your "
                           "<color_light_red>%s</color> is adding encumbrance there.",
                           penalties.body_parts_with_stacking_penalty.size() ),
                layer_description, body_parts
            );
        temp_text = foldstring( message, win_width );
        mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );
    }

    if( !penalties.body_parts_with_out_of_order_penalty.empty() ) {
        std::string body_parts =
            body_part_names( penalties.body_parts_with_out_of_order_penalty );
        std::string message;

        if( penalties.bad_items_within.empty() ) {
            message = string_format(
                          n_gettext( "Wearing this outside items it would normally be beneath "
                                     "is adding encumbrance to your <color_light_red>%s</color>.",
                                     "Wearing this outside items it would normally be beneath "
                                     "is adding encumbrance to your <color_light_red>%s</color>.",
                                     penalties.body_parts_with_out_of_order_penalty.size() ),
                          body_parts
                      );
        } else {
            std::string bad_item_name = *penalties.bad_items_within.begin();
            message = string_format(
                          n_gettext( "Wearing this outside your <color_light_blue>%s</color> "
                                     "is adding encumbrance to your <color_light_red>%s</color>.",
                                     "Wearing this outside your <color_light_blue>%s</color> "
                                     "is adding encumbrance to your <color_light_red>%s</color>.",
                                     penalties.body_parts_with_out_of_order_penalty.size() ),
                          bad_item_name, body_parts
                      );
        }
        temp_text = foldstring( message, win_width );
        mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );
    }
    temp_text = foldstring( clothing_layer( *worn_item_it ), win_width );
    mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );
    temp_text = clothing_flags_description( *worn_item_it, win_width );
    mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );
    temp_text = clothing_properties( *worn_item_it, win_width - 1, c, bp );
    mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );
    temp_text = clothing_protection( *worn_item_it, win_width - 1, bp );
    mid_pane_text.insert( mid_pane_text.end(), temp_text.begin(), temp_text.end() );

    std::string rml = "<div class=\"sa-name\">" + rml_ui::markup_to_rml( worn_item_it->type_name( 1 ),
                      c_white ) + "</div>";
    for( const std::string &line : mid_pane_text ) {
        rml += "<div>" + rml_ui::markup_to_rml( line, c_light_gray ) + "</div>";
    }
    return rml;
}

std::string clothing_layer( const item &worn_item )
{
    std::string layer;

    if( worn_item.has_flag( flag_PERSONAL ) ) {
        layer = _( "This is in your personal aura." );
    } else if( worn_item.has_flag( flag_SKINTIGHT ) ) {
        layer = _( "This is worn next to the skin." );
    } else if( worn_item.has_flag( flag_WAIST ) ) {
        layer = _( "This is worn on or around your waist." );
    } else if( worn_item.has_flag( flag_OUTER ) ) {
        layer = _( "This is worn over your other clothes." );
    } else if( worn_item.has_flag( flag_BELTED ) ) {
        layer = _( "This is strapped onto you." );
    } else if( worn_item.has_flag( flag_AURA ) ) {
        layer = _( "This is an aura around you." );
    }

    return layer;
}

std::vector<std::string> clothing_properties(
    const item &worn_item, const int width, const Character &c, const bodypart_id &bp )
{
    std::vector<std::string> props;
    bodypart_id used_bp = bp;

    // catch all for items that aren't armor
    if( !worn_item.find_armor_data() ) {
        props.push_back( string_format( "<color_c_red>%s</color>",
                                        _( "Item provides no protection" ) ) );
        return props;
    }
    if( bp == bodypart_str_id::NULL_ID() ) {
        // if the armor has no protection data
        if( worn_item.find_armor_data()->sub_data.empty() ) {
            props.push_back( string_format( "<color_c_red>%s</color>",
                                            _( "Item provides no protection" ) ) );
            return props;
        }

        // if there is only one data entry for the armor
        if( worn_item.find_armor_data()->sub_data.size() > 1 ) {
            props.push_back( string_format( "<color_c_red>%s</color>",
                                            _( "Armor is nonuniform.  Specify a limb to get armor data" ) ) );
            return props;
        } else {
            used_bp = *worn_item.get_covered_body_parts().begin();
            props.reserve( 6 );
            props.push_back( string_format( "<color_c_blue>%s</color>",
                                            _( "Each limb is uniform" ) ) );
        }
    } else {
        props.reserve( 5 );
    }

    props.push_back( string_format( "<color_c_green>[%s]</color>", _( "Properties" ) ) );

    int coverage = worn_item.get_coverage( used_bp );
    add_folded_name_and_value( props, _( "Coverage:" ), string_format( "%3d", coverage ),
                               width );
    coverage = worn_item.get_coverage( used_bp, item::cover_type::COVER_MELEE );
    add_folded_name_and_value( props, _( "Coverage (Melee):" ), string_format( "%3d",
                               coverage ), width );
    coverage = worn_item.get_coverage( used_bp, item::cover_type::COVER_RANGED );
    add_folded_name_and_value( props, _( "Coverage (Ranged):" ), string_format( "%3d",
                               coverage ), width );
    coverage = worn_item.get_coverage( used_bp, item::cover_type::COVER_VITALS );
    add_folded_name_and_value( props, _( "Coverage (Vitals):" ), string_format( "%3d",
                               coverage ), width );

    const int encumbrance = worn_item.get_encumber( c, used_bp );
    add_folded_name_and_value( props, _( "Encumbrance:" ), string_format( "%3d", encumbrance ),
                               width );
    add_folded_name_and_value( props, _( "Warmth:" ), string_format( "%3d",
                               worn_item.get_warmth( used_bp ) ), width );
    return props;
}

void add_category_and_values( std::vector<std::string> &current_text, const int field_width,
                              const std::string &category_name,
                              const std::vector<std::pair<std::string, std::string>> &names_and_values )
{
    if( names_and_values.empty() ) {
        current_text.emplace_back( trim_by_length( category_name, field_width ) );
    } else {
        const std::string separator = ", ";
        const std::string spacer = " ";
        std::string assembled_value;
        for( size_t i = 0; i < names_and_values.size() ; ++i ) {
            if( i == 0 ) {
                assembled_value += spacer;
            } else {
                assembled_value += separator;
            }
            assembled_value += names_and_values[i].first + spacer + names_and_values[i].second;
        }
        int required_oneline_width = utf8_width( category_name + spacer + assembled_value );
        if( required_oneline_width <= field_width ) {
            current_text.emplace_back( trimmed_name_and_value( category_name, assembled_value, field_width ) );
        } else {
            current_text.emplace_back( trim_by_length( category_name, field_width ) );
            for( const std::pair<std::string, std::string> &name_and_value : names_and_values ) {
                current_text.emplace_back( trimmed_name_and_value( spacer + name_and_value.first,
                                           name_and_value.second, field_width ) );
            }
        }
    }
}

std::vector<std::pair<std::string, std::string>> collect_protection_subvalues(
            const resistances &worst_res, const resistances &best_res, const resistances &median_res,
            const bool display_median, const damage_type_id &type )
{
    std::vector<std::pair<std::string, std::string>> subvalues;
    subvalues.reserve( 2 + ( display_median ? 1 : 0 ) );
    subvalues.emplace_back( _( "Worst:" ), string_format( "%.2f",
                            worst_res.type_resist( type ) ) );
    if( display_median ) {
        subvalues.emplace_back( _( "Median:" ), string_format( "%.2f",
                                median_res.type_resist( type ) ) );
    }
    subvalues.emplace_back( _( "Best:" ), string_format( "%.2f",
                            best_res.type_resist( type ) ) );
    return subvalues;
}

std::vector<std::string> clothing_protection( const item &worn_item, const int width,
        const bodypart_id &bp )
{
    std::vector<std::string> prot;
    bodypart_id used_bp = bp;

    if( !worn_item.find_armor_data() ) {
        return prot;
    }

    // if bp is null its gonna be impossible to really get good info
    if( bp == bodypart_str_id::NULL_ID() ) {
        // if we have exactly one entry for armor we can use that data
        if( worn_item.find_armor_data()->sub_data.size() == 1 ) {
            used_bp = *worn_item.get_covered_body_parts().begin();
        } else {
            return prot;
        }
    }

    // prebuild and calc some values
    // the rolls are basically a perfect hit for protection and a
    // worst possible and a median hit
    resistances worst_res = resistances( worn_item, false, 99, used_bp );
    resistances best_res = resistances( worn_item, false, 0, used_bp );
    resistances median_res = resistances( worn_item, false, 50, used_bp );

    int percent_best = 100;
    int percent_worst = 0;
    const armor_portion_data *portion = worn_item.portion_for_bodypart( used_bp );
    // if there isn't a portion this is probably pet armor
    if( portion ) {
        percent_best = portion->best_protection_chance;
        percent_worst = portion->worst_protection_chance;
    }

    bool display_median = percent_best < 50 && percent_worst < 50;

    prot.push_back( string_format( "<color_c_green>[%s]</color>", _( "Protection" ) ) );
    for( const damage_type &dt : damage_type::get_all() ) {
        bool skipped_details = false;
        const std::string dtname = uppercase_first_letter( dt.name.translated() );
        damage_info_order_id dio( dt.id.c_str() );
        if( dio->info_display == damage_info_order::info_disp::DETAILED ) {
            if( percent_worst > 0 ) {
                std::vector<std::pair<std::string, std::string>> subvalues = collect_protection_subvalues(
                            worst_res, best_res, median_res, display_median, dt.id );
                add_category_and_values( prot, width, dtname, subvalues );
            } else {
                skipped_details = true;
            }
        }
        if( skipped_details || dio->info_display == damage_info_order::info_disp::BASIC ) {
            add_folded_name_and_value( prot, dtname,
                                       string_format( "%.2f", best_res.type_resist( dt.id ) ), width );
        }
    }
    add_folded_name_and_value( prot, _( "Environmental:" ),
                               string_format( "%3d", worn_item.get_env_resist() ), width );
    add_folded_name_and_value( prot, _( "Breathability:" ),
                               string_format( "%3d", worn_item.breathability( used_bp ) ), width );
    return prot;
}

std::vector<std::string> clothing_flags_description( const item &worn_item, const int width )
{
    std::vector<std::string> description_stack;
    std::vector<std::string> current_description;

    //Handle flag_FIT and flag_VARSIZE as a special case
    if( worn_item.has_flag( flag_FIT ) ) {
        current_description = foldstring( _( "It fits you well." ), width );
        description_stack.insert( description_stack.end(), current_description.begin(),
                                  current_description.end() );
    } else if( worn_item.has_flag( flag_VARSIZE ) ) {
        current_description = foldstring( _( "It could be refitted." ), width );
        description_stack.insert( description_stack.end(), current_description.begin(),
                                  current_description.end() );
    }

    //Handle all other flags:
    const std::vector<std::pair<flag_id, std::string>> flag_descriptions = {
        { flag_HOOD, translate_marker( "It has a hood." ) },
        { flag_POCKETS, translate_marker( "It has pockets." ) },
        { flag_SUN_GLASSES, translate_marker( "It keeps the sun out of your eyes." ) },
        { flag_WATERPROOF, translate_marker( "It is waterproof." ) },
        { flag_WATER_FRIENDLY, translate_marker( "It is water friendly." ) },
        { flag_FLOTATION, translate_marker( "You will not drown today." ) },
        { flag_OVERSIZE, translate_marker( "It is very bulky." ) },
        { flag_SWIM_GOGGLES, translate_marker( "It helps you to see clearly underwater." ) },
        { flag_SEMITANGIBLE, translate_marker( "It can occupy the same space as other things." ) }
    };

    for( const std::pair<flag_id, std::string> &flag_pair : flag_descriptions ) {
        if( worn_item.has_flag( std::get<0>( flag_pair ) ) ) {
            current_description = foldstring( _( std::get<1>( flag_pair ) ), width );
            description_stack.insert( description_stack.end(), current_description.begin(),
                                      current_description.end() );
        }
    }

    return description_stack;
}

} //namespace

// Figure out encumbrance penalties this clothing is involved in
item_penalties outfit::get_item_penalties( std::list<item>::const_iterator worn_item_it,
        const Character &c, const bodypart_id &_bp )
{
    std::vector<layer_level> layer = worn_item_it->get_layer();

    std::vector<bodypart_id> body_parts_with_stacking_penalty;
    std::vector<bodypart_id> body_parts_with_out_of_order_penalty;
    std::vector<std::set<std::string>> lists_of_bad_items_within;

    for( const bodypart_id &bp : c.get_all_body_parts() ) {
        if( bp != _bp && _bp != bodypart_str_id::NULL_ID() ) {
            continue;
        }
        if( !worn_item_it->covers( bp ) ) {
            continue;
        }

        std::set<std::string> bad_items_within;

        // if no subparts do the old way
        if( bp->sub_parts.empty() ) {
            std::vector<layer_level> layer = worn_item_it->get_layer( bp );
            bool first_integrated = true;
            const int num_items = std::count_if( worn.begin(), worn.end(),
            [layer, bp, &first_integrated]( const item & i ) {
                if( i.has_layer( layer, bp ) && i.covers( bp ) && !i.has_flag( flag_SEMITANGIBLE ) ) {
                    if( i.has_flag( flag_INTEGRATED ) ) {
                        if( first_integrated ) {
                            first_integrated = false;
                            return true;
                        }
                        return false;
                    }
                    return true;
                }
                return false;
            } );
            if( num_items > 1 ) {
                body_parts_with_stacking_penalty.push_back( bp );
            }
            for( auto it = worn.begin(); it != worn_item_it; ++it ) {
                if( it->get_layer( bp ) > layer && it->covers( bp ) ) {
                    bad_items_within.insert( it->type_name() );
                }
            }
        } else {
            for( const sub_bodypart_str_id &sbp : bp->sub_parts ) {
                if( !worn_item_it->covers( sbp ) ) {
                    continue;
                }
                std::vector<layer_level> layer = worn_item_it->get_layer( sbp );
                bool first_integrated = true;
                const int num_items = std::count_if( worn.begin(), worn.end(),
                [layer, bp, sbp, &first_integrated]( const item & i ) {
                    if( i.has_layer( layer, sbp ) && i.covers( bp ) && !i.has_flag( flag_SEMITANGIBLE ) &&
                        i.covers( sbp ) ) {
                        if( i.has_flag( flag_INTEGRATED ) ) {
                            if( first_integrated ) {
                                first_integrated = false;
                                return true;
                            }
                            return false;
                        }
                        return true;
                    }
                    return false;
                } );
                if( num_items > 1 ) {
                    body_parts_with_stacking_penalty.push_back( bp );
                }
                for( auto it = worn.begin(); it != worn_item_it; ++it ) {
                    if( it->covers( sbp ) ) {
                        if( it->get_highest_layer( sbp ) > worn_item_it->get_highest_layer( sbp ) ) {
                            bad_items_within.insert( it->type_name() );
                        }
                    }
                }
            }
        }

        if( !bad_items_within.empty() ) {
            body_parts_with_out_of_order_penalty.push_back( bp );
            lists_of_bad_items_within.push_back( bad_items_within );
        }
    }

    // We intersect all the lists_of_bad_items_within so that if there is one
    // common bad item we're wearing this one over it can be mentioned in the
    // message explaining the penalty.
    while( lists_of_bad_items_within.size() > 1 ) {
        std::set<std::string> intersection_of_first_two;
        std::set_intersection(
            lists_of_bad_items_within[0].begin(), lists_of_bad_items_within[0].end(),
            lists_of_bad_items_within[1].begin(), lists_of_bad_items_within[1].end(),
            std::inserter( intersection_of_first_two, intersection_of_first_two.begin() )
        );
        lists_of_bad_items_within.erase( lists_of_bad_items_within.begin() );
        lists_of_bad_items_within[0] = std::move( intersection_of_first_two );
    }

    if( lists_of_bad_items_within.empty() ) {
        lists_of_bad_items_within.emplace_back();
    }

    return { std::move( body_parts_with_stacking_penalty ),
             std::move( body_parts_with_out_of_order_penalty ),
             std::move( lists_of_bad_items_within[0] ) };
}

std::vector<layering_item_info> outfit::items_cover_bp( const Character &c, const bodypart_id &bp )
{
    std::vector<layering_item_info> s;
    for( auto elem_it = worn.begin(); elem_it != worn.end(); ++elem_it ) {
        if( elem_it->covers( bp ) ) {
            s.push_back( { get_item_penalties( elem_it, c, bp ),
                           elem_it->get_encumber( c, bp ),
                           elem_it->tname()
                         } );
        }
    }
    return s;
}

namespace
{
/** Sort armor, from data/raw/rmlui/sort_armor.rml; outfit::sort_armor() fills it. */
class sort_armor_window : public rml_ui::window
{
    public:
        explicit sort_armor_window( const std::function<void( sort_armor_window & )> &update ) :
            rml_ui::window( _( "Sort Armor" ), "sort_armor.rml" ), update( update ) {
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
        std::function<void( sort_armor_window & )> update;
};
} // namespace

void outfit::sort_armor( Character &guy )
{
    // FIXME: get_all_body_parts() doesn't return a sorted list
    //        and bodypart_id is not compatible with std::sort()
    //        so let's use a dirty hack
    cata::flat_set<bodypart_id> armor_cat;
    for( const bodypart_id &it : guy.get_all_body_parts() ) {
        armor_cat.insert( it );
    }
    armor_cat.insert( bodypart_str_id::NULL_ID() );
    const int num_of_parts = guy.get_all_body_parts().size();

    int tabindex = 0;
    const int tabcount = num_of_parts + 1;

    int leftListIndex  = 0;
    int selected       = -1;

    std::vector<std::list<item>::iterator> tmp_worn;

    input_context ctxt( "SORT_ARMOR" );
    ctxt.register_cardinal();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "MOVE_ARMOR" );
    ctxt.register_action( "CHANGE_SIDE" );
    ctxt.register_action( "TOGGLE_CLOTH" );
    ctxt.register_action( "ASSIGN_INVLETS" );
    ctxt.register_action( "SORT_ARMOR" );
    ctxt.register_action( "EQUIP_ARMOR" );
    ctxt.register_action( "EQUIP_ARMOR_HERE" );
    ctxt.register_action( "REMOVE_ARMOR" );
    ctxt.register_action( "TOGGLE_FAVORITE" );
    ctxt.register_action( "USAGE_HELP" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "SCROLL_ITEM_INFO_UP" );
    ctxt.register_action( "SCROLL_ITEM_INFO_DOWN" );
    // Clicks on the items, the body part arrows and the close button
    ctxt.register_action( "SELECT" );

    Character &player_character = get_player_character();
    auto do_return_entry = [this, guy_ptr = &guy]() {
        uistate.open_menu = [this, guy_ptr]() {
            sort_armor( *guy_ptr );
        };
    };

    int leftListSize = 0;
    // What was last shown, to keep the cursor in view and new details at their top
    std::pair<int, int> shown_cursor( -1, -1 );

    std::unique_ptr<sort_armor_window> window = std::make_unique<sort_armor_window>(
    [&]( sort_armor_window & win ) {
        const int win_w = FULL_SCREEN_WIDTH + ( TERMX - FULL_SCREEN_WIDTH ) * 3 / 4;
        const int middle_w = ( win_w - 4 ) - 2 * ( ( win_w - 4 ) / 3 );
        win.set_size( rml_ui::term_width( win_w ), "96%" );

        // Create ptr list of items to display
        tmp_worn.clear();
        const bodypart_id &bp = armor_cat[ tabindex ];
        if( bp == bodypart_str_id::NULL_ID() ) {
            // All
            for( auto it = worn.begin(); it != worn.end(); ++it ) {
                tmp_worn.push_back( it );
            }
        } else {
            // bp_*
            for( auto it = worn.begin(); it != worn.end(); ++it ) {
                if( it->covers( bp ) ) {
                    tmp_worn.push_back( it );
                }
            }
        }

        leftListSize = tmp_worn.size();
        // Ensure leftListIndex is in bounds
        int new_index_upper_bound = std::max( 0, leftListSize - 1 );
        leftListIndex = std::min( leftListIndex, new_index_upper_bound );

        // top bar: the body part, with buttons either side, then the keys
        const std::string part_name = bp == bodypart_str_id::NULL_ID() ?  _( "All" ) :
                                      body_part_name_as_heading( bp, 1 );
        win.set_header( "<div class=\"hbox\"><div class=\"fixed\">" + rml_ui::button_rml( "LEFT",
                        "&lt;&lt;" ) + rml_ui::colored_rml( part_name, c_yellow ) + rml_ui::button_rml( "RIGHT",
                                "&gt;&gt;" ) + "</div><div class=\"grow right\">" + rml_ui::markup_to_rml( string_format(
                                            _( "[<color_yellow>%s</color>] Hide sprite.  "
                                                    "[<color_yellow>%s</color>] Change side.  "
                                                    "Press [<color_yellow>%s</color>] for help.  "
                                                    "Press [<color_yellow>%s</color>] to change keybindings." ),
                                            ctxt.get_desc( "TOGGLE_CLOTH" ),
                                            ctxt.get_desc( "CHANGE_SIDE" ),
                                            ctxt.get_desc( "USAGE_HELP" ),
                                            ctxt.get_desc( "HELP_KEYBINDINGS" ) ), c_white ) + "</div></div>" );

        const auto pane_head = []( const std::string & right ) {
            return "<div class=\"hbox sa-head\"><div class=\"grow\">" + rml_ui::colored_rml(
                       _( "(Innermost)" ), c_light_gray ) + "</div><div class=\"fixed\">" +
                   rml_ui::colored_rml( right, c_light_gray ) + "</div></div>";
        };
        const std::string pane_foot = "<div class=\"sa-foot\">" + rml_ui::colored_rml( _( "(Outermost)" ),
                                      c_light_gray ) + "</div>";

        // Left list: the items, with their storage
        std::string left;
        for( int itemindex = 0; itemindex < leftListSize; itemindex++ ) {
            std::string worn_armor_name = tmp_worn[itemindex]->tname();
            // Get storage capacity in user's preferred units
            units::volume worn_armor_capacity = tmp_worn[itemindex]->get_volume_capacity();
            double worn_armor_storage = convert_volume( units::to_milliliter( worn_armor_capacity ) );

            item_penalties const penalties =
                get_item_penalties( tmp_worn[itemindex], guy, bp );

            std::string mark;
            if( tmp_worn[itemindex]->has_flag( json_flag_HIDDEN ) ) {
                //~ Hint: Letter to show which piece of armor is Hidden in the layering menu
                mark = rml_ui::colored_rml( _( "H" ), c_cyan );
            }
            std::string row_class;
            if( itemindex == leftListIndex ) {
                row_class += " selected";
            }
            if( itemindex == selected ) {
                // picked up, to be moved
                row_class += " sa-moving";
            }
            left += string_format( "<div id=\"sa-row-%d\" class=\"item line sa-row%s\" data-row=\"%d\">",
                                   itemindex, row_class, itemindex ) +
                    "<span class=\"sa-mark\">" + mark + "</span><span class=\"sa-item\">" +
                    rml_ui::colored_rml( worn_armor_name, penalties.color_for_stacking_badness() ) +
                    "</span><span class=\"sa-value\">" + ( worn_armor_storage > 0 ?
                            // two digits, accurate to 1% of preferred storage unit
                            rml_ui::colored_rml( string_format( "%.2f", worn_armor_storage ), c_light_gray ) :
                            std::string() ) + "</span></div>";
        }
        if( leftListSize == 0 ) {
            left = rml_ui::colored_rml( _( "<empty>" ), c_light_blue );
        }

        // Middle: the item's details, over the encumbrance and warmth
        std::string middle;
        if( leftListSize > 0 ) {
            middle = mid_pane_rml( tmp_worn[leftListIndex], guy, bp, middle_w - 1 );
        } else {
            middle = rml_ui::colored_rml( _( "Nothing to see here!" ), c_white );
        }
        std::string encumbrance = "<div>" + rml_ui::colored_rml( _( "Encumbrance and Warmth" ),
                                  c_white ) + "</div>";
        for( const std::pair<std::string, std::string> &line : guy.encumbrance_lines(
                 ( leftListSize > 0 ) ? &*tmp_worn[leftListIndex] : nullptr ) ) {
            encumbrance += "<div class=\"hbox\"><div class=\"grow\">" + rml_ui::markup_to_rml( line.first ) +
                           "</div><div class=\"fixed sa-numbers\">" + rml_ui::markup_to_rml( line.second ) +
                           "</div></div>";
        }

        // Right list: the items over each body part
        const auto &combine_bp = [&guy]( const bodypart_id & cover ) -> bool {
            const bodypart_id opposite = cover.obj().opposite_part;
            return cover != opposite &&
            guy.worn.items_cover_bp( guy, cover ) == guy.worn.items_cover_bp( guy, opposite );
        };

        cata::flat_set<bodypart_id> rl;
        for( const bodypart_id &cover : armor_cat ) {
            if( cover == bodypart_str_id::NULL_ID() ) {
                continue;
            }
            if( !combine_bp( cover ) || rl.count( cover.obj().opposite_part ) == 0 ) {
                rl.insert( cover );
            }
        }
        std::string right;
        for( const bodypart_id &cover : rl ) {
            if( cover == bodypart_str_id::NULL_ID() ) {
                continue;
            }
            right += "<div class=\"line\">" + rml_ui::colored_rml( string_format( "%s:",
                     body_part_name_as_heading( cover, combine_bp( cover ) ? 2 : 1 ) ),
                     cover == bp ? c_yellow : c_white ) + "</div>";
            for( layering_item_info &elem : items_cover_bp( guy, cover ) ) {
                const char plus = elem.penalties.badness() > 0 ? '+' : ' ';
                right += "<div class=\"line sa-row\"><span class=\"sa-mark\"></span><span class=\"sa-item\">" +
                         rml_ui::colored_rml( elem.name, elem.penalties.color_for_stacking_badness() ) +
                         "</span><span class=\"sa-value\">" + rml_ui::colored_rml( string_format( "%d%c",
                                 elem.encumber, plus ), c_light_gray ) + "</span></div>";
            }
        }

        win.set_body( "<div class=\"panes\"><div class=\"pane sa-pane\">" +
                      pane_head( string_format( _( "Storage (%s)" ), volume_units_abbr() ) ) +
                      "<div id=\"sa-list\" class=\"sa-scroll\">" + left + "</div>" + pane_foot +
                      "</div><div class=\"pane sa-pane\"><div id=\"sa-info\" class=\"sa-scroll\">" + middle +
                      "</div><div class=\"sa-encumbrance\">" + encumbrance +
                      "</div></div><div class=\"pane sa-pane\">" + pane_head( _( "Encumbrance" ) ) +
                      "<div id=\"sa-cover\" class=\"sa-scroll\">" + right + "</div>" + pane_foot + "</div></div>" );

        if( shown_cursor != std::make_pair( tabindex, leftListIndex ) ) {
            shown_cursor = std::make_pair( tabindex, leftListIndex );
            win.reveal( "sa-row-" + std::to_string( leftListIndex ) );
            win.scroll_pane( "sa-info", rml_ui::scroll::top );
        }
    } );

    bool exit = false;
    while( !exit ) {
        if( guy.is_avatar() ) {
            // Totally hoisted this from advanced_inv
            if( player_character.get_moves() < 0 ) {
                do_return_entry();
                return;
            }
        } else {
            // Player is sorting NPC's armor here
            if( rl_dist( player_character.pos_bub(), guy.pos_bub() ) > 1 ) {
                guy.add_msg_if_npc( m_bad, _( "%s is too far to sort armor." ), guy.get_name() );
                return;
            }
            if( guy.attitude_to( player_character ) != Creature::Attitude::FRIENDLY ) {
                guy.add_msg_if_npc( m_bad, _( "%s is not friendly!" ), guy.get_name() );
                return;
            }
        }

        ui_manager::redraw();
        std::string action = ctxt.handle_input();
        if( action == "SELECT" ) {
            const std::string clicked = window->resolve_click( action );
            const int row = window->hovered_index( "data-row" );
            if( clicked != action ) {
                // the body part arrows, and the close button
                action = clicked;
            } else if( row >= 0 && row < leftListSize && selected < 0 ) {
                leftListIndex = row;
            }
        }
        if( guy.is_npc() && action == "ASSIGN_INVLETS" ) {
            // It doesn't make sense to assign invlets to NPC items
            continue;
        }

        // Helper function for moving items in the list
        auto shift_selected_item = [&]() {
            if( selected >= 0 ) {
                std::list<item>::iterator to = tmp_worn[leftListIndex];
                if( leftListIndex > selected ) {
                    ++to;
                }
                worn.splice( to, worn, tmp_worn[selected] );
                selected = leftListIndex;
                guy.calc_encumbrance();
            }
        };

        if( action == "UP" && leftListSize > 0 ) {
            if( leftListIndex > 0 ) {
                item &item_to_check = *tmp_worn[leftListIndex - 1];
                if( selected < 0 || !item_to_check.has_flag( flag_INTEGRATED ) ) {
                    leftListIndex--;
                    shift_selected_item();
                } else {
                    popup( _( "Can't sort this under your integrated armor!" ) );
                }
            } else {
                leftListIndex = leftListSize - 1;
                shift_selected_item();
            }
        } else if( action == "DOWN" && leftListSize > 0 ) {
            if( leftListIndex + 1 < leftListSize ) {
                leftListIndex++;
                shift_selected_item();
            } else {
                item &item_to_check = *tmp_worn[0];
                if( selected < 0 || !item_to_check.has_flag( flag_INTEGRATED ) ) {
                    leftListIndex = 0;
                    shift_selected_item();
                } else {
                    popup( _( "Can't sort this under your integrated armor!" ) );
                }
            }
        } else if( action == "LEFT" || action == "PREV_TAB" || action == "RIGHT" || action == "NEXT_TAB" ) {
            tabindex = inc_clamp_wrap( tabindex, action == "RIGHT" || action == "NEXT_TAB", tabcount );
            leftListIndex = 0;
            selected = -1;
        } else if( action == "PAGE_DOWN" ) {
            window->scroll_pane( "sa-cover", rml_ui::scroll::line_down );
        } else if( action == "PAGE_UP" ) {
            window->scroll_pane( "sa-cover", rml_ui::scroll::line_up );
        } else if( action == "MOVE_ARMOR" && leftListIndex < leftListSize ) {
            if( selected >= 0 ) {
                selected = -1;
            } else {
                item &item_to_check = *tmp_worn[leftListIndex];
                if( !item_to_check.has_flag( flag_INTEGRATED ) ) {
                    selected = leftListIndex;
                } else {
                    popup( _( "Can't move your integrated armor!" ) );
                }
            }
        } else if( action == "CHANGE_SIDE" ) {
            if( leftListIndex < leftListSize && tmp_worn[leftListIndex]->is_sided() ) {
                if( player_character.query_yn( _( "Swap side for %s?" ),
                                               colorize( tmp_worn[leftListIndex]->tname(),
                                                       tmp_worn[leftListIndex]->color_in_inventory() ) ) ) {
                    guy.change_side( *tmp_worn[leftListIndex] );
                }
            }
        } else if( action == "TOGGLE_CLOTH" ) {
            if( leftListIndex < leftListSize ) {
                if( !tmp_worn[leftListIndex]->has_flag( json_flag_HIDDEN ) ) {
                    tmp_worn[leftListIndex]->set_flag( json_flag_HIDDEN );
                } else {
                    tmp_worn[leftListIndex]->unset_flag( json_flag_HIDDEN );
                }
            }
        } else if( action == "SORT_ARMOR" ) {
            worn.sort(
            []( const item & l, const item & r ) {
                if( l.has_flag( flag_INTEGRATED ) == r.has_flag( flag_INTEGRATED ) ) {
                    return l.get_layer() < r.get_layer();
                } else {
                    return l.has_flag( flag_INTEGRATED );
                }
            }
            );
            guy.calc_encumbrance();
        } else if( action == "EQUIP_ARMOR" ) {
            // filter inventory for all items that are armor/clothing
            item_location loc = game_menus::inv::wear( guy );
            // only equip if something valid selected!
            if( loc ) {
                // store the item name just in case obtain() fails
                const std::string item_name = loc->display_name();
                item_location obtained = loc.obtain( guy );
                if( obtained ) {
                    // wear the item
                    std::optional<std::list<item>::iterator> new_equip_it =
                        guy.wear( obtained );
                    if( new_equip_it ) {
                        const bodypart_id &bp = armor_cat[tabindex];
                        if( tabindex == num_of_parts || ( **new_equip_it ).covers( bp ) ) {
                            // Set ourselves up to be pointing at the new item
                            // TODO: This doesn't work yet because we don't save our
                            // state through other activities, but that's a thing
                            // that would be nice to do.
                            leftListIndex =
                                std::count_if( worn.begin(), *new_equip_it,
                            [&]( const item & i ) {
                                return tabindex == num_of_parts || i.covers( bp );
                            } );
                        }
                    } else if( guy.is_npc() ) {
                        // TODO: Pass the reason here
                        popup( _( "Can't put this on!" ) );
                    }
                } else {
                    guy.add_msg_if_player( _( "You chose not to wear the %s." ), item_name );
                }
            }
        } else if( action == "EQUIP_ARMOR_HERE" ) {
            // filter inventory for all items that are armor/clothing
            item_location loc = game_menus::inv::wear( guy, armor_cat[tabindex] );
            // only equip if something valid selected!
            if( loc ) {
                // store the item name just in case obtain() fails
                const std::string item_name = loc->display_name();
                item_location obtained = loc.obtain( guy );
                if( obtained ) {
                    // wear the item
                    std::optional<std::list<item>::iterator> new_equip_it =
                        guy.wear( obtained );
                    if( new_equip_it && !tmp_worn.empty() ) {
                        // save iterator to cursor's position
                        std::list<item>::iterator cursor_it = tmp_worn[leftListIndex];
                        item &item_to_check = *cursor_it;
                        if( item_to_check.has_flag( flag_INTEGRATED ) ) {
                            // prevent adding under integrated armor
                            popup( _( "Can't put this on under your integrated armor!" ) );
                        } else {
                            // reorder `worn` vector to place new item at cursor
                            worn.splice( cursor_it, worn, *new_equip_it );
                        }
                    } else if( guy.is_npc() && !tmp_worn.empty() ) {
                        // TODO: Pass the reason here
                        popup( _( "Can't put this on!" ) );
                    }
                } else {
                    guy.add_msg_if_player( _( "You chose not to wear the %s." ), item_name );
                }
            }
        } else if( action == "REMOVE_ARMOR" ) {
            // query (for now)
            if( leftListIndex < leftListSize ) {
                if( player_character.query_yn( _( "Remove selected armor?" ) ) ) {
                    do_return_entry();

                    //create an item_location for player::takeoff to handle.
                    item &item_for_takeoff = *tmp_worn[leftListIndex];
                    item_location loc_for_takeoff = item_location( guy, &item_for_takeoff );

                    // remove the item, asking to drop it if necessary
                    guy.takeoff( loc_for_takeoff );
                    return;
                }
            }
        } else if( action == "TOGGLE_FAVORITE" ) {
            if( leftListIndex < leftListSize ) {
                item &item_to_toggle = *tmp_worn[leftListIndex];
                item_to_toggle.set_favorite( !item_to_toggle.is_favorite );
            }
        } else if( action == "ASSIGN_INVLETS" ) {
            // prompt first before doing this (yes, yes, more popups...)
            if( query_yn( _( "Reassign inventory letters for armor?" ) ) ) {
                // Start with last armor (the most unimportant one?)
                auto iiter = inv_chars.rbegin();
                auto witer = worn.rbegin();
                while( witer != worn.rend() && iiter != inv_chars.rend() ) {
                    const char invlet = *iiter;
                    item &w = *witer;
                    if( invlet == w.invlet ) {
                        ++witer;
                    } else if( guy.invlet_to_item( invlet ) != nullptr ) {
                        ++iiter;
                    } else {
                        guy.inv->reassign_item( w, invlet );
                        ++witer;
                        ++iiter;
                    }
                }
            }
        } else if( action == "SCROLL_ITEM_INFO_UP" ) {
            window->scroll_pane( "sa-info", rml_ui::scroll::line_up );
        } else if( action == "SCROLL_ITEM_INFO_DOWN" ) {
            window->scroll_pane( "sa-info", rml_ui::scroll::line_down );
        } else if( action == "USAGE_HELP" ) {
            const std::vector<std::string> help_strings = {
                string_format( _( "[<color_yellow>%s</color>]/[<color_yellow>%s</color>] to scroll the left pane (list of items).\n" ),
                               ctxt.get_desc( "UP" ), ctxt.get_desc( "DOWN" ) ),
                string_format( _( "[<color_yellow>%s</color>]/[<color_yellow>%s</color>] to scroll the middle pane (item information).\n" ),
                               ctxt.get_desc( "SCROLL_ITEM_INFO_UP" ), ctxt.get_desc( "SCROLL_ITEM_INFO_DOWN" ) ),
                string_format( _( "[<color_yellow>%s</color>]/[<color_yellow>%s</color>] to scroll the right pane (items grouped by body part).\n" ),
                               ctxt.get_desc( "PREV_TAB" ), ctxt.get_desc( "NEXT_TAB" ) ),
                string_format( _( "[<color_yellow>%s</color>]/[<color_yellow>%s</color>] to limit the left pane to a particular body part.\n" ),
                               ctxt.get_desc( "LEFT" ), ctxt.get_desc( "RIGHT" ) ),
                string_format( _( "[<color_yellow>%s</color>] to select an item for reordering.\n" ),
                               ctxt.get_desc( "MOVE_ARMOR" ) ),
                string_format( _( "[<color_yellow>%s</color>] to assign special inventory letters to clothing.\n" ),
                               ctxt.get_desc( "ASSIGN_INVLETS" ) ),
                string_format( _( "[<color_yellow>%s</color>] to change the side on which item is worn.\n" ),
                               ctxt.get_desc( "CHANGE_SIDE" ) ),
                string_format( _( "[<color_yellow>%s</color>] to toggle item visibility on character sprite.\n" ),
                               ctxt.get_desc( "TOGGLE_CLOTH" ) ),
                string_format( _( "[<color_yellow>%s</color>] to sort worn items into natural layer order.\n" ),
                               ctxt.get_desc( "SORT_ARMOR" ) ),
                string_format( _( "[<color_yellow>%s</color>] to equip a new item.\n" ),
                               ctxt.get_desc( "EQUIP_ARMOR" ) ),
                string_format( _( "[<color_yellow>%s</color>] to equip a new item at the currently selected position.\n" ),
                               ctxt.get_desc( "EQUIP_ARMOR_HERE" ) ),
                string_format( _( "[<color_yellow>%s</color>] to remove selected item.\n" ),
                               ctxt.get_desc( "REMOVE_ARMOR" ) ),
                string_format( _( "[<color_yellow>%s</color>] to toggle item as favorite.\n" ),
                               ctxt.get_desc( "TOGGLE_FAVORITE" ) ),
                "\n",
                _( "Encumbrance explanation:\n" ),
                _( "<color_light_gray>The first number is the summed encumbrance from all clothing on that bodypart."
                   "The second number is an additional encumbrance penalty caused by wearing either multiple items "
                   "on one of the bodypart's layers or wearing items the wrong way (e.g. a shirt over a backpack)."
                   "The sum of these values is the effective encumbrance value your character has for that bodypart."
                   "</color>" )
            };
            std::string assembled_string;
            for( const std::string &current_line : help_strings ) {
                assembled_string += current_line;
            }

            const auto new_win = []() {
                return catacurses::newwin( FULL_SCREEN_HEIGHT, FULL_SCREEN_WIDTH,
                                           point( std::max( 0, ( TERMX - FULL_SCREEN_WIDTH ) / 2 ),
                                                  std::max( 0, ( TERMY - FULL_SCREEN_HEIGHT ) / 2 ) ) );
            };
            scrollable_text( new_win, _( "Sort armor help" ), assembled_string );
        } else if( action == "QUIT" ) {
            exit = true;
        }
    }
    uistate.open_menu.reset();
}

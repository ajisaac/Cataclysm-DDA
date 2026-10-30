#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "addiction.h"
#include "bionics.h"
#include "bodygraph.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character.h"
#include "character_modifier.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "cursesdef.h"
#include "debug.h"
#include "display.h"
#include "effect.h"
#include "enum_conversions.h"
#include "flag.h"
#include "game.h"
#include "game_constants.h"
#include "game_inventory.h"
#include "input_context.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "magic_enchantment.h"
#include "mutation.h"
#include "output.h"
#include "pimpl.h"
#include "point.h"
#include "proficiency.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "skill.h"
#include "skill_ui.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"
#include "weather.h"

static const bionic_id bio_cqb( "bio_cqb" );

static const skill_id skill_bashing( "bashing" );
static const skill_id skill_cutting( "cutting" );
static const skill_id skill_dodge( "dodge" );
static const skill_id skill_melee( "melee" );
static const skill_id skill_stabbing( "stabbing" );
static const skill_id skill_unarmed( "unarmed" );

static const trait_id trait_TROGLO( "TROGLO" );
static const trait_id trait_TROGLO2( "TROGLO2" );
static const trait_id trait_TROGLO3( "TROGLO3" );

static const std::string title_STATS = translate_marker( "STATS" );
static const std::string title_ENCUMB = translate_marker( "ENCUMBRANCE AND WARMTH" );
static const std::string title_EFFECTS = translate_marker( "EFFECTS" );
static const std::string title_SPEED = translate_marker( "SPEED" );
static const std::string title_MOVE_COST = translate_marker( "MOVE COST" );
static const std::string title_SKILLS = translate_marker( "SKILLS" );
static const std::string title_BIONICS = translate_marker( "BIONICS" );
static const std::string title_TRAITS = translate_marker( "TRAITS" );
static const std::string title_PROFICIENCIES = translate_marker( "PROFICIENCIES" );

// Rescale temperature value to one that the player sees
static int temperature_print_rescaling( units::temperature temp )
{
    return ( units::to_legacy_bodypart_temp( temp ) / 100.0 ) * 2 - 100;
}

static bool should_combine_bps( const Character &p, const bodypart_id &l, const bodypart_id &r,
                                const item *selected_clothing )
{
    return l != r && // are different parts
           l ==  r->opposite_part && r == l->opposite_part && // are complementary parts
           // same encumbrance & temperature
           p.compare_encumbrance_data( l, r ) &&
           temperature_print_rescaling( p.get_part_temp_conv( l ) ) == temperature_print_rescaling(
               p.get_part_temp_conv( r ) ) &&
           // selected_clothing covers both or neither parts
           ( !selected_clothing || ( selected_clothing->covers( l ) == selected_clothing->covers( r ) ) ) &&
           // they have the same HP
           p.get_part_hp_cur( l ) == p.get_part_hp_cur( r );

}

static std::vector<std::pair<bodypart_id, bool>> list_and_combine_bps( const Character &p,
        const item *selected_clothing )
{
    // bool represents whether the part has been combined with its other half
    std::vector<std::pair<bodypart_id, bool>> bps;
    for( const bodypart_id &bp : p.get_all_body_parts( get_body_part_flags::sorted ) ) {
        // assuming that a body part has at most one other half
        if( should_combine_bps( p, bp, bp->opposite_part.id(), selected_clothing ) ) {
            if( std::find( bps.begin(), bps.end(), std::pair<bodypart_id, bool>( bp->opposite_part.id(),
                           true ) ) == bps.end() ) {
                // only add one
                bps.emplace_back( bp, true );
            }
        } else {
            bps.emplace_back( bp, false );
        }
    }
    return bps;
}

std::vector<std::pair<std::string, std::string>> Character::encumbrance_lines(
            const item *selected_clothing ) const
{
    /*** I chose to instead only display X+Y instead of X+Y=Z. More room was needed ***
     *** for displaying triple digit encumbrance, due to new encumbrance system.    ***
     *** If the player wants to see the total without having to do them maths, the  ***
     *** armor layers ui shows everything they want :-) -Davek                      ***/
    std::vector<std::pair<std::string, std::string>> lines;
    for( const std::pair<bodypart_id, bool> &part : list_and_combine_bps( *this, selected_clothing ) ) {
        const bodypart_id &bp = part.first;
        const int encumbrance = get_part_encumbrance( bp );
        const int layer_penalty = get_part_layer_penalty( bp );
        const nc_color encumbrance_color = display::encumb_color( encumbrance );
        // Make the text green if this part is covered by the passed in item.
        const bool highlighted = selected_clothing ? selected_clothing->covers( bp ) : false;
        lines.emplace_back(
            colorize( body_part_name_as_heading( bp, part.second ? 2 : 1 ),
                      highlighted ? c_green : c_light_gray ),
            // accumulated encumbrance from clothing, plus extra encumbrance from layering,
            // then warmth
            colorize( string_format( "%3d", encumbrance - layer_penalty ), encumbrance_color ) +
            colorize( "+", c_light_gray ) +
            colorize( string_format( "%-3d", layer_penalty ), encumbrance_color ) +
            colorize( string_format( " (% 3d)", temperature_print_rescaling( get_part_temp_conv( bp ) ) ),
                      display::bodytemp_color( *this, bp ) ) );
    }
    return lines;
}

static nc_color limb_score_current_color( float cur_score, float bp_score )
{
    if( cur_score < bp_score * 0.25 ) {
        return c_brown;
    }
    if( cur_score < bp_score * 0.5 ) {
        return c_light_red;
    }
    if( cur_score < bp_score * 0.8 ) {
        return c_yellow;
    }
    return c_white;
}

static std::string get_score_text( const std::string &sc_name, float cur_score, float bp_score )
{
    if( std::abs( bp_score ) <= std::numeric_limits<float>::epsilon() ) {
        return std::string();
    }

    nc_color score_c = limb_score_current_color( cur_score, bp_score );
    std::string sc_txt = colorize( string_format( "%.2f (%.f%%)", cur_score,
                                   cur_score * 100.f / bp_score ), score_c );
    //~ 1$ = name of the limb score (ex: Balance), 2$ = current score value (colored)
    return string_format( _( "%1$s score: %2$s" ), sc_name, sc_txt );
}

static std::vector<std::string> get_encumbrance_description( const Character &you,
        const bodypart_id &bp )
{
    std::vector<std::string> s;
    const bodypart *part = you.get_part( bp );
    if( !bp->encumb_text.empty() ) {
        s.emplace_back( colorize( string_format( _( "Encumbrance effects: %s" ), bp->encumb_text ),
                                  c_magenta ) );
    }
    for( const limb_score &sc : limb_score::get_all() ) {
        if( !bp->has_limb_score( sc.getId() ) ) {
            continue;
        }
        float cur_score = part->get_limb_score( you, sc.getId() );
        float bp_score = bp->get_limb_score( sc.getId() );
        // Check for any global limb score modifiers
        for( const effect &eff : you.get_effects_with_flag( flag_EFFECT_LIMB_SCORE_MOD ) ) {
            cur_score *= eff.get_limb_score_mod( sc.getId(), you.resists_effect( eff ) );
        }
        // Check for local score-modifying effects only if we have any
        if( you.has_flag( flag_EFFECT_LIMB_SCORE_MOD_LOCAL ) ) {
            for( const effect &local : you.get_effects_from_bp( part->get_id() ) ) {
                float local_mul = 1.0f;
                if( local.has_flag( flag_EFFECT_LIMB_SCORE_MOD_LOCAL ) ) {
                    local_mul = local.get_limb_score_mod( sc.getId(), you.resists_effect( local ) );
                    cur_score *= local_mul;
                }
            }
        }
        s.emplace_back( get_score_text( sc.name().translated(), cur_score, bp_score ) );
    }
    for( const character_modifier &mod : character_modifier::get_all() ) {
        for( const auto &sc : mod.use_limb_scores() ) {
            if( sc.second == 0.0f || sc.first.is_null() || !bp->has_limb_score( sc.first ) ) {
                continue;
            }
            std::string desc = mod.description().translated();
            std::string valstr = colorize( string_format( "%.2f", mod.modifier( you ) ),
                                           limb_score_current_color( part->get_limb_score( you, sc.first ) * sc.second,
                                                   bp->get_limb_score( sc.first ) * sc.second ) );
            s.emplace_back( string_format( "%s: %s%s", desc, mod.mod_type_str(), valstr ) );
        }
    }
    return s;
}

static bool is_cqb_skill( const skill_id &id )
{
    // TODO: this skill list here is used in other places as well. Useless redundancy and
    // dependency. Maybe change it into a flag of the skill that indicates it's a skill used
    // by the bionic?
    static const std::array<skill_id, 5> cqb_skills = { {
            skill_melee, skill_unarmed, skill_cutting,
            skill_bashing, skill_stabbing,
        }
    };
    return std::find( cqb_skills.begin(), cqb_skills.end(), id ) != cqb_skills.end();
}

namespace
{
enum class player_display_tab : int {
    stats,
    encumbrance,
    speed,
    skills,
    traits,
    bionics,
    effects,
    proficiencies,
    num_tabs,
};
} // namespace

namespace
{
struct bionic_grouping {
    const translation name;
    const translation description;
    const int installed_count;
};

struct speedlist_entry {
    bool is_speed;
    std::string description;
    int val;
    bool percent;
};

/** A line of a section: markup on the left, and on the right in a fixed-width font. */
struct sheet_row {
    std::string left;
    std::string right;
    bool header = false;
};

/**
 * The character sheet, from data/raw/rmlui/character_sheet.rml. The caller
 * fills it on every redraw.
 */
class character_sheet : public rml_ui::window
{
    public:
        explicit character_sheet( const std::function<void( character_sheet & )> &update ) :
            rml_ui::window( std::string(), "character_sheet.rml" ), update( update ) {
            set_body_scrolls( false );
        }

        void scroll_info( rml_ui::scroll how ) {
            scroll( "cs-info", how );
        }
        void reveal( const std::string &id ) {
            scroll_into_view( id );
        }

    protected:
        void on_redraw() override {
            update( *this );
        }

    private:
        std::function<void( character_sheet & )> update;
};
} // namespace

static std::string key_markup( const input_context &ctxt, const std::string &action )
{
    return string_format( "[<color_yellow>%s</color>]", ctxt.get_desc( action ) );
}

/**
 * A section's title: its name, a button for `view_action` if it has one, and
 * the key that selects it.
 */
static std::string section_title_rml( const input_context &ctxt, const std::string &title,
                                      const std::string &select_action, const std::string &view_action = std::string() )
{
    std::string rml = "<span class=\"cs-name\">";
    if( view_action.empty() ) {
        rml += rml_ui::colored_rml( title, c_light_gray );
    } else {
        rml += rml_ui::button_rml( view_action, rml_ui::markup_to_rml( key_markup( ctxt,
                                   view_action ) + " " + title, c_light_gray ) );
    }
    rml += "</span>";
    if( !select_action.empty() ) {
        rml += rml_ui::button_rml( select_action, rml_ui::markup_to_rml( key_markup( ctxt,
                                   select_action ), c_light_gray ) );
    }
    return rml;
}

/**
 * One section of the sheet, its lines carrying `data-sec` and `data-row` so
 * the mouse can pick them, and an id so the selected one can be scrolled to.
 */
static std::string section_rml( const player_display_tab tab, const player_display_tab curtab,
                                const unsigned line, const std::string &title_rml, const std::string &above_rml,
                                const std::vector<sheet_row> &rows )
{
    const int index = static_cast<int>( tab );
    const bool focused = tab == curtab;
    std::string rml = string_format( "<div class=\"cs-section%s\" data-sec=\"%d\">",
                                     focused ? " focused" : "", index );
    rml += "<div class=\"cs-title\">" + title_rml + "</div>" + above_rml;
    rml += "<div class=\"cs-list\">";
    for( size_t i = 0; i < rows.size(); ++i ) {
        const sheet_row &row = rows[i];
        if( row.header ) {
            rml += "<div class=\"line cs-head\">" + rml_ui::markup_to_rml( row.left ) + "</div>";
            continue;
        }
        rml += string_format( "<div id=\"cs-row-%d-%d\" class=\"item line cs-row%s\" data-row=\"%d\">",
                              index, static_cast<int>( i ), focused && i == line ? " selected" : "",
                              static_cast<int>( i ) ) +
               "<span class=\"cs-left\">" + rml_ui::markup_to_rml( row.left ) +
               "</span><span class=\"cs-right\">" + rml_ui::markup_to_rml( row.right ) + "</span></div>";
    }
    return rml + "</div></div>";
}

static std::vector<sheet_row> stats_rows( const Character &you )
{
    const auto stat_row = []( const std::string & name, const int cur, const int max ) {
        nc_color cstatus;
        if( cur <= 0 ) {
            cstatus = c_dark_gray;
        } else if( cur < max / 2 ) {
            cstatus = c_red;
        } else if( cur < max ) {
            cstatus = c_light_red;
        } else if( cur == max ) {
            cstatus = c_white;
        } else if( cur < max * 1.5 ) {
            cstatus = c_light_green;
        } else {
            cstatus = c_green;
        }
        return sheet_row{ colorize( name, c_light_gray ), colorize( string_format( "%2d", cur ), cstatus ) +
                          colorize( string_format( " (%2d)", max ), c_light_gray ) };
    };

    std::vector<sheet_row> rows;
    rows.push_back( stat_row( _( "Strength:" ), you.get_str(), you.get_str_base() ) );
    rows.push_back( stat_row( _( "Dexterity:" ), you.get_dex(), you.get_dex_base() ) );
    rows.push_back( stat_row( _( "Intelligence:" ), you.get_int(), you.get_int_base() ) );
    rows.push_back( stat_row( _( "Perception:" ), you.get_per(), you.get_per_base() ) );
    rows.push_back( { colorize( _( "Weight:" ), c_light_gray ), display::weight_string( you ) } );
    rows.push_back( { colorize( _( "Lifestyle:" ), c_light_gray ), display::health_string( you ) } );
    rows.push_back( { colorize( _( "Height:" ), c_light_gray ), colorize( you.height_string(), c_light_gray ) } );
    rows.push_back( { colorize( _( "Age:" ), c_light_gray ), colorize( you.age_string(), c_light_gray ) } );
    rows.push_back( { colorize( _( "Blood type:" ), c_light_gray ),
                      colorize( io::enum_to_string( you.my_blood_type ) + ( you.blood_rh_factor ? "+" : "-" ),
                                c_light_gray ) } );
    return rows;
}

static std::string stats_info( const Character &you, const unsigned line )
{
    // A description, then the numbers it bears on
    const auto info = []( const std::string & description, const std::vector<std::string> &values ) {
        std::string text = colorize( description, c_magenta );
        if( !values.empty() ) {
            text += "\n\n" + string_join( values, "\n" );
        }
        return text;
    };

    if( line == 0 ) {
        return info( _( "Strength affects your melee damage, the amount of weight you can carry, your total HP, "
                        "your resistance to many diseases, and the effectiveness of actions which require brute force." ), {
            string_format( _( "Base HP: <color_white>%d</color>" ),
                           you.get_part_hp_max( you.get_root_body_part() ) ),
            string_format( _( "Carry weight (%s): <color_white>%.1f</color>" ), weight_units(),
                           convert_weight( you.weight_capacity() ) ),
            string_format( _( "Bash damage: <color_white>%.1f</color>" ), you.bonus_damage( false ) )
        } );
    } else if( line == 1 ) {
        return info( _( "Dexterity affects your chance to hit in melee combat, helps you steady your "
                        "gun for ranged combat, and enhances many actions that require finesse." ), {
            string_format( _( "Melee to-hit bonus: <color_white>%+.1lf</color>" ), you.get_melee_hit_base() ),
            string_format( _( "Ranged penalty: <color_white>%+d</color>" ),
                           -std::abs( you.ranged_dex_mod() ) ),
            string_format( _( "Throwing penalty per target's dodge: <color_white>%+d</color>" ),
                           you.throw_dispersion_per_dodge( false ) )
        } );
    } else if( line == 2 ) {
        return info( _( "Intelligence is less important in most situations, but it is vital for more complex tasks like "
                        "electronics crafting.  It also affects how much skill you can pick up from reading a book." ), {
            string_format( _( "Read times: <color_white>%d%%</color>" ), you.read_speed() ),
            string_format( _( "Crafting bonus: <color_white>%d%%</color>" ), you.get_int() )
        } );
    } else if( line == 3 ) {
        std::vector<std::string> values = {
            string_format( _( "Trap detection level: <color_white>%d</color>" ), you.get_per() )
        };
        if( you.ranged_per_mod() > 0 ) {
            values.push_back( string_format( _( "Aiming penalty: <color_white>%+d</color>" ),
                                             -you.ranged_per_mod() ) );
        }
        return info( _( "Perception is the most important stat for ranged combat.  It's also used for "
                        "detecting traps and other things of interest." ), values );
    } else if( line == 4 ) {
        return info( _( "Your weight is a general indicator of how much fat your body has stored up,"
                        " which in turn shows how prepared you are to survive for a time without food."
                        "  Having too much, or too little, can be unhealthy." ), {
            display::weight_long_description( you )
        } );
    } else if( line == 5 ) {
        return info( _( "How healthy you feel.  Exercise, vitamins, sleep and not ingesting poison will increase this over time." ), {} );
    } else if( line == 6 ) {
        return info( _( "Your height.  Simply how tall you are." ), { you.height_string() } );
    } else if( line == 7 ) {
        return info( _( "This is how old you are." ), { you.age_string() } );
    } else if( line == 8 ) {
        return info( _( "This is your blood type and Rh factor." ), {
            string_format( _( "Blood type: %s" ), io::enum_to_string( you.my_blood_type ) ),
            string_format( _( "Rh factor: %s" ),
                           you.blood_rh_factor ? _( "positive (+)" ) : _( "negative (-)" ) )
        } );
    }
    return std::string();
}

static std::vector<sheet_row> encumbrance_rows( const Character &you )
{
    std::vector<sheet_row> rows;
    for( const std::pair<std::string, std::string> &line : you.encumbrance_lines() ) {
        rows.push_back( { line.first, line.second } );
    }
    return rows;
}

static std::string encumbrance_info( const Character &you, const unsigned line )
{
    const std::vector<std::pair<bodypart_id, bool>> bps = list_and_combine_bps( you, nullptr );
    bodypart_id bp;
    if( line < bps.size() ) {
        bp = bps[line].first;
    }
    return string_join( get_encumbrance_description( you, bp ), "\n" );
}

static std::vector<sheet_row> traits_rows( const std::vector<trait_and_var> &traitslist )
{
    std::vector<sheet_row> rows;
    for( const trait_and_var &cur : traitslist ) {
        rows.push_back( { colorize( cur.name(), cur.trait->get_display_color() ), std::string() } );
    }
    return rows;
}

static std::string traits_info( const Character &you, const unsigned line,
                                const std::vector<trait_and_var> &traitslist )
{
    if( line >= traitslist.size() ) {
        return std::string();
    }
    const trait_and_var &cur = traitslist[line];
    std::string trait_desc = you.mutation_desc( cur.trait );
    if( !you.purifiable( cur.trait ) ) {
        trait_desc +=
            _( "\n<color_yellow>This trait is an intrinsic part of you now, purifier won't be able to remove it.</color>" );
    }
    return string_format( "%s: %s", colorize( cur.name(), cur.trait->get_display_color() ), trait_desc );
}

static std::string bionics_power( const Character &you )
{
    int power_amount;
    std::string power_unit;
    if( you.get_power_level() < 1_J ) {
        power_amount = units::to_millijoule( you.get_power_level() );
        power_unit = pgettext( "energy unit: millijoule", "mJ" );
    } else if( you.get_power_level() < 1_kJ ) {
        power_amount = units::to_joule( you.get_power_level() );
        power_unit = pgettext( "energy unit: joule", "J" );
    } else {
        power_amount = units::to_kilojoule( you.get_power_level() );
        power_unit = pgettext( "energy unit: kilojoule", "kJ" );
    }
    return string_format( _( "Power: <color_light_blue>%1$d %2$s</color>"
                             " / <color_light_blue>%3$d kJ</color>" ),
                          power_amount, power_unit, units::to_kilojoule( you.get_max_power_level() ) );
}

static std::vector<sheet_row> bionics_rows( const std::vector<bionic_grouping> &bionicslist )
{
    std::vector<sheet_row> rows;
    for( const bionic_grouping &bio_group : bionicslist ) {
        std::string name = bio_group.name.translated( bio_group.installed_count );
        if( bio_group.installed_count > 1 ) {
            name += string_format( " (%d)", bio_group.installed_count );
        }
        rows.push_back( { colorize( name, c_white ), std::string() } );
    }
    return rows;
}

static std::vector<sheet_row> effects_rows( const
        std::vector<std::pair<std::string, std::string>> &effect_name_and_text )
{
    std::vector<sheet_row> rows;
    for( const std::pair<std::string, std::string> &effect : effect_name_and_text ) {
        rows.push_back( { colorize( effect.first, c_light_gray ), std::string() } );
    }
    return rows;
}

static std::vector<sheet_row> skills_rows( const Character &you,
        const std::vector<HeaderSkill> &skillslist )
{
    std::vector<sheet_row> rows;
    for( const HeaderSkill &header_skill : skillslist ) {
        const Skill *aSkill = header_skill.skill;
        if( header_skill.is_header ) {
            const SkillDisplayType t = SkillDisplayType::get_skill_type( aSkill->display_category() );
            rows.push_back( { colorize( t.display_string(), c_yellow ), std::string(), true } );
            continue;
        }
        const SkillLevel &level = you.get_skill_level_object( aSkill->ident() );
        int exercise = level.knowledgeExperience();
        int level_num = level.knowledgeLevel();
        const bool can_train = level.can_train();
        const bool skill_gap = level_num > level.level();
        const bool skill_small_gap = exercise > level.exercise();
        bool locked = false;
        if( you.has_active_bionic( bio_cqb ) && is_cqb_skill( aSkill->ident() ) ) {
            level_num = 5;
            exercise = 0;
            locked = true;
        }
        level_num = you.enchantment_cache->modify_value( aSkill->ident(), level_num );
        nc_color cstatus;
        if( locked ) {
            cstatus = c_yellow;
        } else if( skill_gap || skill_small_gap ) {
            cstatus = c_light_cyan;
        } else if( !can_train ) {
            cstatus = c_white;
        } else {
            cstatus = c_light_blue;
        }
        std::string value;
        if( aSkill->ident() == skill_dodge ) {
            value = string_format( "%4.1f/%-2d(%2d%%)", you.get_dodge(), level_num,
                                   ( exercise < 0 ? 0 : exercise ) );
        } else {
            value = string_format( "%-2d(%2d%%)", level_num, ( exercise < 0 ? 0 : exercise ) );
        }
        rows.push_back( { colorize( aSkill->name() + ":", cstatus ), colorize( value, cstatus ) } );
    }
    return rows;
}

static std::string skills_info( const Character &you, unsigned int line,
                                const std::vector<HeaderSkill> &skillslist )
{
    if( line >= skillslist.size() || skillslist[line].is_header ) {
        return std::string();
    }
    const Skill *selectedSkill = skillslist[line].skill;
    const SkillLevel &level = you.get_skill_level_object( selectedSkill->ident() );
    std::string info_text = selectedSkill->description();
    info_text = string_format( _( "%s\n%s\n%s" ), info_text,
                               selectedSkill->get_level_description( level.knowledgeLevel(), false ),
                               selectedSkill->get_level_description( level.level(), true ) );
    float level_gap = 100.0f * std::max( level.knowledgeLevel(), 1 ) / std::max( level.level(), 1 );
    if( level.knowledgeLevel() == 1 && level.level() == 0 ) {
        level_gap = 150.0f;
    }
    float learning_bonus = 100.0f * std::max( ( 1.0f + you.get_int() / 40.0f ) - 0.1f *
                           level.exercise() / level.knowledgeExperience(), 1.0f );
    if( level.isRusty() ) {
        info_text = string_format( _( "%s\n\nPractical level: %d (%d%%) " ), info_text,
                                   level.level(), level.exercise() );
        if( level.knowledgeLevel() > level.level() ) {
            info_text = string_format( _( "%s| Learning bonus: %.0f%%" ), info_text,
                                       level_gap );
        } else {
            info_text = string_format( _( "%s| Learning bonus: %.0f%%" ), info_text,
                                       learning_bonus );
        }
    }
    return info_text;
}

static std::vector<sheet_row> speed_rows( const std::vector<speedlist_entry> &speedlist )
{
    std::vector<sheet_row> rows;
    for( const speedlist_entry &entry : speedlist ) {
        // +speed is good, -movecost is good
        const nc_color col = entry.is_speed == ( entry.val > 0 ) ? c_light_green : c_red;
        //~ single character abbreviations for Speed and Move Cost
        std::string value = entry.is_speed ? _( "S" ) : _( "M" );
        if( entry.val != 0 ) {
            const char prefix = entry.val < 0 ? '-' : '+';
            const char suffix = entry.percent ? '%' : ' ';
            if( std::abs( entry.val ) < 1000 ) {
                value += string_format( "%c%3d%c", prefix, std::abs( entry.val ), suffix );
            } else {
                // broken limbs or very big numbers can cause this
                value += string_format( "%c!!!%c", prefix, suffix );
            }
        }
        rows.push_back( { colorize( entry.description, col ), colorize( value, col ) } );
    }
    return rows;
}

static std::string speed_info( unsigned int line, const std::vector<speedlist_entry> &speedlist )
{
    if( line >= speedlist.size() ) {
        return std::string();
    }
    if( speedlist[line].val == 0 ) {
        return speedlist[line].description;
    }
    return string_format(
               //~ One of a list of reasons speed or movecost are being changed
               _( "Cause: %s" ), speedlist[line].description ) + "\n" + string_format(
               //~ speed or movecost reduced by a constant or percentage
               speedlist[line].val < 0 ? _( "Effect: %1$s reduced by %2$d%3$c" ) :
               //~ speed or movecost increased by a constant or percentage
               _( "Effect: %1$s increased by %2$d%3$c" ),
               speedlist[line].is_speed ? _( "Speed" ) : _( "Move Cost" ),
               std::abs( speedlist[line].val ),
               speedlist[line].percent ? '%' : ' ' );
}

static std::vector<sheet_row> proficiencies_rows( const Character &guy )
{
    std::vector<sheet_row> rows;
    for( const display_proficiency &cur : guy.display_proficiencies() ) {
        std::string progress;
        if( !cur.known && cur.id->can_learn() ) {
            progress = colorize( string_format( "%2.0f%%", std::floor( cur.practice * 100 ) ), cur.color );
        }
        rows.push_back( { colorize( cur.id->name(), cur.color ), progress } );
    }
    return rows;
}

static std::string proficiencies_info( const unsigned line, const Character &guy )
{
    const std::vector<display_proficiency> profs = guy.display_proficiencies();
    if( line >= profs.size() ) {
        return std::string();
    }
    const display_proficiency &cur = profs[line];
    std::string progress;
    if( cur.known ) {
        progress = _( "You know this proficiency." );
    } else {
        progress = string_format( _( "You are %.2f%% of the way towards learning this proficiency." ),
                                  cur.practice * 100 );
        if( debug_mode ) {
            progress += string_format( "\nYou have spent %s practicing this proficiency.",
                                       to_string( cur.spent ) );
        }
    }
    return colorize( cur.id->name(), cur.color )
           + "\n" + colorize( progress, c_cyan )
           + "\n" + colorize( cur.id->description(), c_white );
}

static std::string info_text( const Character &you, const unsigned line,
                              const player_display_tab curtab,
                              const std::vector<speedlist_entry> &speedlist,
                              const std::vector<trait_and_var> &traitslist,
                              const std::vector<bionic_grouping> &bionicslist,
                              const std::vector<std::pair<std::string, std::string>> &effect_name_and_text,
                              const std::vector<HeaderSkill> &skillslist )
{
    switch( curtab ) {
        case player_display_tab::stats:
            return stats_info( you, line );
        case player_display_tab::encumbrance:
            return encumbrance_info( you, line );
        case player_display_tab::speed:
            return speed_info( line, speedlist );
        case player_display_tab::skills:
            return skills_info( you, line, skillslist );
        case player_display_tab::traits:
            return traits_info( you, line, traitslist );
        case player_display_tab::bionics:
            return line < bionicslist.size() ? bionicslist[line].description.translated() : std::string();
        case player_display_tab::effects:
            return line < effect_name_and_text.size() ? effect_name_and_text[line].second : std::string();
        case player_display_tab::proficiencies:
            return proficiencies_info( line, you );
        case player_display_tab::num_tabs:
            cata_fatal( "Invalid curtab" );
    }
    return std::string();
}

static std::string sheet_title( const Character &you, const std::string &race )
{
    // Print name and header
    if( you.crossed_threshold() ) {
        //~ player info window: 1s - name, 2s - gender, 3s - Prof or Mutation name
        return string_format( _( " %1$s | %2$s | %3$s" ), you.get_name(),
                              you.male ? _( "Male" ) : _( "Female" ), race );
    }
    std::string profession = you.disp_profession();
    if( !profession.empty() ) {
        return string_format( _( " %1$s | %2$s | %3$s " ), you.get_name(),
                              you.male ? _( "Male" ) : _( "Female" ), profession );
    }
    return string_format( _( " %1$s | %2$s " ), you.get_name(),
                          you.male ? _( "Male" ) : _( "Female" ) );
}

static void on_customize_character( Character &you )
{
    uilist cmenu;
    cmenu.title = _( "Customize Character" );
    cmenu.addentry( 1, true, 'y', _( "Change gender" ) );
    cmenu.addentry( 2, true, 'n', _( "Change name" ) );

    cmenu.query();
    if( cmenu.ret == 1 ) {
        you.male = !you.male;
        popup( _( "Gender set to %s." ), you.male ? _( "Male" ) : _( "Female" ) );
    } else if( cmenu.ret == 2 ) {
        std::string filterstring = you.play_name.value_or( std::string() );
        string_input_popup popup;
        popup
        .title( _( "New name ( leave empty to reset ):" ) )
        .width( 85 )
        .max_length( NAME_CHARACTER_LIMIT )
        .edit( filterstring );
        if( popup.confirmed() ) {
            if( filterstring.empty() ) {
                you.play_name.reset();
            } else {
                you.play_name = filterstring;
            }
        }
    }
}

static void change_armor_sprite( Character &you )
{
    item_location target_loc;
    target_loc = game_menus::inv::change_sprite( you );
    if( target_loc && target_loc.get_item() ) {
        item *target_item = target_loc.get_item();

        uilist menu;
        menu.title = _( "Change sprite" );
        menu.addentry( 0, true, MENU_AUTOASSIGN, _( "Select sprite from items" ) );
        menu.addentry( 1, true, MENU_AUTOASSIGN, _( "Restore default sprite" ) );
        menu.addentry( 2, true, MENU_AUTOASSIGN, _( "Cancel" ) );

        menu.query();
        if( menu.ret == 0 ) {
            item_location sprite_loc;
            auto armor_filter = [&]( const item & i ) {
                return i.is_armor();
            };
            sprite_loc = game_menus::inv::titled_filter_menu( armor_filter,
                         you,
                         _( "Select appearance of this armor:" ),
                         -1,
                         _( "You have nothing to wear." ) );
            if( sprite_loc && sprite_loc.get_item() ) {
                const item *sprite_item = sprite_loc.get_item();
                const std::string variant = sprite_item->has_itype_variant() ? sprite_item->itype_variant().id : "";
                target_item->set_var( "sprite_override", sprite_item->typeId().str() );
                target_item->set_var( "sprite_override_variant", variant );
            }
        } else if( menu.ret == 1 ) {
            target_item->erase_var( "sprite_override" );
            target_item->erase_var( "sprite_override_variant" );
        }
    }
}

static int get_line_count( player_display_tab curtab,
                           Character &you,
                           const std::vector<speedlist_entry> &speedlist,
                           std::vector<trait_and_var> &traitslist,
                           const std::vector<bionic_grouping> &bionicslist,
                           const std::vector<std::pair<std::string,
                           std::string>> &effect_name_and_text,
                           const std::vector<HeaderSkill> &skillslist )
{
    switch( curtab ) {
        case player_display_tab::stats:
            return 9;
        case player_display_tab::encumbrance: {
            const std::vector<std::pair<bodypart_id, bool>> bps = list_and_combine_bps( you, nullptr );
            return bps.size();
        }
        case player_display_tab::speed:
            return speedlist.size();
        case player_display_tab::traits:
            return traitslist.size();
        case player_display_tab::bionics:
            return bionicslist.size();
        case player_display_tab::effects:
            return effect_name_and_text.size();
        case player_display_tab::skills:
            return skillslist.size();
        case player_display_tab::proficiencies:
            return you.display_proficiencies().size();
        case player_display_tab::num_tabs:
            cata_fatal( "Invalid curtab" );
    }
    return 0;
}

static std::vector<speedlist_entry> get_speedlist_entries( const Character &you, int &move_cost,
        const std::map<std::string, int> &speed_effects )
{
    std::vector<speedlist_entry> entries;

    for( const std::pair<const std::string, int> &speed_effect : speed_effects ) {
        if( speed_effect.second != 0 ) {
            const speedlist_entry entry { true, speed_effect.first, speed_effect.second, false };
            entries.push_back( entry );
        }
    }

    for( const speed_bonus_effect &effect : you.get_speed_bonus_effects() ) {
        if( effect.bonus != 0 && speed_effects.end() == speed_effects.find( effect.description ) ) {
            const speedlist_entry entry { true, effect.description, effect.bonus, false };
            entries.push_back( entry );
        }
    }


    float movecost = 100;
    for( const run_cost_effect &effect : you.run_cost_effects( movecost ) ) {
        const int percent = std::trunc( effect.times * 100 - 100 ); // truncate toward 0%
        const int constant = std::trunc( effect.plus ); // truncate toward 0
        if( ( effect.times != 1.0 && percent == 0 ) || ( effect.plus != 0 && constant == 0 ) ) {
            continue; // negligible effect
        }
        const speedlist_entry entry { false, effect.description, percent != 0 ? percent : constant, percent != 0 };
        entries.push_back( entry );
    }
    move_cost = static_cast<int>( movecost );

    return entries;
}

static bool handle_player_display_action( Character &you, unsigned int &line,
        player_display_tab &curtab, input_context &ctxt, character_sheet &sheet,
        const std::vector<speedlist_entry> &speedlist,
        std::vector<trait_and_var> &traitslist,
        const std::vector<bionic_grouping> &bionicslist,
        const std::vector<std::pair<std::string, std::string>> &effect_name_and_text,
        const std::vector<HeaderSkill> &skillslist, bool customize_character )
{
    unsigned int line_count = get_line_count( curtab, you, speedlist, traitslist, bionicslist,
                              effect_name_and_text, skillslist );

    if( line_count > 0 ) {
        line = std::clamp( line, 0U, line_count - 1 );
        if( curtab == player_display_tab::skills ) {
            skip_skill_headers( skillslist, line, true );
        }
    } else {
        line = 0;
    }

    bool done = false;
    std::string action = ctxt.handle_input();

    // The line under the mouse, in whichever section it is
    const int mouse_section = sheet.hovered_index( "data-sec" );
    const int mouse_line = sheet.hovered_index( "data-row" );
    const bool over_line = mouse_section >= 0 &&
                           mouse_section < static_cast<int>( player_display_tab::num_tabs ) && mouse_line >= 0;
    bool clicked_button = false;
    if( action == "SELECT" ) {
        const std::string clicked = sheet.resolve_click( action );
        if( clicked != action ) {
            action = clicked;
            clicked_button = true;
        } else if( over_line ) {
            // Clicking a line acts on it as confirming it would
            curtab = player_display_tab( mouse_section );
            line = mouse_line;
            action = "CONFIRM";
        } else {
            return done;
        }
    }

    if( navigate_ui_list( action, line, 3, line_count, true ) ) {
        if( curtab == player_display_tab::skills ) {
            // including actios "PAGE_UP", and "HOME", because these shouldn't wrap
            const bool inc = action == "DOWN" || action == "PAGE_DOWN" || action == "PAGE_UP" ||
                             action == "SCROLL_DOWN" || action == "HOME";
            skip_skill_headers( skillslist, line, inc );
        }
    } else if( action == "MOUSE_MOVE" ) {
        if( over_line ) {
            curtab = player_display_tab( mouse_section );
            line = mouse_line;
        }
    } else if( action == "LEFT" || action == "PREV_TAB" || action == "RIGHT" || action == "NEXT_TAB" ) {
        curtab = inc_clamp_wrap( curtab, action == "RIGHT" || action == "NEXT_TAB",
                                 player_display_tab::num_tabs );
        line = curtab == player_display_tab::skills ? 1 : 0; // avoid a call to skip_skill_headers
    } else if( action == "SELECT_TRAIT_VARIANT" ) {
        if( curtab == player_display_tab::traits && line < traitslist.size() ) {
            const mutation_variant *var = traitslist[line].trait->pick_variant_menu();
            you.set_mut_variant( traitslist[line].trait, var );
            const std::string &varid = var == nullptr ? "" : var->id;
            traitslist[line].variant = varid;
        }
    } else if( action == "QUIT" ) {
        done = true;
    } else if( action == "CONFIRM" ) {
        if( curtab == player_display_tab::proficiencies ) {
            const std::vector<display_proficiency> profs = you.display_proficiencies();
            if( line < profs.size() ) {
                show_proficiencies_window( you, profs[line].id );
            }
        }
    } else if( action == "CHANGE_PROFESSION_NAME" ) {
        string_input_popup popup;
        popup.title( _( "Profession Name: " ) )
        .width( 25 )
        .text( "" )
        .max_length( 25 )
        .query();

        you.custom_profession = popup.text();
    } else if( action == "VIEW_PROFICIENCIES" ) {
        show_proficiencies_window( you );
    } else if( action == "morale" ) {
        you.disp_morale( );
    } else if( action == "VIEW_BODYSTAT" ) {
        display_bodygraph( you );
    } else if( customize_character && action == "SWITCH_GENDER" ) {
        on_customize_character( you );
    } else if( action == "HELP_KEYBINDINGS" ) {
        // A key has shown them already, inside handle_input()
        if( clicked_button ) {
            ctxt.display_menu();
        }
    } else if( action == "SCROLL_INFOBOX_UP" ) {
        sheet.scroll_info( rml_ui::scroll::line_up );
    } else if( action == "SCROLL_INFOBOX_DOWN" ) {
        sheet.scroll_info( rml_ui::scroll::line_down );
    } else if( action == "MEDICAL_MENU" ) {
        if( you.disp_medical() ) {
            done = true;
        }
    } else if( action == "SELECT_STATS_TAB" ) {
        curtab = player_display_tab::stats;
        line = 0;
    } else if( action == "SELECT_ENCUMBRANCE_TAB" ) {
        curtab = player_display_tab::encumbrance;
        line = 0;
    } else if( action == "SELECT_SKILLS_TAB" ) {
        curtab = player_display_tab::skills;
        line = 1; // avoid a call to skip_skill_headers
    } else if( action == "SELECT_TRAITS_TAB" ) {
        curtab = player_display_tab::traits;
        line = 0;
    } else if( action == "SELECT_BIONICS_TAB" ) {
        curtab = player_display_tab::bionics;
        line = 0;
    } else if( action == "SELECT_EFFECTS_TAB" ) {
        curtab = player_display_tab::effects;
        line = 0;
    } else if( action == "SELECT_PROFICIENCIES_TAB" ) {
        curtab = player_display_tab::proficiencies;
        line = 0;
    } else if( action == "CHANGE_ARMOR_SPRITE" ) {
        change_armor_sprite( you );
    }
    return done;
}

void Character::disp_info( bool customize_character )
{
    // Customizing any character is always enabled in debug mode
    customize_character |= debug_mode;

    std::vector<std::pair<std::string, std::string>> effect_name_and_text;
    for( auto &elem : *effects ) {
        for( auto &_effect_it : elem.second ) {
            const std::string name = _effect_it.second.disp_name();
            effect_name_and_text.emplace_back( name,
                                               _effect_it.second.disp_desc() + '\n' + _effect_it.second.disp_mod_source_info() );
        }
    }
    if( get_perceived_pain() > 0 ) {
        const stat_mod ppen = read_pain_penalty();
        std::pair<std::string, nc_color> pain_desc = display::pain_text_color( *this );
        std::string pain_text;
        pain_desc.first = string_format( _( "You are in %s\n" ), pain_desc.first );
        pain_text += colorize( pain_desc.first, pain_desc.second );
        const auto add_if = [&]( const int amount, const char *const name ) {
            if( amount > 0 ) {
                pain_text += string_format( name, amount ) + "   ";
            }
        };
        add_if( ppen.strength, _( "Strength -%d" ) );
        add_if( ppen.dexterity, _( "Dexterity -%d" ) );
        add_if( ppen.intelligence, _( "Intelligence -%d" ) );
        add_if( ppen.perception, _( "Perception -%d" ) );
        add_if( ppen.speed, _( "Speed -%d %%" ) );
        effect_name_and_text.emplace_back( _( "Pain" ), pain_text );
    }

    const float bmi = get_bmi_fat();

    if( bmi < character_weight_category::underweight ) {
        std::string starvation_name;
        std::string starvation_text;

        if( bmi < character_weight_category::emaciated ) {
            starvation_name = _( "Severely Malnourished" );
            starvation_text =
                _( "Your body is severely weakened by starvation.  You might die if you don't start eating regular meals!\n\n" );
        } else {
            starvation_name = _( "Malnourished" );
            starvation_text =
                _( "Your body is weakened by starvation.  Only time and regular meals will help you recover.\n\n" );
        }

        if( bmi < character_weight_category::normal ) {
            const stat_mod wpen = get_weight_penalty();
            starvation_text += std::string( _( "Strength" ) ) + " -" + string_format( "%d\n",
                               wpen.strength );
            starvation_text += std::string( _( "Dexterity" ) ) + " -" + string_format( "%d\n",
                               wpen.dexterity );
            starvation_text += std::string( _( "Intelligence" ) ) + " -" + string_format( "%d",
                               wpen.intelligence );
        }

        effect_name_and_text.emplace_back( starvation_name, starvation_text );
    }

    if( has_trait( trait_TROGLO3 ) && g->is_in_sunlight( pos_bub() ) ) {
        effect_name_and_text.emplace_back( _( "In Sunlight" ),
                                           _( "The sunlight irritates you terribly.\n"
                                              "Strength - 4;    Dexterity - 4;    Intelligence - 4;    Perception - 4" )
                                         );
    } else if( has_trait( trait_TROGLO2 ) && g->is_in_sunlight( pos_bub() ) ) {
        if( incident_sun_irradiance( get_weather().weather_id, calendar::turn ) > irradiance::moderate ) {
            effect_name_and_text.emplace_back( _( "In Sunlight" ),
                                               _( "The sunlight irritates you badly.\n"
                                                  "Strength - 2;    Dexterity - 2;    Intelligence - 2;    Perception - 2" ) );
        } else if( incident_sun_irradiance( get_weather().weather_id, calendar::turn ) > irradiance::low ) {
            effect_name_and_text.emplace_back( _( "In Sunlight" ),
                                               _( "The sunlight irritates you badly.\n"
                                                  "Strength - 1;    Dexterity - 1;    Intelligence - 1;    Perception - 1" ) );
        }

    } else if( has_trait( trait_TROGLO ) && g->is_in_sunlight( pos_bub() ) &&
               incident_sun_irradiance( get_weather().weather_id, calendar::turn ) > irradiance::moderate ) {
        effect_name_and_text.emplace_back( _( "In Sunlight" ),
                                           _( "The sunlight irritates you.\n"
                                              "Strength - 1;    Dexterity - 1;    Intelligence - 1;    Perception - 1" )
                                         );
    }

    for( addiction &elem : addictions ) {
        if( elem.sated < 0_turns && elem.intensity >= MIN_ADDICTION_LEVEL ) {
            effect_name_and_text.emplace_back( elem.type->get_name().translated(),
                                               elem.type->get_description().translated() );
        }
    }

    for( const std::pair<std::string, std::string> &detail : enchantment_cache->details ) {
        effect_name_and_text.emplace_back( detail );
    }

    // If any effects with no names have cropped up, let's remove them.
    //   NB: This can happen if an effect no longer exists.
    effect_name_and_text.erase( std::remove_if( effect_name_and_text.begin(),
    effect_name_and_text.end(), []( const std::pair<std::string, std::string> &e ) {
        return e.first.empty();
    } ), effect_name_and_text.end() );

    std::vector<trait_and_var> traitslist = get_mutations_variants( false );
    std::sort( traitslist.begin(), traitslist.end(), trait_var_display_sort );

    std::vector<bionic_grouping> bionicslist;
    {
        // count installed bionics by type and sort by localized name
        const auto bio_comp = []( const bionic_data & lhs, const bionic_data & rhs ) {
            return lhs.name.translated_lt( rhs.name );
        };
        std::map<const bionic_data, int, decltype( bio_comp )> bionics_map( bio_comp );
        for( const bionic &bio : *my_bionics ) {
            bionics_map[bio.info()]++;
        }
        for( const auto &pair : bionics_map ) {
            bionicslist.push_back( { pair.first.name, pair.first.description, pair.second } );
        }
    }

    const std::vector<const Skill *> player_skill = Skill::get_skills_for_chr_display( *this,
    [&]( const Skill & a, const Skill & b ) {
        return a.get_sort_rank() < b.get_sort_rank();
    } );

    std::vector<HeaderSkill> skillslist = get_HeaderSkills( player_skill );
    // Print name and header
    // Post-humanity trumps your pre-Cataclysm life
    // Unless you have a custom profession.
    std::string race;
    if( custom_profession.empty() && crossed_threshold() ) {
        for( const trait_and_var &mut : get_mutations_variants() ) {
            if( mut.trait->threshold ) {
                race = mut.name();
                break;
            }
        }
    }

    input_context ctxt( "PLAYER_INFO" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "LEFT", to_translation( "Cycle to next category" ) );
    ctxt.register_action( "RIGHT", to_translation( "Cycle to previous category" ) );
    ctxt.register_action( "NEXT_TAB", to_translation( "Cycle to next category" ) );
    ctxt.register_action( "PREV_TAB", to_translation( "Cycle to previous category" ) );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM", to_translation( "Upgrade stat" ) );
    ctxt.register_action( "CHANGE_PROFESSION_NAME", to_translation( "Change profession name" ) );
    ctxt.register_action( "SWITCH_GENDER", to_translation( "Customize base appearance and name" ) );
    ctxt.register_action( "VIEW_PROFICIENCIES", to_translation( "View character proficiencies" ) );
    ctxt.register_action( "VIEW_BODYSTAT", to_translation( "View character's body status" ) );
    ctxt.register_action( "morale" );
    ctxt.register_action( "SCROLL_INFOBOX_UP", to_translation( "Scroll information box up" ) );
    ctxt.register_action( "SCROLL_INFOBOX_DOWN", to_translation( "Scroll information box down" ) );
    ctxt.register_action( "SELECT_TRAIT_VARIANT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "MEDICAL_MENU" );
    ctxt.register_action( "CHANGE_ARMOR_SPRITE" );
    ctxt.register_action( "SELECT_STATS_TAB" );
    ctxt.register_action( "SELECT_ENCUMBRANCE_TAB" );
    ctxt.register_action( "SELECT_SKILLS_TAB" );
    ctxt.register_action( "SELECT_TRAITS_TAB" );
    ctxt.register_action( "SELECT_BIONICS_TAB" );
    ctxt.register_action( "SELECT_EFFECTS_TAB" );
    ctxt.register_action( "SELECT_PROFICIENCIES_TAB" );

    std::map<std::string, int> speed_effects;
    for( auto &elem : *effects ) {
        for( std::pair<const bodypart_id, effect> &_effect_it : elem.second ) {
            effect &it = _effect_it.second;
            bool reduced = resists_effect( it );
            int move_adjust = it.get_mod( "SPEED", reduced );
            if( move_adjust != 0 ) {
                const std::string dis_text = it.get_speed_name();
                speed_effects[dis_text] += move_adjust;
            }
        }
    }

    int move_cost;
    std::vector<speedlist_entry> speedlist = get_speedlist_entries( *this, move_cost, speed_effects );

    player_display_tab curtab = player_display_tab::stats;
    // line marks at which line is selection in tab window, could be named tab_line
    unsigned int line = 0;
    // The selection as last shown, to scroll to it and to the top of its info when it moves
    std::pair<player_display_tab, unsigned int> shown_selection( player_display_tab::num_tabs, 0 );

    character_sheet sheet( [&]( character_sheet & win ) {
        win.set_size( rml_ui::chars_width( 120 ), "96%" );
        win.set_title( colorize( sheet_title( *this, race ), c_white ) );

        std::string buttons;
        if( customize_character ) {
            buttons += rml_ui::button_rml( "SWITCH_GENDER", rml_ui::markup_to_rml( string_format(
                                               _( "[<color_yellow>%s</color>] Customize " ), ctxt.get_desc( "SWITCH_GENDER" ) ),
                                           c_light_gray ) );
        }
        buttons += rml_ui::button_rml( "morale", rml_ui::markup_to_rml( string_format(
                                           _( "[<color_yellow>%s</color>] Morale" ), ctxt.get_desc( "morale" ) ), c_light_gray ) );
        buttons += rml_ui::button_rml( "HELP_KEYBINDINGS", rml_ui::markup_to_rml( string_format(
                                           _( "[<color_yellow>%s</color>]" ), ctxt.get_desc( "HELP_KEYBINDINGS" ) ), c_light_gray ) );
        win.set_header( "<div class=\"right\">" + buttons + "</div>" );

        const int speed = get_speed();
        const std::string speed_title = rml_ui::markup_to_rml( string_format( "%s: <color_%s>%d</color>",
                                        _( title_SPEED ), speed >= 100 ? "light_green" : "red", speed ), c_light_gray );
        const std::string move_cost_rml = "<div class=\"cs-sub\">" + rml_ui::markup_to_rml(
                                              string_format( "%s: <color_%s>%d</color>", _( title_MOVE_COST ),
                                                      move_cost <= 100 ? "light_green" : "red", move_cost ), c_light_gray ) + "</div>";
        const std::string power_rml = "<div class=\"cs-sub\">" + rml_ui::markup_to_rml(
                                          bionics_power( *this ), c_white ) + "</div>";

        win.show( "body",
                  "<div id=\"cs-top\">" +
                  section_rml( player_display_tab::stats, curtab, line,
                               section_title_rml( ctxt, _( title_STATS ), "SELECT_STATS_TAB", "VIEW_BODYSTAT" ),
                               std::string(), stats_rows( *this ) ) +
                  section_rml( player_display_tab::encumbrance, curtab, line,
                               section_title_rml( ctxt, _( title_ENCUMB ), "SELECT_ENCUMBRANCE_TAB" ),
                               std::string(), encumbrance_rows( *this ) ) +
                  section_rml( player_display_tab::speed, curtab, line,
                               "<span class=\"cs-name\">" + speed_title + "</span>", move_cost_rml,
                               speed_rows( speedlist ) ) +
                  "</div><div id=\"cs-info\">" +
                  rml_ui::markup_to_rml( info_text( *this, line, curtab, speedlist, traitslist, bionicslist,
                                         effect_name_and_text, skillslist ), c_light_gray ) +
                  "</div><div id=\"cs-bottom\"><div class=\"cs-column\">" +
                  section_rml( player_display_tab::skills, curtab, line,
                               section_title_rml( ctxt, _( title_SKILLS ), "SELECT_SKILLS_TAB" ),
                               std::string(), skills_rows( *this, skillslist ) ) +
                  "</div><div class=\"cs-column\">" +
                  section_rml( player_display_tab::traits, curtab, line,
                               section_title_rml( ctxt, _( title_TRAITS ), "SELECT_TRAITS_TAB" ),
                               std::string(), traits_rows( traitslist ) ) +
                  section_rml( player_display_tab::bionics, curtab, line,
                               section_title_rml( ctxt, _( title_BIONICS ), "SELECT_BIONICS_TAB" ),
                               power_rml, bionics_rows( bionicslist ) ) +
                  "</div><div class=\"cs-column\">" +
                  section_rml( player_display_tab::effects, curtab, line,
                               section_title_rml( ctxt, _( title_EFFECTS ), "SELECT_EFFECTS_TAB" ),
                               std::string(), effects_rows( effect_name_and_text ) ) +
                  section_rml( player_display_tab::proficiencies, curtab, line,
                               section_title_rml( ctxt, _( title_PROFICIENCIES ), "SELECT_PROFICIENCIES_TAB",
                                       "VIEW_PROFICIENCIES" ),
                               std::string(), proficiencies_rows( *this ) ) +
                  "</div></div>" );

        if( shown_selection != std::make_pair( curtab, line ) ) {
            shown_selection = std::make_pair( curtab, line );
            win.scroll_info( rml_ui::scroll::top );
            win.reveal( string_format( "cs-row-%d-%d", static_cast<int>( curtab ), static_cast<int>( line ) ) );
        }
    } );

    bool done = false;
    do {
        ui_manager::redraw_invalidated();

        done = handle_player_display_action( *this, line, curtab, ctxt, sheet, speedlist, traitslist,
                                             bionicslist, effect_name_and_text, skillslist, customize_character );
    } while( !done );
}

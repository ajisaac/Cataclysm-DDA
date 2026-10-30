#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar_action.h"
#include "bodypart.h"
#include "calendar.h"
#include "catacharset.h"
#include "character.h"
#include "character_attire.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "debug.h"
#include "display.h"
#include "effect.h"
#include "enum_traits.h"
#include "enums.h"
#include "flag.h"
#include "game.h"
#include "game_constants.h"
#include "input_context.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "math_parser_diag_value.h"
#include "messages.h"
#include "output.h"
#include "pimpl.h"
#include "proficiency.h"
#include "requirements.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "skill.h"
#include "string_formatter.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "uilist.h"
#include "units.h"
#include "units_utility.h"
#include "weighted_list.h"
#include "wound.h"

static const efftype_id effect_bite( "bite" );
static const efftype_id effect_bleed( "bleed" );
static const efftype_id effect_infected( "infected" );
static const efftype_id effect_mending( "mending" );

static const json_character_flag json_flag_BIONIC_LIMB( "BIONIC_LIMB" );
static const json_character_flag json_flag_PAIN_IMMUNE( "PAIN_IMMUNE" );

static const trait_id trait_DEBUG_HS( "DEBUG_HS" );
static const trait_id trait_SUNLIGHT_DEPENDENT( "SUNLIGHT_DEPENDENT" );

template <typename T> struct enum_traits;

namespace
{
enum class medical_tab_mode {
    TAB_LIMBS,
    TAB_SUMMARY,
    last
};
} // namespace

template<>
struct enum_traits<medical_tab_mode> {
    static constexpr medical_tab_mode last = medical_tab_mode::last;
    static constexpr medical_tab_mode first = medical_tab_mode::TAB_LIMBS;
};

namespace
{
class medical_ui : public rml_ui::window
{
    public:
        explicit medical_ui( Character *guy ) : rml_ui::window( _( "Medical" ) ) {
            you = guy;
        };

        bool execute();
        std::string get_limb_effects( const bodypart_id &part ) const;
        std::string get_limb_wounds( const bodypart_id &part ) const;
        std::string get_limb_scores_and_modifiers( const bodypart_id &part ) const;
        std::string get_modified_limb_name( const bodypart_id &part ) const;

    protected:
        void on_redraw() override;

    private:
        // The body parts listed, with `bp` among them
        std::vector<bodypart_id> body_parts();
        std::string limbs_list_rml( const std::vector<bodypart_id> &bodyparts ) const;
        std::string limb_tab_rml( const std::vector<bodypart_id> &bodyparts ) const;
        std::string summary_tab_rml() const;
        std::string hint_rml() const;

        Character *you;
        bodypart_id bp;
        input_context ctxt;
        medical_tab_mode selected_tab = medical_tab_mode::TAB_LIMBS;
        // What was last shown, to bring a new selection into view and a new
        // limb's info to its top
        medical_tab_mode shown_tab = medical_tab_mode::last;
        std::optional<bodypart_id> shown_bp;
};
} // namespace

bool Character::disp_medical()
{
    medical_ui medic( this );
    return medic.execute();
}

bool medical_ui::execute()
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
    ctxt.register_action( "APPLY", to_translation( "Use item" ) );
    ctxt.register_action( "CONFIRM", to_translation( "Pick bodypart to be mended" ) );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.set_timeout( 16 );

    while( true ) {
        ui_manager::redraw_invalidated();
        std::string action = ctxt.handle_input();
        const std::vector<bodypart_id> bodyparts = body_parts();
        if( action == "SELECT" ) {
            const int tab = hovered_tab();
            const int row = hovered_index( "data-row" );
            if( tab >= 0 ) {
                selected_tab = static_cast<medical_tab_mode>( tab );
                continue;
            } else if( row >= 0 && static_cast<size_t>( row ) < bodyparts.size() ) {
                bp = bodyparts[row];
                continue;
            }
            action = resolve_click( action );
        }

        if( action == "QUIT" ) {
            break;
        } else if( action == "NEXT_TAB" || action == "RIGHT" ) {
            ++selected_tab;
        } else if( action == "PREV_TAB" || action == "LEFT" ) {
            --selected_tab;
        } else if( ( action == "UP" || action == "DOWN" ) && !bodyparts.empty() ) {
            const int count = static_cast<int>( bodyparts.size() );
            const int selected_limb = std::distance( bodyparts.begin(),
                                      std::find( bodyparts.begin(), bodyparts.end(), bp ) );
            bp = bodyparts[( selected_limb + ( action == "UP" ? count - 1 : 1 ) ) % count];
        } else if( action == "PAGE_UP" ) {
            scroll( "limb-info", rml_ui::scroll::page_up );
        } else if( action == "PAGE_DOWN" ) {
            scroll( "limb-info", rml_ui::scroll::page_down );
        } else if( action == "HOME" ) {
            scroll( "limb-info", rml_ui::scroll::top );
        } else if( action == "END" ) {
            scroll( "limb-info", rml_ui::scroll::bottom );
        }

        if( action == "CONFIRM" ) {
            if( you->pick_wound_fix( bp ) ) {
                return true;
            }
        }

        if( action == "APPLY" ) {
            if( you->is_avatar() ) {
                avatar_action::use_item( *you->as_avatar() );
            } else {
                popup( _( "Applying not implemented for NPCs." ) );
            }
        }

    }
    return false;
}

static std::string coloured_stat_display( int statCur, int statMax )
{
    nc_color cstatus;
    if( statCur <= 0 ) {
        cstatus = c_dark_gray;
    } else if( statCur < statMax / 2 ) {
        cstatus = c_red;
    } else if( statCur < statMax ) {
        cstatus = c_light_red;
    } else if( statCur == statMax ) {
        cstatus = c_white;
    } else if( statCur < statMax * 1.5 ) {
        cstatus = c_light_green;
    } else {
        cstatus = c_green;
    }
    std::string cur = colorize( string_format( _( "%2d" ), statCur ), cstatus );
    return string_format( _( "%s (%d)" ), cur, statMax );
}

std::string medical_ui::get_limb_effects( const bodypart_id &part ) const
{
    std::string description;

    const int bleed_intensity = you->get_effect_int( effect_bleed, part );
    const bool bleeding = bleed_intensity > 0;
    const bool bitten = you->has_effect( effect_bite, part.id() );
    const bool infected = you->has_effect( effect_infected, part.id() );

    // BLEEDING block
    if( bleeding ) {
        const effect bleed_effect = you->get_effect( effect_bleed, part );
        const nc_color bleeding_color = colorize_bleeding_intensity( bleed_intensity );
        description += string_format( "[ %s ] - %s\n",
                                      colorize( bleed_effect.get_speed_name(), bleeding_color ),
                                      string_format( bleed_effect.disp_short_desc(), body_part_name( bp.id() ) ) );
    }

    // BITTEN block
    if( bitten ) {
        const effect bite_effect = you->get_effect( effect_bite, part );
        description += string_format( "[ %s ] - %s\n",
                                      colorize( bite_effect.get_speed_name(), c_yellow ),
                                      string_format( bite_effect.disp_short_desc(), body_part_name( bp.id() ) ) );
    }

    // INFECTED block
    if( infected ) {
        const effect infected_effect = you->get_effect( effect_infected, part );
        description += string_format( "[ %s ] - %s\n",
                                      colorize( infected_effect.get_speed_name(), c_pink ),
                                      string_format( infected_effect.disp_short_desc(), body_part_name( bp.id() ) ) );
    }

    // rest of effects
    for( const effect &eff : you->get_effects_from_bp( part ) ) {
        if( eff.get_id() != effect_bleed &&
            eff.get_id() != effect_bite &&
            eff.get_id() != effect_infected ) {
            description += string_format( "[ %s ] - %s\n", colorize( eff.get_speed_name(), c_bold ),
                                          string_format( eff.disp_short_desc(), body_part_name( bp.id() ) ) );
        }
    }

    return description;
}

std::string medical_ui::get_limb_wounds( const bodypart_id &part ) const
{

    std::string detail_str;
    const bodypart *bp = you->get_part( part );

    // for existing wounds
    const std::vector<wound> &wds = bp->get_wounds();
    if( !wds.empty() ) {
        detail_str += _( "Current wounds" );
        detail_str += ":\n";
        for( const wound &wd : wds ) {
            if( !debug_mode ) {
                detail_str +=
                    string_format( "%s - %s\n", colorize( wd.type->get_name(), c_cyan ), wd.type->get_description() );
            } else {

                std::string limb_score_output = "limb scores applied:\n";
                for( const wound_limb_score &s : wd.type->get_limb_scores() ) {
                    limb_score_output += string_format( "%s: %.3f\n", s.score.str(), s.value );
                }

                detail_str +=
                    string_format( "wound: %s - %s\nhealing time %s\nhealing percentage %.3f%%\ngives pain: %d\n%s\n",
                                   colorize( wd.type->get_name(), c_cyan ),
                                   wd.type->get_description(),
                                   to_string( wd.get_healing_time() ),
                                   wd.healing_percentage(),
                                   wd.get_pain(),
                                   limb_score_output
                                 );
            }
        }
    }
    if( debug_mode ) {
        detail_str += "All potential wounds:\n";
        for( const std::pair<bp_wounds, int> &wd : bp->get_id()->potential_wounds ) {
            std::string damage_types;

            for( const damage_type_id dt : wd.first.damage_type ) {
                damage_types += dt.c_str();
                damage_types += ", ";
            }
            damage_types.pop_back();
            damage_types.pop_back();
            detail_str +=
                string_format( "%s:\nweight: %d\ndamage types required: %s\ndamage required: [ %d - %d ]\n",
                               colorize( wd.first.id.c_str(), c_yellow ), wd.second, damage_types, wd.first.damage_required.first,
                               wd.first.damage_required.second );
        }
    }

    return detail_str;
}

std::string medical_ui::get_limb_scores_and_modifiers( const bodypart_id &part ) const
{
    const bodypart *bp = you->get_part( part );
    std::string detail_str;
    for( const limb_score &sc : limb_score::get_all() ) {
        if( !part->has_limb_score( sc.getId() ) ) {
            continue;
        }

        float injury_score = bp->get_limb_score( *you, sc.getId() );
        float max_score = part->get_limb_score( sc.getId() );

        if( injury_score < max_score ) {
            std::pair<std::string, nc_color> score_c;
            if( injury_score < max_score * 0.4f ) {
                score_c.first = _( "Crippled" );
                score_c.second = c_red;
            } else if( injury_score < max_score * 0.6f ) {
                score_c.first = _( "Impaired" );
                score_c.second = c_light_red;
            } else if( injury_score < max_score * 0.75f ) {
                score_c.first = _( "Weakened" );
                score_c.second = c_yellow;
            } else if( injury_score < max_score * 0.9f ) {
                score_c.first = _( "Weakened" );
                score_c.second = c_dark_gray;
            } else {
                score_c.first = _( "OK" );
                score_c.second = c_dark_gray;
            }
            detail_str += string_format( _( "%s: %s\n" ), sc.name().translated(), colorize( score_c.first,
                                         score_c.second ) );
        } else {
            detail_str += string_format( _( "%s: %s\n" ), sc.name().translated(), colorize( "OK", c_green ) );
        }
    }

    return detail_str;
}

static std::string hp_feeling( const float cur_hp_pcnt )
{
    if( cur_hp_pcnt < 0.125f ) {
        return colorize( _( "Very Bad" ), c_red );
    } else if( cur_hp_pcnt < 0.375f ) {
        return colorize( _( "Bad" ), c_light_red );
    } else if( cur_hp_pcnt < 0.625f ) {
        return colorize( _( "So-so" ), c_yellow );
    } else if( cur_hp_pcnt < 0.875f ) {
        return colorize( _( "Okay" ), c_light_green );
    } else {
        return colorize( _( "Good" ), c_green );
    }
}

std::string medical_ui::get_modified_limb_name( const bodypart_id &part ) const
{
    std::string header; // Bodypart Title
    std::string hp_str; // Bodypart HP, later merged into the header
    std::string detail; // additional info, like [ BLEEDING ]

    const int bleed_intensity = you->get_effect_int( effect_bleed, part );
    const bool bleeding = bleed_intensity > 0;
    const bool bitten = you->has_effect( effect_bite, part.id() );
    const bool infected = you->has_effect( effect_infected, part.id() );
    const bool no_feeling = you->has_flag( json_flag_PAIN_IMMUNE );
    const int maximal_hp = you->get_part_hp_max( part );
    const int current_hp = you->get_part_hp_cur( part );
    const bool limb_is_broken = you->is_limb_broken( part );
    const bool limb_is_mending = you->worn_with_flag( flag_SPLINT, part );

    if( limb_is_mending ) {
        detail += string_format( _( "[ %s ]" ), colorize( _( "SPLINTED" ), c_yellow ) );
        if( no_feeling ) {
            hp_str = colorize( "==%==", c_blue );
        } else {
            const effect &eff = you->get_effect( effect_mending, part );
            const int mend_perc = eff.is_null() ? 0.0 : 100 * eff.get_duration() / eff.get_max_duration();

            const int num = mend_perc / 20;
            hp_str = colorize( std::string( num, '#' ) + std::string( 5 - num, '=' ), c_blue );
        }
    } else if( limb_is_broken ) {
        detail += string_format( _( "[ %s ]" ), colorize( _( "BROKEN" ), c_red ) );
        hp_str = "==%==";
    } else if( no_feeling ) {
        const float cur_hp_pcnt = current_hp / static_cast<float>( maximal_hp );
        hp_str = hp_feeling( cur_hp_pcnt );
    } else {
        std::pair<std::string, nc_color> h_bar = get_hp_bar( current_hp, maximal_hp, false );
        hp_str = colorize( h_bar.first, h_bar.second ) +
                 colorize( std::string( 5 - utf8_width( h_bar.first ), '.' ), c_white );
    }
    const std::string bp_name = uppercase_first_letter( body_part_name( part, 1 ) );
    header += colorize( bp_name,
                        display::limb_color( *you,
                                part, true, true, true ) ) + " " + hp_str;

    // BLEEDING block
    if( bleeding ) {
        const nc_color bleeding_color = colorize_bleeding_intensity( bleed_intensity );
        detail += string_format( _( "[ %s ]" ), colorize( _( "BLEEDING" ), bleeding_color ) );
    }

    // BITTEN block
    if( bitten ) {
        detail += string_format( _( "[ %s ]" ), colorize( _( "BITTEN" ), c_yellow ) );
    }

    // INFECTED block
    if( infected ) {
        detail += string_format( _( "[ %s ]" ), colorize( _( "SEPTIC" ), c_pink ) );
    }

    return string_format( "%s %s", header, detail );

}

static std::string draw_speed_summary( Character &you )
{
    std::string desc;

    const int runcost = you.run_cost( 100 );
    const int newmoves = you.get_speed();

    const std::string runcost_colored = colorize( string_format( _( "%d" ), runcost ),
                                        ( runcost <= 100 ? c_green : c_red ) );
    desc += string_format( _( "Base Move Cost: %s" ), runcost_colored );
    desc += "\n";

    const std::string speed_colored = colorize( string_format( _( "%d" ), newmoves ),
                                      ( newmoves >= 100 ? c_green : c_red ) );
    desc += string_format( _( "Current Speed: %s" ), speed_colored );
    desc += "\n";

    const int speed_modifier = you.get_enchantment_speed_bonus();

    std::string pge_str;
    if( speed_modifier != 0 ) {
        pge_str = pgettext( "speed bonus", "Bio/Mut/Effects" );
        desc += colorize( string_format( _( "%s: -%2d%%\n" ), pge_str, speed_modifier ),
                          c_green );
    }

    int pen = 0;
    if( you.weight_carried() > you.weight_capacity() ) {
        pen = 25 * ( you.weight_carried() - you.weight_capacity() ) / you.weight_capacity();
        pge_str = pgettext( "speed penalty", "Overburdened" );
        desc += colorize( string_format( _( "%s: -%2d%%" ), pge_str, pen ), c_red );
        desc += "\n";
    }

    pen = you.ppen_spd;
    if( pen >= 1 ) {
        pge_str = pgettext( "speed penalty", "Pain" );
        desc += colorize( string_format( _( "%s: -%2d%%" ), pge_str, pen ), c_red );
        desc += "\n";
    }
    if( you.get_thirst() > 40 ) {
        pen = std::abs( Character::thirst_speed_penalty( you.get_thirst() ) );
        pge_str = pgettext( "speed penalty", "Thirst" );
        desc += colorize( string_format( _( "%s: -%2d%%" ), pge_str, pen ), c_red );
        desc += "\n";
    }
    if( you.kcal_speed_penalty() < 0 ) {
        pen = std::abs( you.kcal_speed_penalty() );
        pge_str = pgettext( "speed penalty", you.get_bmi() < character_weight_category::underweight ?
                            "Starving" : "Underfed" );
        desc += colorize( string_format( _( "%s: -%2d%%" ), pge_str, pen ), c_red );
        desc += "\n";
    }
    if( you.has_trait( trait_SUNLIGHT_DEPENDENT ) && !g->is_in_sunlight( you.pos_bub() ) ) {
        pen = ( g->light_level( you.posz() ) >= 12 ? 5 : 10 );
        pge_str = pgettext( "speed penalty", "Out of Sunlight" );
        desc += colorize( string_format( _( "%s: -%2d%%" ), pge_str, pen ), c_red );
        desc += "\n";
    }

    int speed_effects = 0;
    for( const effect &elem : you.get_effects() ) {
        bool reduced = you.resists_effect( elem );
        int move_adjust = elem.get_mod( "SPEED", reduced );
        if( move_adjust != 0 ) {
            speed_effects += move_adjust;
        }
    }

    if( speed_effects > 0 ) {
        nc_color col = speed_effects > 0 ? c_green : c_red;
        desc += colorize( string_format( _( "Effects: %s%d%%" ), ( speed_effects > 0 ? "+" : "-" ),
                                         std::abs( speed_effects ) ), col );
    }

    return desc;
}

static std::string draw_stats_summary( Character &you )
{

    std::string desc;

    std::string strength_str = coloured_stat_display( you.get_str(), you.get_str_base() );
    desc += string_format( _( "Strength: %s" ), strength_str );
    desc += "\n";

    std::string dexterity_str = coloured_stat_display( you.get_dex(), you.get_dex_base() );
    desc += string_format( _( "Dexterity: %s" ), dexterity_str );
    desc += "\n";

    std::string intelligence_str = coloured_stat_display( you.get_int(), you.get_int_base() );
    desc += string_format( _( "Intelligence: %s" ), intelligence_str );
    desc += "\n";

    std::string perception_str = coloured_stat_display( you.get_per(), you.get_per_base() );
    desc += string_format( _( "Perception: %s" ), perception_str );

    return desc;
}

std::vector<bodypart_id> medical_ui::body_parts()
{
    const std::vector<bodypart_id> bodyparts = you->get_all_body_parts( get_body_part_flags::sorted );
    if( !bodyparts.empty() && std::find( bodyparts.begin(), bodyparts.end(), bp ) == bodyparts.end() ) {
        bp = bodyparts.front();
    }
    return bodyparts;
}

std::string medical_ui::limbs_list_rml( const std::vector<bodypart_id> &bodyparts ) const
{
    std::string rml;
    for( size_t i = 0; i < bodyparts.size(); i++ ) {
        const bodypart_id &part = bodyparts[i];
        rml += string_format( "<div id=\"limb-%d\" class=\"item%s\" data-row=\"%d\">", i,
                              bp == part ? " selected" : "", i ) +
               rml_ui::markup_to_rml( get_modified_limb_name( part ) ) + "</div>";
    }
    return rml;
}

std::string medical_ui::summary_tab_rml() const
{
    std::string rml = "<div class=\"pre\">" + rml_ui::markup_to_rml( draw_speed_summary( *you ) ) +
                      "</div><div class=\"sep\"></div><div class=\"pre\">" +
                      rml_ui::markup_to_rml( draw_stats_summary( *you ) ) + "</div>";

    const diag_value *last_weighting_time = you->maybe_get_value( "last_weighting_time" );
    if( last_weighting_time != nullptr ) {
        const std::string last_weighting_weight = string_format( "%.0f %s",
                convert_weight( units::from_kilogram( you->get_value( "last_weighting_weight_kg" ).dbl() ) ),
                weight_units() );
        const std::string last_weighted_time = string_format( _( "last weighed %s ago." ),
                                               to_string_approx( calendar::turn - time_point( last_weighting_time->dbl() ) ) );
        const std::string weight_desc = string_format(
                                            "%s: %s (%s)",
                                            _( "Weight" ),
                                            colorize( last_weighting_weight, c_yellow ),
                                            colorize( last_weighted_time, c_dark_gray ) );
        rml += "<p>" + rml_ui::markup_to_rml( weight_desc ) + "</p>";
    } else {
        rml += "<p>" + rml_ui::colored_rml( _( "You do not remember the last time you weighed yourself." ),
                                            c_dark_gray ) + "</p>";
    }

    if( debug_mode ) {
        rml += "<div class=\"sep\"></div>" + rml_ui::colored_rml( "Debug:", c_red );
        std::string txt;
        txt += string_format( "bodyweight: %.2f kg\n", units::to_kilogram( you->bodyweight() ) );
        txt += string_format( "inventory: %.2f kg\n", units::to_kilogram( you->inv->weight() ) );
        const units::mass wornWeight = you->worn.weight();
        txt += string_format( "worn: %.2f kg\n", units::to_kilogram( wornWeight ) );

        const item_location &wield = you->get_wielded_item();
        if( wield ) {
            txt += string_format( "wielded: %.2f kg\n", units::to_kilogram( wield->weight() ) );
        }
        txt += string_format( "bionic: %.2f kg\n", units::to_kilogram( you->bionics_weight() ) );

        rml += "<div class=\"pre\">" + rml_ui::markup_to_rml( txt ) + "</div>";
    }
    return rml;
}

std::string medical_ui::limb_tab_rml( const std::vector<bodypart_id> &bodyparts ) const
{
    std::string info;
    const std::string limb_effects = get_limb_effects( bp );
    if( !limb_effects.empty() ) {
        info += "<div class=\"pre\">" + rml_ui::markup_to_rml( limb_effects ) +
                "</div><div class=\"sep\"></div>";
    }
    const std::string limb_wounds = get_limb_wounds( bp );
    if( !limb_wounds.empty() ) {
        info += "<div class=\"pre\">" + rml_ui::markup_to_rml( limb_wounds ) +
                "</div><div class=\"sep\"></div>";
    }
    const std::string limb_scores = get_limb_scores_and_modifiers( bp );
    if( !limb_scores.empty() ) {
        info += "<div class=\"pre\">" + rml_ui::markup_to_rml( limb_scores ) + "</div>";
    }

    return "<div class=\"panes\"><div class=\"pane\" style=\"flex: 0 0 35%;\"><h3>" +
           rml_ui::escape_rml( _( "Limbs" ) ) + "</h3>" + limbs_list_rml( bodyparts ) +
           "</div><div id=\"limb-info\" class=\"pane\"><h3>" +
           rml_ui::escape_rml( uppercase_first_letter( body_part_name( bp, 1 ) ) ) + "</h3>" + info +
           "</div></div>";
}

std::string medical_ui::hint_rml() const
{
    const std::string desc = string_format(
                                 _( "[<color_yellow>%s</color>] Use item [<color_yellow>%s</color>] Treat, [<color_yellow>%s</color>] Keybindings" ),
                                 ctxt.get_desc( "APPLY" ), ctxt.get_desc( "CONFIRM" ), ctxt.get_desc( "HELP_KEYBINDINGS" ) );
    return rml_ui::markup_to_rml( desc );
}

void medical_ui::on_redraw()
{
    // At least half the screen, as the old window was
    set_size( "70%", "80%" );
    const std::vector<bodypart_id> bodyparts = body_parts();

    set_header( hint_rml() );
    set_tabs( { _( "Limbs" ), _( "Summary" ) }, static_cast<size_t>( selected_tab ) );
    if( selected_tab == medical_tab_mode::TAB_LIMBS ) {
        set_body_scrolls( false );
        set_body( limb_tab_rml( bodyparts ) );
        if( shown_tab != selected_tab || shown_bp != bp ) {
            const auto iter = std::find( bodyparts.begin(), bodyparts.end(), bp );
            scroll_into_view( "limb-" + std::to_string( std::distance( bodyparts.begin(), iter ) ) );
            scroll( "limb-info", rml_ui::scroll::top );
            shown_bp = bp;
        }
    } else {
        set_body_scrolls( true );
        set_body( summary_tab_rml() );
        if( shown_tab != selected_tab ) {
            scroll_body( rml_ui::scroll::top );
        }
    }
    shown_tab = selected_tab;
}

namespace
{
struct healing_option {
    wound_type_id wd;
    wound_fix_id fix;
    bool doable;
    time_duration time_to_apply;
};
} // namespace

bool Character::pick_wound_fix( const bodypart_id &bp_id )
{
    std::vector<healing_option> healing_options;
    const inventory &inv = crafting_inventory();

    bodypart *bp = get_part( bp_id );
    const std::vector<wound> wounds = bp->get_wounds();

    if( debug_mode || has_trait( trait_DEBUG_HS ) ) {
        if( query_yn( "Open debug insertion and removing wounds?" ) ) {
            uilist menu;
            do {
                menu.reset();
                menu.text = "Toggle which wound?";
                std::vector<std::pair<wound_type_id, bool>> opts;
                for( const auto& [wound, weight] : bp_id->potential_wounds ) {
                    opts.emplace_back( wound.id, bp->has_wound( wound.id ) );
                    menu.addentry( -1, true, -1, string_format( opts.back().second ? "Treat: %s" : "Set: %s",
                                   wound.id.str() ) );
                }
                if( opts.empty() ) {
                    popup( string_format( "The %s doesn't have any wounds to toggle.", bp_id->name ) );
                    return false;
                }
                menu.query();
                if( menu.ret >= 0 ) {
                    if( opts[menu.ret].second ) {
                        bp->remove_wound( opts[menu.ret].first );
                    } else {
                        bp->add_wound( opts[menu.ret].first );
                    }
                }
            } while( menu.ret >= 0 );
            return false;
        }
    }

    for( const wound &wd : wounds ) {
        for( const wound_fix_id &fix_id : wd.type->fixes ) {
            healing_option opt{wd.type, fix_id, true, fix_id->time};

            for( const auto &[skill_id, level] : fix_id->skills ) {
                if( get_greater_skill_or_knowledge_level( skill_id ) < level ) {
                    opt.doable = false;
                    break;
                }
            }

            for( const wound_proficiency &w_prof : fix_id->proficiencies ) {

                const bool has_prof = has_proficiency( w_prof.prof );

                if( w_prof.is_mandatory && !has_prof ) {
                    opt.doable = false;
                }

                if( has_prof ) {
                    opt.time_to_apply *= w_prof.time_save;
                }
            }

            opt.doable &= fix_id->get_requirements().can_make_with_inventory( inv, is_crafting_component );

            healing_options.emplace_back( opt );
        }
    }

    if( healing_options.empty() ) {
        if( bp->get_wounds().empty() ) {
            if( bp_id->has_flag( json_flag_BIONIC_LIMB ) ) {
                // should be popup, but popup() crashes this message, for some reason
                popup( string_format( _( "The %s doesn't have any damage to repair." ), body_part_name( bp_id ) ) );
                return false;
            } else {
                popup( string_format( _( "The %s doesn't have any wounds to treat." ), body_part_name( bp_id ) ) );
                return false;
            }
        } else {
            if( bp_id->has_flag( json_flag_BIONIC_LIMB ) ) {
                popup( string_format( _( "The %s damage cannot be repaired." ), body_part_name( bp_id ) ) );
                return false;
            } else {
                popup( string_format( _( "The %s wounds cannot be treated." ), body_part_name( bp_id ) ) );
                return false;
            }
        }
    }

    uilist menu;
    menu.title = _( "Treat which wound?" );
    menu.desc_enabled = true;

    constexpr int fold_width = 80;

    for( healing_option &opt : healing_options ) {

        const wound_fix &fix = *opt.fix;
        const requirement_data &reqs = fix.get_requirements();
        const nc_color col = opt.doable ? c_white : c_light_gray;

        const std::vector<std::string> tools = reqs.get_folded_tools_list( fold_width, col, inv );
        const std::vector<std::string> comps = reqs.get_folded_components_list( fold_width, col, inv,
                                               is_crafting_component );

        std::string descr = word_rewrap( fix.get_description(), 80 ) + "\n\n";

        descr += string_format( _( "Time required: <color_cyan>%s</color>\n" ),
                                to_string_approx( opt.time_to_apply ) );

        if( !fix.skills.empty() ) {
            descr += string_format( _( "Skills: %s\n" ), enumerate_as_string(
            fix.skills.begin(), fix.skills.end(), [&]( const std::pair<skill_id, int> &sk ) {
                if( get_greater_skill_or_knowledge_level( sk.first ) >= sk.second ) {
                    return string_format( pgettext( "skill requirement",
                                                    //~ %1$s: skill name, %2$s: current skill level, %3$s: required skill level
                                                    "<color_cyan>%1$s</color> <color_green>(%2$d/%3$d)</color>" ),
                                          sk.first->name(), static_cast<int>( get_greater_skill_or_knowledge_level( sk.first ) ), sk.second );
                } else {
                    return string_format( pgettext( "skill requirement",
                                                    //~ %1$s: skill name, %2$s: current skill level, %3$s: required skill level
                                                    "<color_cyan>%1$s</color> <color_red>(%2$d/%3$d)</color>" ),
                                          sk.first->name(), static_cast<int>( get_greater_skill_or_knowledge_level( sk.first ) ), sk.second );
                }
            } ) );
        }


        if( !fix.proficiencies.empty() ) {
            descr += string_format( _( "Proficiencies: %s\n" ), enumerate_as_string(
            fix.proficiencies.begin(), fix.proficiencies.end(), [&]( const wound_proficiency & w_prof ) {
                nc_color col;
                if( has_proficiency( w_prof.prof ) ) {
                    col = c_green;
                } else if( w_prof.is_mandatory ) {
                    col = c_red;
                } else {
                    col = c_light_gray;
                }
                return string_format( colorize( "%1$s", col ), w_prof.prof->name() );
            } ) );
        }

        for( const std::string &line : tools ) {
            descr += line + "\n";
        }
        for( const std::string &line : comps ) {
            descr += line + "\n";
        }
        menu.addentry_desc( -1, true, -1, fix.get_name(), colorize( descr, col ) );

    }

    menu.query();

    if( menu.ret < 0 ) {
        return false;
    }

    const healing_option &opt = healing_options[menu.ret];
    if( !opt.doable ) {
        add_msg( m_info, _( "You are currently unable to treat the %s this way." ),
                 opt.wd->get_name() );
        return false;
    }

    assign_activity( fix_wound_activity_actor( opt.time_to_apply, bp_id, opt.wd, opt.fix ) );

    return true;
}

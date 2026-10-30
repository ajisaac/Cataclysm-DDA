#include "ui_extended_description.h"

#include "cata_utility.h"
#include "character.h"
#include "color.h"
#include "creature.h"
#include "creature_tracker.h"
#include "map.h"
#include "mapdata.h"
#include "point.h"
#include "rml_markup.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "viewer.h"
#include "vpart_position.h"

static const trait_id trait_ILLITERATE( "ILLITERATE" );

static description_target &operator++( description_target &c )
{
    c = static_cast<description_target>( static_cast<int>( c ) + 1 );
    if( c == description_target::num_targets ) {
        c = static_cast<description_target>( 0 );
    }
    return c;
}

static description_target &operator--( description_target &c )
{
    if( c == static_cast<description_target>( 0 ) ) {
        c = description_target::num_targets;
    }
    c = static_cast<description_target>( static_cast<int>( c ) - 1 );
    return c;
}

static const Creature *seen_critter( const tripoint_bub_ms &p )
{
    const map &here = get_map();

    const Creature *critter = get_creature_tracker().creature_at( p, true );
    if( critter != nullptr && get_player_view().sees( here, *critter ) ) {
        return critter;
    }

    return nullptr;
}

// The text on a sign or a wall here, after a separator, or nothing
static std::string writing_rml( const std::string &text, const char *const readable_fmt,
                                const char *const unreadable )
{
    if( text.empty() ) {
        return std::string();
    }
    return "<div class=\"sep\"></div><div class=\"pre\">" + rml_ui::markup_to_rml(
               get_player_character().has_trait( trait_ILLITERATE ) ? std::string( unreadable ) :
               string_format( readable_fmt, text ) ) + "</div>";
}

std::string extended_description_rml( const std::vector<std::string> &description )
{
    std::string rml;
    for( const std::string &s : description ) {
        if( s == "--" ) {
            rml += "<div class=\"sep\"></div>";
        } else {
            rml += "<div class=\"pre\">" + rml_ui::markup_to_rml( s, c_light_gray ) + "</div>";
        }
    }
    return rml;
}

extended_description_window::extended_description_window( tripoint_bub_ms &p ) :
    rml_ui::window( std::string() ),
    p( p )
{
    const map &here = get_map();

    ctxt = input_context( "EXTENDED_DESCRIPTION" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "CREATURE" );
    ctxt.register_action( "FURNITURE" );
    ctxt.register_action( "TERRAIN" );
    ctxt.register_action( "VEHICLE" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "UP" );
    ctxt.register_action( "DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    // For the tabs and the close button
    ctxt.register_action( "SELECT" );
    ctxt.set_timeout( 10 );

    const Creature *critter = seen_critter( p );
    if( critter ) {
        creature_description = critter->extended_description();
    }
    bool sees = get_player_character().sees( here, p );
    const furn_id &furn = here.furn( p );
    if( sees && furn ) {
        furniture_description = furn->extended_description();
    }
    const ter_id &ter = here.ter( p );
    if( sees && ter ) {
        terrain_description = ter->extended_description();
    }
    const optional_vpart_position &vp = here.veh_at( p );
    if( sees && vp ) {
        veh_app_description = vp.extended_description();
    }

    if( critter ) {
        cur_target = description_target::creature;
    } else if( here.has_furn( p ) ) {
        cur_target = description_target::furniture;
    } else if( here.veh_at( p ) ) {
        cur_target = description_target::vehicle;
    }
}

std::string extended_description_window::tab_rml() const
{
    switch( cur_target ) {
        case description_target::creature:
            return !creature_description.empty() ? extended_description_rml( creature_description ) :
                   rml_ui::colored_rml( _( "You do not see any creature here." ), c_light_gray );
        case description_target::furniture:
            return ( !furniture_description.empty() ?
                     extended_description_rml( furniture_description ) :
                     rml_ui::colored_rml( _( "You do not see any furniture here." ), c_light_gray ) ) +
                   // NOLINTNEXTLINE(cata-text-style): the question mark does not end a sentence
                   writing_rml( get_map().get_signage( p ), _( "Sign: %s" ), _( "Sign: ???" ) );
        case description_target::terrain:
            return ( !terrain_description.empty() ? extended_description_rml( terrain_description ) :
                     rml_ui::colored_rml( _( "You can't see the terrain here." ), c_light_gray ) ) +
                   writing_rml( get_map().graffiti_at( p ), _( "Inscription: %s" ),
                                // NOLINTNEXTLINE(cata-text-style): the question mark does not end a sentence
                                _( "Inscription: ???" ) );
        case description_target::vehicle:
            return !veh_app_description.empty() ? extended_description_rml( veh_app_description ) :
                   rml_ui::colored_rml( _( "You can't see vehicles or appliances here." ), c_light_gray );
        case description_target::num_targets:
            break;
    }
    return std::string();
}

void extended_description_window::on_redraw()
{
    // The whole screen, as the old full-size window had
    set_size( "100%", "100%" );
    set_tabs( {
        string_format( _( "[%s] Creature" ), ctxt.get_desc( "CREATURE" ) ),
        string_format( _( "[%s] Furniture" ), ctxt.get_desc( "FURNITURE" ) ),
        string_format( _( "[%s] Terrain" ), ctxt.get_desc( "TERRAIN" ) ),
        string_format( _( "[%s] Vehicle/Appliance" ), ctxt.get_desc( "VEHICLE" ) )
    }, static_cast<size_t>( cur_target ) );
    set_body( tab_rml() );
    if( shown_target != cur_target ) {
        shown_target = cur_target;
        scroll_body( rml_ui::scroll::top );
    }
}

void extended_description_window::show()
{
    while( true ) {
        ui_manager::redraw_invalidated();
        std::string action = ctxt.handle_input();
        if( action == "SELECT" && hovered_tab() >= 0 ) {
            cur_target = static_cast<description_target>( hovered_tab() );
            continue;
        }
        action = resolve_click( action );
        if( action == "NEXT_TAB" ) {
            ++cur_target;
        } else if( action == "PREV_TAB" ) {
            --cur_target;
        }  else if( action == "UP" ) {
            scroll_body( rml_ui::scroll::line_up );
        }  else if( action == "DOWN" ) {
            scroll_body( rml_ui::scroll::line_down );
        }  else if( action == "PAGE_UP" ) {
            scroll_body( rml_ui::scroll::page_up );
        }  else if( action == "PAGE_DOWN" ) {
            scroll_body( rml_ui::scroll::page_down );
        } else if( action == "CREATURE" ) {
            cur_target = description_target::creature;
        } else if( action == "FURNITURE" ) {
            cur_target = description_target::furniture;
        } else if( action == "TERRAIN" ) {
            cur_target = description_target::terrain;
        } else if( action == "VEHICLE" ) {
            cur_target = description_target::vehicle;
        } else if( action == "CONFIRM" || action == "QUIT" ) {
            return;
        }
    }
}

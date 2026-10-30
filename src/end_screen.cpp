#include <algorithm>
#include <memory>

#include "avatar.h"
#include "ascii_art.h"
#include "condition.h"
#include "dialogue.h"
#include "end_screen.h"
#include "event.h"
#include "event_bus.h"
#include "game.h"
#include "generic_factory.h"
#include "input_context.h"
#include "npc.h" // for parse_tags()! Why!
#include "rml_markup.h"
#include "string_formatter.h"
#include "talker.h"
#include "ui_manager.h"

static const ascii_art_id ascii_art_ascii_tombstone( "ascii_tombstone" );

namespace
{
generic_factory<end_screen> end_screen_factory( "end_screen" );
} // namespace

template<>
const end_screen &string_id<end_screen>::obj()const
{
    return end_screen_factory.obj( *this );
}

template<>
bool string_id<end_screen>::is_valid() const
{
    return end_screen_factory.is_valid( *this );
}

void end_screen::load_end_screen( const JsonObject &jo, const std::string &src )
{
    end_screen_factory.load( jo, src );
}

void end_screen::load( const JsonObject &jo, std::string_view )
{
    mandatory( jo, was_loaded, "id", id );
    mandatory( jo, was_loaded, "picture_id", picture_id );
    mandatory( jo, was_loaded, "priority", priority );
    read_condition( jo, "condition", condition, false );

    optional( jo, was_loaded, "added_info", added_info );
    optional( jo, was_loaded, "last_words_label", last_words_label );
}

const std::vector<end_screen> &end_screen::get_all()
{
    return end_screen_factory.get_all();
}

void end_screen_data::draw_end_screen_ui( bool actually_dead )
{
    input_context ctxt;
    ctxt.register_action( "TEXT.CONFIRM" );
    // For the close button
    ctxt.register_action( "SELECT" );
    ctxt.set_timeout( 50 );
    end_screen_ui_impl p_impl;

    while( true ) {
        ui_manager::redraw_invalidated();
        const std::string action = p_impl.resolve_click( ctxt.handle_input() );
        if( action == "TEXT.CONFIRM" || action == "QUIT" ) {
            break;
        }
    }
    if( actually_dead ) {
        avatar &u = get_avatar();
        const bool is_suicide = g->uquit == QUIT_SUICIDE;
        get_event_bus().send<event_type::game_avatar_death>( u.getID(), u.name, is_suicide,
                p_impl.last_words() );
    }
}

end_screen_ui_impl::end_screen_ui_impl() :
    rml_ui::window( _( "The End" ), "end_screen.rml" )
{
    avatar &u = get_avatar();
    ascii_art_id art = ascii_art_ascii_tombstone;
    dialogue d( get_talker_for( u ), nullptr );
    std::string input_label;
    std::vector<std::pair<std::pair<int, int>, std::string>> added_info;

    //Sort end_screens in order of decreasing priority
    std::vector<end_screen> sorted_screens = end_screen::get_all();
    std::sort( sorted_screens.begin(), sorted_screens.end(), []( end_screen const & a,
    end_screen const & b ) {
        return a.priority > b.priority;
    } );

    for( const end_screen &e_screen : sorted_screens ) {
        if( e_screen.condition( d ) ) {
            art = e_screen.picture_id;
            if( !e_screen.added_info.empty() ) {
                added_info = e_screen.added_info;
            }
            if( !e_screen.last_words_label.empty() ) {
                input_label = e_screen.last_words_label;
            }
            break;
        }
    }

    // The picture, with the added info laid over it at the column and row
    // each asks for; see .art in end_screen.rml
    std::string rml;
    if( art.is_valid() ) {
        rml += "<div class=\"art\">";
        for( const std::string &line : art->picture ) {
            rml += "<div class=\"art-line\">" + rml_ui::markup_to_rml( line ) + "</div>";
        }
        for( const std::pair<std::pair<int, int>, std::string> &info : added_info ) {
            std::string translated_info = _( info.second );
            parse_tags( translated_info, u, u );
            rml += string_format( "<div class=\"art-info\" style=\"left: %.1fem; top: %dem;\">",
                                  info.first.first * 0.5f, info.first.second - 1 ) +
                   rml_ui::markup_to_rml( translated_info ) + "</div>";
        }
        rml += "</div>";
    }
    set_body( rml );

    if( !input_label.empty() ) {
        set_footer( rml_ui::markup_to_rml( _( input_label ) ) );
        set_class( "last-words-row", "hidden", false );
        focus( "last-words" );
    }
}

std::string end_screen_ui_impl::last_words() const
{
    return get_value( "last-words" ).value_or( std::string() );
}

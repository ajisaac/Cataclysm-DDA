#include "ui_iteminfo.h"

#include <string>
#include <vector>

#include "color.h"
#include "input_enums.h"
#include "rml_markup.h"
#include "translations.h"
#include "ui_manager.h"


iteminfo_window::iteminfo_window( item_info_data &info, point pos, int width, int height ) :
    rml_ui::window( _( "Item info" ) ),
    data( info ), pos( pos ), width( width ), height( height )
{
    ctxt = input_context( "default" );
    if( info.handle_scrolling ) {
        ctxt.register_action( "PAGE_UP" );
        ctxt.register_action( "PAGE_DOWN" );
        ctxt.register_action( "HOME" );
        ctxt.register_action( "END" );
        if( info.arrow_scrolling ) {
            ctxt.register_action( "UP" );
            ctxt.register_action( "DOWN" );
        }
    }
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    // For the close button
    ctxt.register_action( "SELECT" );
    if( info.any_input ) {
        ctxt.register_action( "ANY_INPUT" );
    }

    ctxt.set_timeout( 10 );

    // The info does not change while the window is up
    std::string rml;
    if( !data.get_item_name().empty() ) {
        rml += "<p>" + rml_ui::markup_to_rml( data.get_item_name(), c_light_gray ) + "</p>";
    }
    if( !data.get_type_name().empty() &&
        data.get_item_name().find( data.get_type_name() ) == std::string::npos ) {
        rml += "<p>" + rml_ui::markup_to_rml( data.get_type_name(), c_light_gray ) + "</p>";
    }
    rml += "<div class=\"pre\">" + item_info_rml( data.get_item_display(),
            data.get_item_compare() ) + "</div>";
    set_body( rml );
}

void iteminfo_window::on_redraw()
{
    set_size( rml_ui::term_width( width ),
              height > 0 ? rml_ui::term_height( height ) : std::string() );
    set_position( pos.x < 0 ? std::string() : rml_ui::term_width( pos.x ),
                  pos.y < 0 ? std::string() : rml_ui::term_height( pos.y ) );
}

void iteminfo_window::execute()
{
    if( data.without_getch ) {
        ui_manager::redraw();
        return;
    }

    while( true ) {
        ui_manager::redraw();
        const std::string action = resolve_click( ctxt.handle_input() );

        if( data.handle_scrolling && data.arrow_scrolling && action == "UP" ) {
            scroll_body( rml_ui::scroll::line_up );
        } else if( data.handle_scrolling && data.arrow_scrolling && action == "DOWN" ) {
            scroll_body( rml_ui::scroll::line_down );
        } else if( data.handle_scrolling && action == "PAGE_UP" ) {
            scroll_body( rml_ui::scroll::page_up );
        } else if( data.handle_scrolling && action == "PAGE_DOWN" ) {
            scroll_body( rml_ui::scroll::page_down );
        } else if( data.handle_scrolling && action == "HOME" ) {
            scroll_body( rml_ui::scroll::top );
        } else if( data.handle_scrolling && action == "END" ) {
            scroll_body( rml_ui::scroll::bottom );
        } else if( action == "CONFIRM" || action == "QUIT" ||
                   ( data.any_input && action == "ANY_INPUT" && !ctxt.get_raw_input().sequence.empty() ) ) {
            break;
        }
    }
}

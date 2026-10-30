#include "rml_window.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include <RmlUi/Core.h>

#include "input_context.h"
#include "output.h"
#include "rml_markup.h"
#include "string_formatter.h"

// Roughly the width of a digit, and the height of a line, in the window's
// font, to turn the character counts the game thinks in into RCSS lengths.
static constexpr float char_width_em = 0.56f;
static constexpr float line_height_em = 1.3f;

namespace rml_ui
{

window::window( const std::string &title, const std::string &file ) :
    document( file ),
    close_action( "QUIT" )
{
    set_title( title );
}

void window::set_title( const std::string &markup )
{
    show( "title", markup_to_rml( markup ) );
}

void window::set_size( const std::string &width, const std::string &height )
{
    size_width = width;
    size_height = height;
    apply_style();
}

void window::set_position( const std::string &left, const std::string &top )
{
    pos_left = left;
    pos_top = top;
    apply_style();
}

void window::apply_style()
{
    std::string window_style;
    std::string root_style;
    if( !size_width.empty() ) {
        window_style += "width: " + size_width + ";";
    }
    if( !size_height.empty() ) {
        window_style += "height: " + size_height + ";";
    }
    // #root centers #window; a position aligns it to the start instead
    if( !pos_left.empty() ) {
        root_style += "justify-content: flex-start;";
        window_style += "margin-left: " + pos_left + ";";
    }
    if( !pos_top.empty() ) {
        root_style += "align-items: flex-start;";
        window_style += "margin-top: " + pos_top + ";";
    }
    if( window_style != applied_window_style ) {
        applied_window_style = window_style;
        set_attribute( "window", "style", window_style );
    }
    if( root_style != applied_root_style ) {
        applied_root_style = root_style;
        set_attribute( "root", "style", root_style );
    }
}

void window::set_tabs( const std::vector<std::string> &names, size_t active )
{
    std::string rml;
    for( size_t i = 0; i < names.size(); ++i ) {
        rml += "<div class=\"tab" + std::string( i == active ? " active" : "" ) +
               "\" data-tab=\"" + std::to_string( i ) + "\">" + markup_to_rml( names[i] ) + "</div>";
    }
    set_class( "tabs", "hidden", names.empty() );
    show( "tabs", rml );
}

void window::set_header( const std::string &rml )
{
    set_class( "header", "hidden", rml.empty() );
    show( "header", rml );
}

void window::set_body( const std::string &rml )
{
    show( "body", rml );
}

void window::set_footer( const std::string &rml )
{
    set_class( "footer", "hidden", rml.empty() );
    show( "footer", rml );
}

void window::set_body_scrolls( bool scrolls )
{
    set_class( "body", "fill", !scrolls );
}

void window::set_close_action( const std::string &action )
{
    if( action == close_action ) {
        return;
    }
    close_action = action;
    set_class( "close", "hidden", action.empty() );
    set_attribute( "close", "data-action", action );
}

// Where each element below this one that has an id and is scrolled down is
// scrolled to
static void collect_scrolls( Rml::Element *element,
                             std::vector<std::pair<std::string, float>> &scrolls )
{
    for( int i = 0; i < element->GetNumChildren(); ++i ) {
        Rml::Element *child = element->GetChild( i );
        if( !child->GetId().empty() && child->GetScrollTop() > 0.f ) {
            scrolls.emplace_back( child->GetId(), child->GetScrollTop() );
        }
        collect_scrolls( child, scrolls );
    }
}

bool window::show( const std::string &id, const std::string &rml )
{
    const auto iter = shown.find( id );
    if( iter != shown.end() && iter->second == rml ) {
        return false;
    }
    shown[id] = rml;
    Rml::Element *element = get() ? get()->GetElementById( id ) : nullptr;
    if( !element ) {
        return true;
    }
    // New content would start every pane in it back at the top, so keep where
    // each one with an id was scrolled to, for panes rebuilt as a selection
    // changes. A screen can still scroll one elsewhere afterwards.
    std::vector<std::pair<std::string, float>> scrolls;
    collect_scrolls( element, scrolls );
    element->SetInnerRML( rml );
    if( !scrolls.empty() ) {
        get()->UpdateDocument();
        for( const std::pair<std::string, float> &scroll : scrolls ) {
            if( Rml::Element *pane = get()->GetElementById( scroll.first ) ) {
                pane->SetScrollTop( scroll.second );
            }
        }
    }
    return true;
}

std::string window::resolve_click( const std::string &action ) const
{
    if( action != "SELECT" ) {
        return action;
    }
    const std::string clicked = hovered_attribute( "data-action" );
    return clicked.empty() ? action : clicked;
}

int window::hovered_index( const std::string &attribute ) const
{
    const std::string value = hovered_attribute( attribute );
    if( value.empty() ) {
        return -1;
    }
    char *end = nullptr;
    const long index = std::strtol( value.c_str(), &end, 10 );
    return end != value.c_str() && index >= 0 ? static_cast<int>( index ) : -1;
}

int window::hovered_tab() const
{
    return hovered_index( "data-tab" );
}

void window::scroll_body( rml_ui::scroll how )
{
    scroll( "body", how );
}

void window::set_filter_enabled( bool enabled )
{
    Rml::Element *element = get() ? get()->GetElementById( "filter" ) : nullptr;
    if( auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>( element ) ) {
        control->SetDisabled( !enabled );
    }
}

void window::show_filter( const std::string &text )
{
    set_class( "filter-row", "hidden", false );
    if( filter_text() != text ) {
        set_value( "filter", text );
    }
}

void window::start_filter( const std::string &text )
{
    show_filter( text );
    set_filter_enabled( true );
    focus( "filter" );
}

void window::stop_filter()
{
    blur( "filter" );
    set_filter_enabled( false );
}

void window::hide_filter()
{
    stop_filter();
    set_class( "filter-row", "hidden", true );
}

std::string window::filter_text() const
{
    return get_value( "filter" ).value_or( std::string() );
}

std::string keybinding_markup( const input_context &ctxt, const std::string &action,
                               const std::string &description, bool active,
                               const nc_color &default_color )
{
    const nc_color color = active ? ACTIVE_HOTKEY_COLOR : default_color;
    const std::string key = action.size() <= 1 ? action : ctxt.get_desc( action );
    return colorize( "[", default_color ) + colorize( key, color ) +
           colorize( "] ", default_color ) + colorize( description, color );
}

std::string button_rml( const std::string &action, const std::string &label_rml, bool enabled )
{
    if( !enabled ) {
        return "<div class=\"button disabled\">" + label_rml + "</div>";
    }
    return "<div class=\"button\" data-action=\"" + escape_rml( action ) + "\">" + label_rml +
           "</div>";
}

std::string action_button_rml( const input_context &ctxt, const std::string &action,
                               const std::string &label, bool enabled )
{
    const std::string text = label.empty() ? ctxt.get_button_text( action ) :
                             ctxt.get_button_text( action, label );
    return button_rml( action, escape_rml( text ), enabled );
}

std::string colored_rml( const std::string &text, const nc_color &color )
{
    return "<span class=\"" + color_to_rml_classes( color ) + "\">" + escape_rml( text ) +
           "</span>";
}

std::string term_width( float columns )
{
    return string_format( "%.2fvw", 100.f * columns / std::max( 1, TERMX ) );
}

std::string term_height( float rows )
{
    return string_format( "%.2fvh", 100.f * rows / std::max( 1, TERMY ) );
}

std::string chars_width( int chars )
{
    return string_format( "%.1fem", chars * char_width_em );
}

std::string lines_height( int lines )
{
    return string_format( "%.1fem", lines * line_height_em );
}

int lines_fitting( float height_fraction, float reserved_dp )
{
    const float height_dp = viewport_px().y * height_fraction / dp_ratio();
    return std::max( 1, static_cast<int>( ( height_dp - reserved_dp ) / line_dp ) );
}

} // namespace rml_ui

#include "cata_catch.h"

#include <string>

#include "color.h"
#include "rml_markup.h"

using rml_ui::markup_to_rml;

TEST_CASE( "rml_markup_passes_plain_text_through", "[rml]" )
{
    CHECK( markup_to_rml( "" ).empty() );
    CHECK( markup_to_rml( "Plain text, 100%." ) == "Plain text, 100%." );
}

TEST_CASE( "rml_markup_escapes_rml_syntax", "[rml]" )
{
    CHECK( markup_to_rml( "salt & pepper" ) == "salt &amp; pepper" );
    CHECK( markup_to_rml( "1 < 2 > 0" ) == "1 &lt; 2 &gt; 0" );
    CHECK( markup_to_rml( "line one\nline two" ) == "line one<br/>line two" );
}

TEST_CASE( "rml_markup_prints_unknown_tags_literally", "[rml]" )
{
    // Hotkey markup is the caller's to resolve, as with print_colored_text().
    CHECK( markup_to_rml( "<M|m>OTD" ) == "&lt;M|m&gt;OTD" );
    // An unknown color is not a color tag, so its </color> has nothing to close.
    CHECK( markup_to_rml( "<color_not_a_color>x</color>", report_color_error::no ) ==
           "&lt;color_not_a_color&gt;x" );
}

TEST_CASE( "rml_markup_turns_color_tags_into_spans", "[rml]" )
{
    CHECK( markup_to_rml( "<color_light_red>hot</color> cold" ) ==
           "<span class=\"fg-lred\">hot</span> cold" );
    CHECK( markup_to_rml( "<color_c_green>ok</color>" ) == "<span class=\"fg-green\">ok</span>" );

    SECTION( "nesting follows the catacurses color stack" ) {
        CHECK( markup_to_rml( "<color_red>a<color_green>b</color>c</color>" ) ==
               "<span class=\"fg-red\">a<span class=\"fg-green\">b</span>c</span>" );
    }
    SECTION( "unbalanced tags are repaired" ) {
        CHECK( markup_to_rml( "a</color>b" ) == "ab" );
        CHECK( markup_to_rml( "<color_red>a" ) == "<span class=\"fg-red\">a</span>" );
    }
    SECTION( "shorthand tags are expanded" ) {
        CHECK( markup_to_rml( "<good>fine</good>" ) == "<span class=\"fg-green\">fine</span>" );
        CHECK( markup_to_rml( "<bad>worse</bad>" ) == "<span class=\"fg-red\">worse</span>" );
    }
    SECTION( "a base color wraps everything" ) {
        CHECK( markup_to_rml( "a <color_green>b</color>", c_red ) ==
               "<span class=\"fg-red\">a <span class=\"fg-green\">b</span></span>" );
    }
    SECTION( "text inside tags is still escaped" ) {
        CHECK( markup_to_rml( "<color_red>a & b</color>" ) ==
               "<span class=\"fg-red\">a &amp; b</span>" );
    }
}

TEST_CASE( "rml_escape_keeps_text_as_written", "[rml]" )
{
    CHECK( rml_ui::escape_rml( "<color_red>a & b</color>" ) ==
           "&lt;color_red&gt;a &amp; b&lt;/color&gt;" );
    CHECK( rml_ui::escape_rml( "one\ntwo" ) == "one<br/>two" );
}

TEST_CASE( "rml_markup_maps_colors_to_palette_classes", "[rml]" )
{
    CHECK( rml_ui::color_to_rml_classes( c_red ) == "fg-red" );
    // Bold selects the bright foreground.
    CHECK( rml_ui::color_to_rml_classes( c_light_red ) == "fg-lred" );
    CHECK( rml_ui::color_to_rml_classes( c_dark_gray ) == "fg-dgray" );
    // A non-black background gets its own class.
    CHECK( rml_ui::color_to_rml_classes( c_white_red ) == "fg-white bg-red" );
    // Blink selects the bright background.
    CHECK( rml_ui::color_to_rml_classes( i_white ) == "fg-black bg-white" );
    // A fill takes the foreground color, ignoring the background.
    CHECK( rml_ui::color_to_rml_fill_class( c_light_red ) == "bg-lred" );
    CHECK( rml_ui::color_to_rml_fill_class( c_white_red ) == "bg-white" );
}

TEST_CASE( "rml_palette_rcss_defines_every_class", "[rml]" )
{
    const std::string rcss = rml_ui::palette_rcss();
    for( const char *name : {
             "black", "red", "green", "brown", "blue", "magenta", "cyan", "gray",
             "dgray", "lred", "lgreen", "yellow", "lblue", "lmagenta", "lcyan", "white"
         } ) {
        CAPTURE( name );
        CHECK( rcss.find( std::string( ".fg-" ) + name + " {" ) != std::string::npos );
        CHECK( rcss.find( std::string( ".bg-" ) + name + " {" ) != std::string::npos );
    }
}

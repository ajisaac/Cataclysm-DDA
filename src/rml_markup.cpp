#include "rml_markup.h"

#include <array>
#include <cstddef>
#include <string>

#include "color_loader.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "output.h"
#include "sdltiles.h"
#include "string_formatter.h"

namespace
{

constexpr size_t palette_size = color_loader<SDL_Color>::COLOR_NAMES_COUNT;
// Bold and blink select the bright half of the palette, as in curses_color_to_SDL().
constexpr int bright_offset = palette_size / 2;

// color_loader's names, lowercased, indexed like windowsPalette.
const std::array<std::string, palette_size> &palette_names()
{
    static const std::array<std::string, palette_size> names = []() {
        std::array<std::string, palette_size> lowered;
        const std::array<std::string, palette_size> &upper =
            color_loader<SDL_Color>::main_color_names();
        for( size_t i = 0; i < palette_size; ++i ) {
            lowered[i] = to_lower_case( upper[i] );
        }
        return lowered;
    }();
    return names;
}

// The 256-color dark_gray pairs are only set up where supports_256_colors(),
// which the SDL backend does not claim, but stay safe if that ever changes.
bool in_palette( int index )
{
    return index >= 0 && static_cast<size_t>( index ) < palette_size;
}

} // namespace

namespace rml_ui
{

std::string markup_to_rml( std::string_view text, report_color_error color_error )
{
    const std::string expanded = replace_colors( std::string( text ) );
    const std::string_view source( expanded );

    std::string rml;
    rml.reserve( source.size() + source.size() / 4 );
    int open_spans = 0;
    for( size_t i = 0; i < source.size(); ++i ) {
        const char c = source[i];
        if( c == '<' ) {
            const color_tag_parse_result tag =
                get_color_from_tag( source.substr( i ), color_error );
            if( tag.type == color_tag_parse_result::open_color_tag ) {
                rml += "<span class=\"" + color_to_rml_classes( tag.color ) + "\">";
                ++open_spans;
                i = source.find( '>', i );
                continue;
            }
            if( tag.type == color_tag_parse_result::close_color_tag ) {
                if( open_spans > 0 ) {
                    rml += "</span>";
                    --open_spans;
                }
                i = source.find( '>', i );
                continue;
            }
        }
        switch( c ) {
            case '&':
                rml += "&amp;";
                break;
            case '<':
                rml += "&lt;";
                break;
            case '>':
                rml += "&gt;";
                break;
            case '\n':
                rml += "<br/>";
                break;
            default:
                rml += c;
                break;
        }
    }
    for( ; open_spans > 0; --open_spans ) {
        rml += "</span>";
    }
    return rml;
}

std::string escape_rml( std::string_view text )
{
    std::string rml;
    rml.reserve( text.size() + text.size() / 4 );
    for( const char c : text ) {
        switch( c ) {
            case '&':
                rml += "&amp;";
                break;
            case '<':
                rml += "&lt;";
                break;
            case '>':
                rml += "&gt;";
                break;
            case '\n':
                rml += "<br/>";
                break;
            default:
                rml += c;
                break;
        }
    }
    return rml;
}

std::string markup_to_rml( std::string_view text, const nc_color &base_color,
                           report_color_error color_error )
{
    return "<span class=\"" + color_to_rml_classes( base_color ) + "\">" +
           markup_to_rml( text, color_error ) + "</span>";
}

std::string color_to_rml_classes( const nc_color &color )
{
    const cata_cursesport::pairs &pair =
        cata_cursesport::colorpairs[color.to_color_pair_index()];
    const int fg = pair.FG + ( color.is_bold() ? bright_offset : 0 );
    const int bg = pair.BG + ( color.is_blink() ? bright_offset : 0 );

    std::string classes;
    if( in_palette( fg ) ) {
        classes = "fg-" + palette_names()[fg];
    }
    if( bg != catacurses::black && in_palette( bg ) ) {
        if( !classes.empty() ) {
            classes += ' ';
        }
        classes += "bg-" + palette_names()[bg];
    }
    return classes;
}

std::string color_to_rml_fill_class( const nc_color &color )
{
    const cata_cursesport::pairs &pair =
        cata_cursesport::colorpairs[color.to_color_pair_index()];
    const int fg = pair.FG + ( color.is_bold() ? bright_offset : 0 );
    return in_palette( fg ) ? "bg-" + palette_names()[fg] : std::string();
}

std::string palette_rcss()
{
    std::string rcss;
    for( size_t i = 0; i < palette_size; ++i ) {
        const SDL_Color &color = windowsPalette[i];
        const std::string hex = string_format( "#%02x%02x%02x", static_cast<int>( color.r ),
                                               static_cast<int>( color.g ),
                                               static_cast<int>( color.b ) );
        rcss += string_format( ".fg-%s { color: %s; }\n", palette_names()[i], hex );
        rcss += string_format( ".bg-%s { background-color: %s; }\n", palette_names()[i], hex );
    }
    return rcss;
}

} // namespace rml_ui

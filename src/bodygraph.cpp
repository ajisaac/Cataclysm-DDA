#include "bodygraph.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <tuple>

#include "bodypart.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character.h"
#include "character_attire.h"
#include "creature.h"
#include "cursesdef.h"
#include "damage.h"
#include "debug.h"
#include "enums.h"
#include "flexbuffer_json.h"
#include "generic_factory.h"
#include "input_context.h"
#include "memory_fast.h"
#include "output.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "subbodypart.h"
#include "translation.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"
#include "weather.h"

#define BPGRAPH_MAXROWS 20
#define BPGRAPH_MAXCOLS 40

static const bodygraph_id bodygraph_full_body( "full_body" );

static const flag_id json_flag_THERMOMETER( "THERMOMETER" );

namespace
{

generic_factory<bodygraph> bodygraph_factory( "bodygraph" );

} // namespace

/** @relates string_id */
template<>
const bodygraph &string_id<bodygraph>::obj() const
{
    return bodygraph_factory.obj( *this );
}

/** @relates string_id */
template<>
bool string_id<bodygraph>::is_valid() const
{
    return bodygraph_factory.is_valid( *this );
}

void bodygraph::load_bodygraphs( const JsonObject &jo, const std::string &src )
{
    bodygraph_factory.load( jo, src );
}

void bodygraph::reset()
{
    bodygraph_factory.reset();
}

const std::vector<bodygraph> &bodygraph::get_all()
{
    return bodygraph_factory.get_all();
}

void bodygraph::finalize_all()
{
    bodygraph_factory.finalize();
}

void bodygraph::check_all()
{
    bodygraph_factory.check();
}

void bodygraph::load( const JsonObject &jo, std::string_view )
{
    optional( jo, was_loaded, "parent_bodypart", parent_bp );
    optional( jo, was_loaded, "fill_sym", fill_sym );
    if( jo.has_string( "fill_color" ) ) {
        fill_color = color_from_string( jo.get_string( "fill_color" ) );
    }

    if( jo.has_string( "mirror" ) ) {
        mirror.reset();
        mandatory( jo, false, "mirror", mirror );
    } else if( !was_loaded || jo.has_array( "rows" ) ) {
        rows.clear();
        fill_rows.clear();
        for( const JsonValue jval : jo.get_array( "rows" ) ) {
            if( !jval.test_string() ) {
                jval.throw_error( "\"rows\" array must contain string values." );
            } else {
                rows.emplace_back( utf8_display_split( jval.get_string() ) );
            }
        }
        if( jo.has_array( "fill_rows" ) ) {
            for( const JsonValue jval : jo.get_array( "fill_rows" ) ) {
                if( !jval.test_string() ) {
                    jval.throw_error( "\"rows\" array must contain string values." );
                } else {
                    fill_rows.emplace_back( utf8_display_split( jval.get_string() ) );
                }
            }
        }
    }

    if( !was_loaded || jo.has_object( "parts" ) ) {
        parts.clear();
        for( const JsonMember memb : jo.get_object( "parts" ) ) {
            const std::string &sym = memb.name();
            JsonObject mobj = memb.get_object();
            bodygraph_part bpg;
            optional( mobj, false, "body_parts", bpg.bodyparts );
            optional( mobj, false, "sub_body_parts", bpg.sub_bodyparts );
            optional( mobj, false, "sym", bpg.sym, fill_sym );
            if( mobj.has_string( "select_color" ) ) {
                bpg.sel_color = color_from_string( mobj.get_string( "select_color" ) );
            } else {
                bpg.sel_color = fill_color;
            }
            optional( mobj, false, "nested_graph", bpg.nested_graph );
            parts.emplace( sym, bpg );
        }
    }

    if( jo.has_string( "label_fill" ) ) {
        label_fill = jo.get_string( "label_fill" );
    }

}

void bodygraph::finalize()
{
    if( rows.size() > BPGRAPH_MAXROWS ) {
        debugmsg( "body_graph \"%s\" defines more rows than the maximum (%d).", id.c_str(),
                  BPGRAPH_MAXROWS );
    }

    if( !fill_rows.empty() &&  fill_rows.size() != rows.size() ) {
        debugmsg( "body_graph \"%s\" defines a different number of fill_rows than rows (%d vs. %d).",
                  id.c_str(),
                  fill_rows.size(), rows.size() );
    }

    int width = -1;
    bool w_warned = false;
    for( size_t i = 0; i < rows.size(); i++ ) {
        std::vector<std::string> &r = rows[i];
        int w = r.size();
        if( width == -1 ) {
            width = w;
        } else if( !w_warned && width != w ) {
            debugmsg( "body_graph \"%s\" defines rows with different widths.", id.c_str() );
            w_warned = true;
        } else if( !w_warned && w > BPGRAPH_MAXCOLS ) {
            debugmsg( "body_graph \"%s\" defines rows with more columns than the maximum (%d).", id.c_str(),
                      BPGRAPH_MAXCOLS );
            w_warned = true;
        }

        r.insert( r.begin(), ( BPGRAPH_MAXCOLS - w ) / 2, " " );
        r.insert( r.end(), BPGRAPH_MAXCOLS - r.size(), " " );

        if( ! fill_rows.empty() ) {
            std::vector<std::string> &fr = fill_rows[i];
            if( fr.size() != r.size() ) {
                debugmsg( "body_graph \"%s\" defines a different number of columns in fill_rows than in rows (%d vs. %d).",
                          id.c_str(),
                          fr.size(), w );
            }
            fr.insert( fr.begin(), ( BPGRAPH_MAXCOLS - w ) / 2, " " );
            fr.insert( fr.end(), BPGRAPH_MAXCOLS - fr.size(), " " );
        }

    }

    for( std::vector<std::vector<std::string>> temp_rows : {
             rows, fill_rows
         } ) {
        if( !temp_rows.empty() ) {
            for( int i = ( BPGRAPH_MAXROWS - temp_rows.size() ) / 2; i > 0; i-- ) {
                std::vector<std::string> r;
                r.insert( r.begin(), BPGRAPH_MAXCOLS, " " );
                temp_rows.insert( temp_rows.begin(), r );
            }

            for( int i = temp_rows.size(); i <= BPGRAPH_MAXROWS; i++ ) {
                std::vector<std::string> r;
                r.insert( r.begin(), BPGRAPH_MAXCOLS, " " );
                temp_rows.emplace_back( r );
            }
        }
    }
}

void bodygraph::check() const
{
    if( parts.empty() ) {
        debugmsg( "body_graph \"%s\" defined without parts.", id.c_str() );
    }
    for( const auto &bgp : parts ) {
        if( utf8_width( bgp.first ) > 1 ) {
            debugmsg( "part \"%s\" in body_graph \"%s\" is more than 1 character.", bgp.first, id.c_str() );
        }
        if( bgp.second.bodyparts.empty() && bgp.second.sub_bodyparts.empty() ) {
            debugmsg( "part \"%s\" in body_graph \"%s\" contains no body_parts or sub_body_parts definitions.",
                      bgp.first, id.c_str() );
        }
    }
}

/***************************** Display routines *****************************/

/* Basic layout. Not accurate or final, but illustrates the basic idea.

     parts list                      body graph                  part info
<- variable width -><------------ fixed width (40) --------><- variable width ->

+------------------+----------------Body Status-------------+------------------+
| >Head            |                                        | Worn:            |
|  Torso           |                     O                  |  Baseball cap,   |
|  L. Arm          |                   #####                |  Eyeglasses      |
|  R. Arm          |                  #######               | Coverage: ~60%   |
|  L. Hand         |                 ## ### ##              | Encumbrance: 8   |
|  R. Hand         |                 #  ###  #              | Protection:      |
...

*/

using part_tuple =
    std::tuple<bodypart_id, const sub_body_part_type *, const bodygraph_part *, bool>;

namespace
{
/** The body status window, from data/raw/rmlui/bodygraph.rml. */
class bodygraph_window : public rml_ui::window
{
    public:
        explicit bodygraph_window( const std::function<void( bodygraph_window & )> &update ) :
            rml_ui::window( _( "Body status" ), "bodygraph.rml" ), update( update ) {
            set_body_scrolls( false );
        }

        void reveal( const std::string &id ) {
            scroll_into_view( id );
        }
        void scroll_pane( const std::string &id, rml_ui::scroll how ) {
            scroll( id, how );
        }

    protected:
        void on_redraw() override {
            update( *this );
        }

    private:
        std::function<void( bodygraph_window & )> update;
};

struct bodygraph_display {
    const Character *u;
    bodygraph_id id;
    input_context ctxt;
    std::unique_ptr<bodygraph_window> window;
    // For each tuple:
    //   0 = bodypart or parent of subpart
    //   1 = subpart (if viewing a sub part)
    //   2 = bodygraph data for this part
    //   3 = part is present on character
    std::vector<part_tuple> partlist;
    bodygraph_info info;
    std::vector<std::string> info_txt;
    // The width the info is lined up for, in terminal columns
    int info_width = 18;
    int sel_part = 0;
    // The part as last shown, to keep it in view and its info at the top
    int shown_part = -1;

    bodygraph_display() = delete;
    bodygraph_display( const bodygraph_display & ) = delete;
    bodygraph_display( const bodygraph_display && ) = delete;
    explicit bodygraph_display( const Character *u, const bodygraph_id &id );

    void prepare_partlist();
    void prepare_infolist();
    void prepare_infotext();
    void redraw( bodygraph_window &win );
    std::string partlist_rml() const;
    std::string graph_rml() const;
    std::string info_rml() const;
    void display();
};
} // namespace

bodygraph_display::bodygraph_display( const Character *u, const bodygraph_id &id ) :
    u( u ), id( id ), ctxt( "BODYGRAPH" )
{
    if( id.is_null() ) {
        this->id = bodygraph_full_body;
    }

    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "SCROLL_INFOBOX_UP" );
    ctxt.register_action( "SCROLL_INFOBOX_DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    // Clicks on the parts and the close button
    ctxt.register_action( "SELECT" );
}

void bodygraph_display::redraw( bodygraph_window &win )
{
    win.set_size( rml_ui::term_width( clamp( BPGRAPH_MAXCOLS + 40 + 40, 0, TERMX ) ),
                  rml_ui::term_height( clamp( BPGRAPH_MAXROWS + 24, 0, TERMY ) ) );
    // body part subtitle
    if( id->parent_bp.has_value() ) {
        win.set_header( "<div class=\"center\">" + rml_ui::markup_to_rml( string_format( "\\_ %s _/",
                        colorize( to_upper_case( id->parent_bp.value()->name.translated() ), c_yellow ) ),
                        c_white ) + "</div>" );
    }
    win.set_body( "<div class=\"panes\"><div id=\"bg-parts\" class=\"pane\">" + partlist_rml() +
                  "</div><div id=\"bg-graph\" class=\"pane narrow\">" + graph_rml() +
                  "</div><div id=\"bg-info\" class=\"pane\">" + info_rml() + "</div></div>" );
    if( sel_part != shown_part ) {
        shown_part = sel_part;
        win.reveal( "bg-part-" + std::to_string( sel_part ) );
        win.scroll_pane( "bg-info", rml_ui::scroll::top );
    }
}

std::string bodygraph_display::partlist_rml() const
{
    std::string rml;
    for( int i = 0; i < static_cast<int>( partlist.size() ); i++ ) {
        const auto &bgt = partlist[i];
        std::string txt = !std::get<1>( bgt ) ?
                          std::get<0>( bgt )->name.translated() :
                          std::get<1>( bgt )->name.translated();
        const nc_color clr = std::get<3>( bgt ) ? c_white : c_dark_gray;
        // an arrow for parts with a graph of their own
        const std::string nested = !std::get<2>( bgt )->nested_graph.is_null() ?
                                   rml_ui::colored_rml( ">", c_light_green ) : std::string();
        rml += string_format( "<div id=\"bg-part-%d\" class=\"item line bg-part%s\" data-row=\"%d\">", i,
                              i == sel_part ? " selected" : "", i ) + "<span class=\"bg-name\">" +
               rml_ui::colored_rml( uppercase_first_letter( txt ), clr ) + "</span><span>" + nested +
               "</span></div>";
    }
    return rml;
}

static const bodygraph_id &get_bg_rows( const bodygraph_id &bgid )
{
    if( !!bgid->mirror ) {
        return get_bg_rows( bgid->mirror.value() );
    }
    // bodygraph_id is a string_id with stable storage; callers do not pass temporaries.
    // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
    return bgid;
}

std::string bodygraph_display::graph_rml() const
{
    const bodygraph_part *selected_graph = std::get<2>( partlist[sel_part] );
    auto process_sym = [&]( const bodygraph_part * bgp, const std::string & sym ) {
        return colorize( sym, bgp != nullptr && selected_graph == bgp ?
                         selected_graph->sel_color : id->fill_color );
    };
    std::string rml;
    for( const std::string &row : get_bodygraph_lines( *u, process_sym, id ) ) {
        rml += "<div class=\"bg-row\">" + rml_ui::markup_to_rml( row, c_white ) + "</div>";
    }
    return rml;
}

std::string bodygraph_display::info_rml() const
{
    std::string rml;
    if( std::get<3>( partlist[sel_part] ) ) {
        for( const std::string &line : info_txt ) {
            rml += line == "--" ? "<div class=\"sep\"></div>" :
                   "<div class=\"bg-info-line\">" + rml_ui::markup_to_rml( line, c_white ) + "</div>";
        }
    }
    return rml;
}

void bodygraph_display::prepare_partlist()
{
    partlist.clear();
    for( const auto &bgp : id->parts ) {
        for( const bodypart_id &bid : bgp.second.bodyparts ) {
            partlist.emplace_back( bid, static_cast<const sub_body_part_type *>( nullptr ),
                                   &bgp.second, u->has_part( bid, body_part_filter::equivalent ) );
        }
        for( const sub_bodypart_id &sid : bgp.second.sub_bodyparts ) {
            const bodypart_id bid = sid->parent.id();
            partlist.emplace_back( bid, &*sid, &bgp.second, u->has_part( bid, body_part_filter::equivalent ) );
        }
    }
    std::sort( partlist.begin(), partlist.end(),
    []( const part_tuple & a, const part_tuple & b ) {
        if( !!std::get<1>( a ) && !!std::get<1>( b ) ) {
            return std::get<1>( a )->name.translated_lt( std::get<1>( b )->name );
        }
        if( std::get<0>( a )->name.translated_ne( std::get<0>( b )->name ) ) {
            return std::get<0>( a )->name.translated_lt( std::get<0>( b )->name );
        }
        if( !std::get<1>( a ) != !std::get<1>( b ) ) {
            return !std::get<1>( a );
        }
        return a != b;
    } );
}

void bodygraph_display::prepare_infolist()
{
    // reset info for a new read
    info = bodygraph_info();
    info_txt.clear();

    // if character doesn't have this part, don't generate info
    if( !std::get<3>( partlist[sel_part] ) ) {
        return;
    }

    const bodypart_id &bp = std::get<0>( partlist[sel_part] );

    // sbps will either be a group for a body part and we'll need to do averages OR a single sub part
    std::set<sub_bodypart_id> sub_parts;

    // this might be null need to test for nullbp as this all continues
    if( std::get<1>( partlist[sel_part] ) != nullptr ) {
        sub_parts.emplace( std::get<1>( partlist[sel_part] )->id );
    } else {
        for( const sub_bodypart_str_id &sbp : bp->sub_parts ) {
            // don't worry about secondary sub parts would just make things confusing
            if( !sbp->secondary ) {
                sub_parts.emplace( sbp.id() );
            }
        }
    }

    u->worn.prepare_bodymap_info( info, bp, sub_parts, *u );

    // update info text cache
    prepare_infotext();
}

void bodygraph_display::prepare_infotext()
{
    if( info.specific_sublimb ) {
        // parent part
        info_txt.emplace_back( string_format( "%s: %s", colorize( _( "Sub part of" ), c_magenta ),
                                              info.parent_bp_name ) );
    }
    // part health
    std::pair<std::string, nc_color> hpbar = get_hp_bar( info.part_hp_cur, info.part_hp_max );
    info_txt.emplace_back( string_format( "%s: %s", colorize( _( "Health" ), c_magenta ),
                                          colorize( hpbar.first, hpbar.second ) ) );
    // part wetness
    info_txt.emplace_back( string_format( "%s: %d%%", colorize( _( "Wetness" ), c_magenta ),
                                          static_cast<int>( info.wetness * 100.0f ) ) );
    // part temperature
    const bool temp_precise = u->cache_has_item_with( json_flag_THERMOMETER ) ||
                              u->has_flag( json_flag_THERMOMETER );
    const units::temperature temp = units::from_fahrenheit( info.temperature.first / 50.0 );
    info_txt.emplace_back( string_format( "%s: %s", colorize( _( "Body temp" ), c_magenta ),
                                          temp_precise ? colorize( print_temperature( temp ),
                                                  info.temperature.second ) : info.temp_approx ) );
    info_txt.emplace_back( "--" );
    // part effects
    info_txt.emplace_back( string_format( "%s:", colorize( _( "Effects" ), c_magenta ) ) );
    for( const effect &eff : info.effects ) {
        if( eff.get_id()->is_show_in_info() ) {
            game_message_type rt = eff.get_id()->get_rating( eff.get_intensity() );
            info_txt.emplace_back( string_format( "  %s", colorize( eff.disp_name(),
                                                  rt == m_good ? c_green : rt == m_bad ? c_red : c_yellow ) ) );
        }
    }
    info_txt.emplace_back( "--" );
    // worn armor
    info_txt.emplace_back( string_format( "%s:", colorize( _( "Worn" ), c_magenta ) ) );
    for( const std::string &worn : info.worn_names ) {
        info_txt.emplace_back( string_format( "  %s", worn ) );
    }
    info_txt.emplace_back( "--" );
    // coverage
    info_txt.emplace_back( string_format( "%s: %d%%",
                                          colorize( info.specific_sublimb ? _( "Coverage" ) : _( "Coverage (Avg.)" ), c_magenta ),
                                          info.avg_coverage ) );
    info_txt.emplace_back( "--" );
    // encumbrance
    info_txt.emplace_back( string_format( "%s: %d", colorize( _( "Encumbrance" ), c_magenta ),
                                          info.total_encumbrance ) );
    info_txt.emplace_back( "--" );
    // protection
    info_txt.emplace_back( string_format( "%s:",
                                          colorize( info.specific_sublimb ? _( "Protection" ) : _( "Protection (Avg.)" ), c_magenta ) ) );
    std::string prot_legend = string_format( "%s %s %s", colorize( _( "worst" ), c_red ),
                              colorize( _( "median" ), c_yellow ), colorize( _( "best" ), c_light_green ) );
    int wavail = clamp( ( info_width - 2 ) - utf8_width( prot_legend, true ), 0, info_width - 2 );
    prot_legend.insert( prot_legend.begin(), wavail > 4 ? 4 : wavail, ' ' );
    info_txt.emplace_back( prot_legend );
    auto get_res_str = [&]( const damage_type_id & dt ) -> std::string {
        const std::string wval = string_format( info_width <= 18 ? "%4.1f" : "%5.2f", info.worst_case.type_resist( dt ) );
        const std::string mval = string_format( info_width <= 18 ? "%4.1f" : "%5.2f", info.median_case.type_resist( dt ) );
        const std::string bval = string_format( info_width <= 18 ? "%4.1f" : "%5.2f", info.best_case.type_resist( dt ) );
        std::string txt = string_format( "%s %s %s", colorize( wval, c_red ), colorize( mval, c_yellow ), colorize( bval, c_light_green ) );
        int res_avail = clamp( ( info_width - 2 ) - utf8_width( txt, true ), 0, info_width - 2 );
        txt.insert( txt.begin(), res_avail > 4 ? 4 : res_avail, ' ' );
        return txt;
    };
    auto get_env_str = [&]( const damage_type_id & dt ) -> std::string {
        return colorize( string_format( "    %5.2f", info.best_case.type_resist( dt ) ), c_white );
    };
    for( const damage_type &dt : damage_type::get_all() ) {
        if( info.best_case.type_resist( dt.id ) > 1 ) {
            info_txt.emplace_back( string_format( "  %s:", uppercase_first_letter( dt.name.translated() ) ) );
            info_txt.emplace_back( dt.env ? get_env_str( dt.id ) : get_res_str( dt.id ) );
        }
    }
}

void bodygraph_display::display()
{
    prepare_partlist();
    prepare_infolist();
    window = std::make_unique<bodygraph_window>( [this]( bodygraph_window & win ) {
        redraw( win );
    } );

    bool done = false;
    while( !done ) {
        ui_manager::redraw();
        std::string action = ctxt.handle_input();
        if( action == "SELECT" ) {
            const std::string clicked = window->resolve_click( action );
            const int row = window->hovered_index( "data-row" );
            if( clicked != action ) {
                // the close button
                action = clicked;
            } else if( row >= 0 && row < static_cast<int>( partlist.size() ) ) {
                // A click picks a part, and a click on the picked one opens its graph
                if( row == sel_part ) {
                    action = "CONFIRM";
                } else {
                    sel_part = row;
                    prepare_infolist();
                }
            }
        }
        if( action == "QUIT" || ( action == "LEFT" && id != bodygraph_full_body ) ) {
            done = true;
        } else if( action == "CONFIRM" || action == "RIGHT" ) {
            if( !!std::get<2>( partlist[sel_part] ) ) {
                bodygraph_id nextgraph = std::get<2>( partlist[sel_part] )->nested_graph;
                if( !nextgraph.is_null() ) {
                    display_bodygraph( *u, nextgraph );
                    prepare_infolist();
                }
            }
        } else if( action == "SCROLL_INFOBOX_UP" || action == "PAGE_UP" ) {
            window->scroll_pane( "bg-info", rml_ui::scroll::line_up );
        } else if( action == "SCROLL_INFOBOX_DOWN" || action == "PAGE_DOWN" ) {
            window->scroll_pane( "bg-info", rml_ui::scroll::line_down );
        } else if( navigate_ui_list( action, sel_part, 3, partlist.size(), true ) ) {
            prepare_infolist();
        }
    }
}

void display_bodygraph( const Character &u, const bodygraph_id &id )
{
    bodygraph_display bgd( &u, id );
    bgd.display();
}

std::vector<std::string> get_bodygraph_lines( const Character &u,
        const bodygraph_callback &fragment_cb, const bodygraph_id &id, int width, int height,
        std::string_view label )
{
    width = ( width <= 0 || width > BPGRAPH_MAXCOLS ) ? BPGRAPH_MAXCOLS : width;
    height = ( height <= 0 || height > BPGRAPH_MAXROWS ) ? BPGRAPH_MAXROWS : height;

    std::vector<std::string> ret;

    // Bodypart not present on character
    if( !!id->parent_bp && !u.has_part( *id->parent_bp, body_part_filter::equivalent ) ) {
        std::string txt = string_format( u.is_avatar() ?
                                         //~ 1$ = 2nd person pronoun (You), 2$ = body part (left arm)
                                         _( "%1$s do not have a %2$s." ) :
                                         //~ 1$ = name of character, 2$ = body part (left arm)
                                         _( "%1$s does not have a %2$s." ), u.disp_name( false, true ),
                                         id->parent_bp->obj().name.translated() );
        for( int y = 0; y < height; y++ ) {
            if( y == height / 2 ) {
                txt.insert( txt.begin(), center_text_pos( txt, 0, width ), ' ' );
                ret.emplace_back( colorize( txt, c_light_red ) );
            } else {
                ret.emplace_back( width, ' ' );
            }
        }
        return ret;
    }

    const bool hflip = !!id->mirror;
    const bodygraph_id &rid = get_bg_rows( id );
    for( int i = 0; static_cast<size_t>( i ) < rid->rows.size() && i < height; i++ ) {
        std::string ret_row;
        int j = hflip ? rid->rows[i].size() - 1 : 0;
        size_t label_i = 0;
        for( int x = 0 ; x < width && j < BPGRAPH_MAXCOLS && j >= 0; hflip ? j-- : j++, x++ ) {
            std::string sym = " ";
            const std::string &r = rid->rows[i][j];
            bool label_sym = false;
            if( !id->fill_rows.empty() && id->label_fill == id->fill_rows[i][j] ) {
                if( label_i < label.length() ) {
                    label_sym = true;
                    sym = label[label_i++];
                }
            } else {
                sym = id->fill_sym.empty() ? r : id->fill_sym;
            }
            auto iter = id->parts.find( r );
            const bodygraph_part *bgp = nullptr;
            if( iter != id->parts.end() ) {
                bgp = &iter->second;
                bool missing_section = true;
                for( const bodypart_id &bp : iter->second.bodyparts ) {
                    if( u.has_part( bp, body_part_filter::equivalent ) ) {
                        missing_section = false;
                    }
                }
                for( const sub_bodypart_id &sp : iter->second.sub_bodyparts ) {
                    if( u.has_part( sp->parent, body_part_filter::equivalent ) ) {
                        missing_section = false;
                    }
                }
                if( !label_sym ) {
                    sym = missing_section ? " " : ( id->fill_rows.empty() ? iter->second.sym : id->fill_rows[i][j] );
                }
            }
            if( rid->rows[i][j] == " " ) {
                sym = " ";
            }
            ret_row.append( fragment_cb( bgp, sym ) );
        }
        ret.emplace_back( ret_row );
    }
    return ret;
}

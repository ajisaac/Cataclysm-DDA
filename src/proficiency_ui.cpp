#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "input_context.h"
#include "memory_fast.h"
#include "output.h"
#include "point.h"
#include "proficiency.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"

// Basic layout:
//
// +---------------------------Proficiencies---------------------------------+
// |  Category: < All >                                [/] filter  [?] help  |
// |  (category description)                                                 |
// |                                                                         |
// +--------------------------------+----------------------------------------+
// | >Proficiency 1                 |  Time to learn: 8 hours                |
// |  Proficiency 2                 |  Progress: 12%                         |
// |  Proficiency 3                 |  Category: Miscellaneous Crafts        |
// |  Proficiency 4                 |  Pre-requisite:                        |
// |  Proficiency 5                 |  Proficiency 2, Proficiency 4, ...     |
// |  Proficiency 6                 |  Leads to:                             |
// |  Proficiency 7                 |  Proficiency 6, Proficiency 7, ...     |
// |  Proficiency 8                 |  Description:                          |
// |  Proficiency 9                 |  Lorem ipsum etc........               |
// |  Proficiency 10                |                                        |
// ...

using display_prof_deps = std::pair<display_proficiency, std::vector<proficiency_id>>;

namespace
{
/** The proficiencies window, from data/raw/rmlui/proficiencies.rml. */
class prof_rml_window : public rml_ui::window
{
    public:
        explicit prof_rml_window( const std::function<void( prof_rml_window & )> &update ) :
            rml_ui::window( _( "Proficiencies" ), "proficiencies.rml" ), update( update ) {
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
        std::function<void( prof_rml_window & )> update;
};

struct prof_window {
    input_context ctxt;
    std::unique_ptr<prof_rml_window> window;

    const Character *u = nullptr;
    std::vector<const proficiency_category *> cats;
    std::vector<display_prof_deps> all_profs;
    std::vector<display_prof_deps *> filtered_profs;
    std::map<int, std::vector<display_prof_deps *>> profs_by_cat;
    std::string filter_str;
    int sel_prof = 0;
    int current_cat = 0;
    // What was last shown, to keep the selection in view and new details at their top
    std::pair<int, int> shown_selection = { -1, -1 };

    prof_window() = delete;
    prof_window( prof_window & ) = delete;
    prof_window( prof_window && ) = delete;
    explicit prof_window( const Character *u ) : u( u ) {}
    void filter();
    std::vector<display_prof_deps *> &get_current_set();
    void populate_categories();
    void redraw( prof_rml_window &win );
    std::string header_rml();
    std::string profs_rml();
    std::string details_rml();
    void run( std::optional<proficiency_id> default_selection = std::nullopt );
};
} // namespace

std::vector<display_prof_deps *> &prof_window::get_current_set()
{
    return filter_str.empty() ? profs_by_cat[current_cat] : filtered_profs;
}

void prof_window::filter()
{
    enum class _prof_filter_prefix {
        LEARNED,
        UNLEARNED,
        AS_PROGRESS,
        NONE
    };
    std::string qry = trim( filter_str );
    filtered_profs.clear();
    if( qry.empty() ) {
        return;
    }
    _prof_filter_prefix prefix = _prof_filter_prefix::NONE;
    if( qry.size() > 2 && qry[1] == ':' ) {
        switch( qry[0] ) {
            case 'l':
                prefix = _prof_filter_prefix::LEARNED;
                qry = qry.substr( 2 );
                break;
            case 'i':
                prefix = _prof_filter_prefix::UNLEARNED;
                qry = qry.substr( 2 );
                break;
            case 'p':
                prefix = _prof_filter_prefix::AS_PROGRESS;
                qry = qry.substr( 2 );
                break;
            default:
                break;
        }
    }
    float prog_val = 0.0f;
    if( prefix == _prof_filter_prefix::AS_PROGRESS ) {
        std::optional<double> v = svtod( qry, false );
        if( !v.has_value() ) {
            // User mistyped query. Not severe enough for debugmsg.
            DebugLog( DebugLevel::D_WARNING, DebugClass::D_GAME ) <<
                    "Malformed proficiency query \"" << filter_str << "\"";
            return;
        }
        prog_val = v.value();
    }
    for( display_prof_deps &dp : all_profs ) {
        if( prefix == _prof_filter_prefix::AS_PROGRESS ) {
            if( prog_val < dp.first.practice * 100.0f ) {
                filtered_profs.push_back( &dp );
            }
            continue;
        }
        if( ( prefix == _prof_filter_prefix::LEARNED && !dp.first.known ) ||
            ( prefix == _prof_filter_prefix::UNLEARNED && dp.first.known ) ) {
            continue;
        }
        if( qry == "*" || lcmatch( dp.first.id->name(), qry ) ) {
            filtered_profs.push_back( &dp );
        }
    }
}

void prof_window::populate_categories()
{
    for( display_proficiency &p : u->display_proficiencies() ) {
        all_profs.push_back( { p, {} } );
    }
    for( const proficiency &p : proficiency::get_all() ) {
        for( const proficiency_id &pid : p.required_proficiencies() ) {
            auto iter = std::find_if( all_profs.begin(), all_profs.end(),
            [&pid]( const display_prof_deps & dpd ) {
                return dpd.first.id == pid;
            } );
            if( iter != all_profs.end() ) {
                iter->second.push_back( p.prof_id() );
            }
        }
    }
    cats.push_back( nullptr );
    for( const proficiency_category &pc : proficiency_category::get_all() ) {
        cats.push_back( &pc );
    }

    for( int i = 0; i < static_cast<int>( cats.size() ); i++ ) {
        for( display_prof_deps &dp : all_profs ) {
            if( i == 0 || dp.first.id->prof_category() == cats[i]->id ) {
                profs_by_cat[i].push_back( &dp );
            }
        }
    }
}

std::string prof_window::details_rml()
{
    std::vector<display_prof_deps *> &cur_set = get_current_set();
    if( cur_set.empty() ) {
        return std::string();
    }
    display_prof_deps *p = cur_set[sel_prof];
    std::vector<std::string> lines;
    // Time to learn
    std::string ttl = !p->first.id->can_learn() ? colorize( _( "Can't learn" ), c_dark_gray ) :
                      to_string( p->first.id->time_to_learn() );
    lines.push_back( string_format( "%s: %s", colorize( _( "Time to learn" ), c_magenta ), ttl ) );
    // Progress
    lines.push_back( string_format( "%s: %.2f%%", colorize( _( "Progress" ), c_magenta ),
                                    p->first.practice * 100.0f ) );
    // Category
    lines.push_back( string_format( "%s: %s", colorize( _( "Category" ), c_magenta ),
                                    p->first.id->prof_category()->_name.translated() ) );
    // Pre-requisites
    lines.push_back( string_format( "%s:", colorize( _( "Pre-requisites" ), c_magenta ) ) );
    lines.push_back( enumerate_as_string( p->first.id->required_proficiencies(),
    []( const proficiency_id & pid ) {
        return colorize( pid->name(), c_yellow );
    }, enumeration_conjunction::and_ ) );
    // Leads to
    lines.push_back( string_format( "%s:", colorize( _( "Leads to" ), c_magenta ) ) );
    lines.push_back( enumerate_as_string( p->second, []( const proficiency_id & pid ) {
        return colorize( pid->name(), c_yellow );
    }, enumeration_conjunction::and_ ) );
    std::string rml;
    for( const std::string &line : lines ) {
        rml += "<div>" + rml_ui::markup_to_rml( line, c_white ) + "</div>";
    }
    // Description
    return rml + "<div class=\"prof-desc\">" + rml_ui::markup_to_rml( string_format( "%s: %s",
            colorize( _( "Description" ), c_magenta ), p->first.id->description() ), c_light_gray ) + "</div>";
}

std::string prof_window::profs_rml()
{
    std::string rml;
    std::vector<display_prof_deps *> &cur_set = get_current_set();
    for( int i = 0; i < static_cast<int>( cur_set.size() ); i++ ) {
        rml += string_format( "<div id=\"prof-row-%d\" class=\"item line%s\" data-row=\"%d\">", i,
                              sel_prof == i ? " selected" : "", i ) +
               rml_ui::colored_rml( cur_set[i]->first.id->name(), cur_set[i]->first.color ) + "</div>";
    }
    return rml;
}

std::string prof_window::header_rml()
{
    std::string cat_title = !filter_str.empty() ? _( "FILTERED" ) :
                            ( current_cat == 0 ) ? _( "ALL" ) : cats[current_cat]->_name.translated();
    std::string hint_txt = string_format( "[%s] %s   [%s] %s",
                                          colorize( ctxt.get_desc( "FILTER", 1 ), c_yellow ), _( "filter" ),
                                          colorize( ctxt.get_desc( "HELP_KEYBINDINGS", 1 ), c_yellow ), _( "help" ) );
    std::string rml = "<div class=\"hbox\"><div class=\"grow\">" +
                      rml_ui::colored_rml( std::string( _( "Category" ) ) + ":  ", c_white ) +
                      rml_ui::button_rml( "PREV_CATEGORY", rml_ui::colored_rml( "<", c_yellow ) ) +
                      rml_ui::colored_rml( cat_title, c_light_green ) +
                      rml_ui::button_rml( "NEXT_CATEGORY", rml_ui::colored_rml( ">", c_yellow ) ) +
                      "</div><div class=\"fixed\">" + rml_ui::markup_to_rml( hint_txt, c_white ) + "</div></div>";
    if( current_cat != 0 && filter_str.empty() ) {
        rml += "<div>" + rml_ui::markup_to_rml( cats[current_cat]->_description.translated(),
                                                c_light_gray ) + "</div>";
    }
    return rml;
}

void prof_window::redraw( prof_rml_window &win )
{
    win.set_size( rml_ui::term_width( std::min( 120, TERMX ) ), rml_ui::term_height( std::min( 28,
                  TERMY ) ) );
    win.set_header( header_rml() );
    win.set_body( "<div class=\"panes\"><div id=\"prof-list\" class=\"pane\">" + profs_rml() +
                  "</div><div id=\"prof-details\" class=\"pane\">" + details_rml() + "</div></div>" );
    if( shown_selection != std::make_pair( current_cat, sel_prof ) ) {
        shown_selection = std::make_pair( current_cat, sel_prof );
        win.reveal( "prof-row-" + std::to_string( sel_prof ) );
        win.scroll_pane( "prof-details", rml_ui::scroll::top );
    }
}

void prof_window::run( std::optional<proficiency_id> default_selection )
{
    if( !u ) {
        return;
    }

    ctxt = input_context( "PROFICIENCY_WINDOW" );
    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "PREV_CATEGORY" );
    ctxt.register_action( "NEXT_CATEGORY" );
    ctxt.register_action( "FILTER" );
    ctxt.register_action( "QUIT" );
    // Clicks on the list, the category arrows and the close button
    ctxt.register_action( "SELECT" );

    populate_categories();
    window = std::make_unique<prof_rml_window>( [this]( prof_rml_window & win ) {
        redraw( win );
    } );

    if( default_selection ) {
        std::vector<display_prof_deps *> &cur_set = get_current_set();
        for( int i = 0; i < static_cast<int>( cur_set.size() ); i++ ) {
            if( cur_set[i]->first.id == default_selection.value() ) {
                sel_prof = i;
                break;
            }
        }
    }

    bool done = false;
    while( !done ) {
        ui_manager::redraw();
        std::string action = ctxt.handle_input();
        std::vector<display_prof_deps *> &cur_set = get_current_set();
        if( action == "SELECT" ) {
            const std::string clicked = window->resolve_click( action );
            const int row = window->hovered_index( "data-row" );
            if( clicked != action ) {
                action = clicked;
            } else if( row >= 0 && row < static_cast<int>( cur_set.size() ) ) {
                sel_prof = row;
            }
        }
        if( navigate_ui_list( action, sel_prof, 10, cur_set.size(), true ) ) {
        } else if( filter_str.empty() && ( action == "LEFT" || action == "PREV_CATEGORY" ||
                                           action == "PREV_TAB" ||
                                           action == "RIGHT" || action == "NEXT_CATEGORY" || action == "NEXT_TAB" ) ) {
            current_cat = inc_clamp_wrap( current_cat,
                                          action == "RIGHT" || action == "NEXT_CATEGORY" || action == "NEXT_TAB", cats.size() );
            sel_prof = 0;
        } else if( action == "FILTER" ) {
            //~ Refers to single-character search prefixes, like p: or s:
            std::string desc( _( "Available prefixes:" ) );
            desc += "\n  ";
            desc += _( "<color_c_light_cyan>l:</color>  Learned proficiencies" );
            desc += "\n  ";
            desc += _( "<color_c_light_cyan>i:</color>  In-progress proficiencies" );
            desc += "\n  ";
            desc += _( "<color_c_light_cyan>p:</color>  Returns proficiencies above the given progress.  "
                       "ex: <color_c_yellow>p:95.00</color>" );
            desc += "\n\n";
            desc += _( "By default, the filter applies to proficiencies containing the given string.  "
                       "Using just a glob <color_c_yellow>*</color> matches anything.\n"
                       "ex: <color_c_yellow>l:*</color> returns all learned proficiencies." );
            desc += "\n\n";

            string_input_popup popup;
            popup
            .title( _( "Search:" ) )
            .width( 78 )
            .description( desc )
            .desc_color( c_light_gray )
            .identifier( "proficiency_filter" )
            .hist_use_uilist( false )
            .edit( filter_str );

            if( popup.confirmed() ) {
                sel_prof = 0;
                current_cat = 0;
                filter();
            }
        } else if( action == "QUIT" ) {
            done = true;
        }
    }
}

void show_proficiencies_window( const Character &u,
                                std::optional<proficiency_id> default_selection )
{
    prof_window w( &u );
    w.run( default_selection );
}

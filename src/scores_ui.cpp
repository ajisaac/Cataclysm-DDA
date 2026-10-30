#include "scores_ui.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "achievement.h"
#include "color.h"
#include "enum_traits.h"
#include "event_statistics.h"
#include "game.h"
#include "input_context.h"
#include "kill_tracker.h"
#include "localized_comparator.h"
#include "mtype.h"
#include "past_games_info.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "stats_tracker.h"
#include "string_formatter.h"
#include "string_id.h"
#include "translation.h"
#include "translations.h"
#include "ui_manager.h"

template <typename E> struct enum_traits;

namespace
{
enum class scores_ui_tab : int {
    achievements = 0,
    conducts,
    scores,
    kills,
    num_tabs
};
} // namespace

template<>
struct enum_traits<scores_ui_tab> {
    static constexpr scores_ui_tab first = scores_ui_tab::achievements;
    static constexpr scores_ui_tab last = scores_ui_tab::num_tabs;
};

namespace
{
class scores_ui_impl : public rml_ui::window
{
    public:
        scores_ui_impl() : rml_ui::window( _( "Your scores" ) ) {}
        void init_data();
        void run();

    protected:
        void on_redraw() override;

    private:
        std::string achievements_rml( bool use_conducts = false ) const;
        std::string scores_rml() const;
        std::string kills_rml() const;

        scores_ui_tab selected_tab = enum_traits<scores_ui_tab>::first;
        // The tab the body was last filled for, to scroll a new one to its top
        scores_ui_tab shown_tab = enum_traits<scores_ui_tab>::last;

        bool monster_group_collapsed = false;
        bool npc_group_collapsed = false;

        std::vector<std::string> achievements_text;
        std::vector<std::string> conducts_text;
        std::vector<std::string> scores_text;
        std::vector<std::tuple<int, std::string, nc_color, std::string>> monster_kills_data;
        std::vector<std::string> npc_kills_data;

        int monster_kills = 0;
        int npc_kills = 0;
        int total_kills = 0;
};

void scores_ui_impl::run()
{
    input_context ctxt( "SCORES_UI" );

    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "TOGGLE_MONSTER_GROUP" );
    ctxt.register_action( "TOGGLE_NPC_GROUP" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    // Smooths out our handling, makes tabs load immediately after input instead of waiting for next.
    ctxt.set_timeout( 10 );

    while( true ) {
        ui_manager::redraw_invalidated();

        std::string action = ctxt.handle_input();
        if( action == "SELECT" && hovered_tab() >= 0 ) {
            selected_tab = static_cast<scores_ui_tab>( hovered_tab() );
            continue;
        }
        action = resolve_click( action );

        if( action == "QUIT" ) {
            break;
        } else if( action == "TOGGLE_MONSTER_GROUP" ) {
            monster_group_collapsed = !monster_group_collapsed;
        } else if( action == "TOGGLE_NPC_GROUP" ) {
            npc_group_collapsed = !npc_group_collapsed;
        } else if( action == "UP" ) {
            scroll_body( rml_ui::scroll::line_up );
        } else if( action == "DOWN" ) {
            scroll_body( rml_ui::scroll::line_down );
        } else if( action == "NEXT_TAB" || action == "RIGHT" ) {
            ++selected_tab;
        } else if( action == "PREV_TAB" || action == "LEFT" ) {
            --selected_tab;
        } else if( action == "PAGE_UP" ) {
            scroll_body( rml_ui::scroll::page_up );
        } else if( action == "PAGE_DOWN" ) {
            scroll_body( rml_ui::scroll::page_down );
        } else if( action == "HOME" ) {
            scroll_body( rml_ui::scroll::top );
        } else if( action == "END" ) {
            scroll_body( rml_ui::scroll::bottom );
        }
    }
}

void scores_ui_impl::init_data()
{
    const achievements_tracker &achievements = g->achievements();
    // Load past game info beforehand because otherwise it may erase an `achievement_tracker`
    // within a call to its method when lazy-loaded, causing dangling pointer.
    get_past_games();
    std::vector<const achievement *> valid_achievements = achievements.valid_achievements();
    valid_achievements.erase(
        std::remove_if( valid_achievements.begin(), valid_achievements.end(),
    [&]( const achievement * a ) {
        return achievements.is_hidden( a );
    } ), valid_achievements.end() );

    using sortable_achievement =
        std::tuple<achievement_completion, std::string, const achievement *>;
    std::vector<sortable_achievement> sortable_achievements;
    std::transform( valid_achievements.begin(), valid_achievements.end(),
                    std::back_inserter( sortable_achievements ),
    [&]( const achievement * ach ) {
        achievement_completion comp = achievements.is_completed( ach->id );
        return std::make_tuple( comp, ach->name().translated(), ach );
    } );
    std::sort( sortable_achievements.begin(), sortable_achievements.end(), localized_compare );
    for( const sortable_achievement &entry : sortable_achievements ) {
        const achievement *ach = std::get<const achievement *>( entry );
        if( ach->is_conduct() ) {
            conducts_text.emplace_back( achievements.ui_text_for( ach ) );
        } else {
            achievements_text.emplace_back( achievements.ui_text_for( ach ) );
        }
    }

    stats_tracker &stats = g->stats();
    std::vector<const score *> valid_scores = stats.valid_scores();
    scores_text.reserve( stats.valid_scores().size() );
    for( const score *scr : valid_scores ) {
        scores_text.emplace_back( scr->description( stats ).c_str() );
    }

    const kill_tracker &kills_data = g->get_kill_tracker();
    monster_kills = kills_data.monster_kill_count();
    npc_kills = kills_data.npc_kill_count();
    total_kills = kills_data.total_kill_count();

    for( const auto &entry : kills_data.kills ) {
        const int num_kills = entry.second;
        const mtype &m = entry.first.obj();
        const std::string &symbol = m.sym;
        nc_color color = m.color;
        const std::string &name = m.nname();
        monster_kills_data.emplace_back( num_kills, symbol, color, name );
    }
    auto sort_by_count_then_name = []( auto & a, auto & b ) {
        return
            std::tie( std::get<0>( a ), std::get<3>( b ) )
            >
            std::tie( std::get<0>( b ), std::get<3>( a ) );
    };
    std::sort( monster_kills_data.begin(), monster_kills_data.end(), sort_by_count_then_name );
    for( const auto &entry : kills_data.npc_kills ) {
        npc_kills_data.emplace_back( entry );
    }
    std::sort( npc_kills_data.begin(), npc_kills_data.end(), localized_compare );
}

std::string scores_ui_impl::achievements_rml( bool use_conducts ) const
{
    const auto paragraph = []( const std::string & text ) {
        return "<p>" + rml_ui::escape_rml( text ) + "</p>";
    };
    if( !g->achievements().is_enabled() ) {
        return paragraph( _( "Achievements and conducts are disabled for debug characters." ) );
    }
    if( use_conducts && conducts_text.empty() ) {
        return paragraph( _( "This game has no valid conducts." ) );
    }
    if( !use_conducts && achievements_text.empty() ) {
        return paragraph( _( "This game has no valid achievements." ) );
    }
    std::string rml;
    for( const std::string &entry : use_conducts ? conducts_text : achievements_text ) {
        rml += "<div class=\"pre\">" + rml_ui::markup_to_rml( entry ) + "</div><div class=\"sep\"></div>";
    }
    return rml + "<div class=\"gap\"></div>" + paragraph(
               use_conducts
               ? _( "Note that only conducts that existed when you started this game and still exist now will appear here." )
               : _( "Note that only achievements that existed when you started this game and still exist now will appear here." ) );
}

std::string scores_ui_impl::scores_rml() const
{
    if( scores_text.empty() ) {
        return "<p>" + rml_ui::escape_rml( _( "This game has no valid scores." ) ) + "</p>";
    }
    std::string rml;
    for( const std::string &entry : scores_text ) {
        rml += "<p>" + rml_ui::escape_rml( entry ) + "</p>";
    }
    return rml + "<div class=\"gap\"></div><p>" + rml_ui::escape_rml(
               _( "Note that only scores that existed when you started this game and still exist now will appear here." ) )
           + "</p>";
}

std::string scores_ui_impl::kills_rml() const
{
    // A row of the kill list: the count and the symbol in the monospace font, as on the map
    const auto kill_row = []( int count, const std::string & symbol, const nc_color & color,
    const std::string & name ) {
        return "<div class=\"pre\"><span class=\"mono\">" + rml_ui::colored_rml( string_format( "%5d ",
                count ), c_light_gray ) + rml_ui::colored_rml( symbol, color ) + "</span>" +
               rml_ui::colored_rml( " " + name, c_light_gray ) + "</div>";
    };
    // Clicking a heading opens or closes its group
    const auto heading = []( const std::string & action, const std::string & text, bool collapsed ) {
        return "<div class=\"section-head\" data-action=\"" + action + "\">" +
               std::string( collapsed ? "+ " : "- " ) + rml_ui::escape_rml( text ) + "</div>";
    };

    std::string rml = heading( "TOGGLE_MONSTER_GROUP", string_format( _( "Monster kills (%d):" ),
                               monster_kills ), monster_group_collapsed );
    if( !monster_group_collapsed ) {
        if( monster_kills == 0 ) {
            rml += "<p>" + rml_ui::escape_rml( _( "You haven't killed any monsters yet!" ) ) + "</p>";
        } else {
            for( const auto &entry : monster_kills_data ) {
                rml += kill_row( std::get<0>( entry ), std::get<1>( entry ), std::get<2>( entry ),
                                 std::get<3>( entry ) );
            }
        }
    }
    rml += heading( "TOGGLE_NPC_GROUP", string_format( _( "NPC kills (%d):" ), npc_kills ),
                    npc_group_collapsed );
    if( !npc_group_collapsed ) {
        if( npc_kills == 0 ) {
            rml += "<p>" + rml_ui::escape_rml( _( "You haven't killed any NPCs yet!" ) ) + "</p>";
        } else {
            for( const std::string &npc_name : npc_kills_data ) {
                rml += kill_row( 1, "@", c_magenta, npc_name );
            }
        }
    }
    return rml;
}

void scores_ui_impl::on_redraw()
{
    set_size( "89%", "89%" );
    set_tabs( { _( "ACHIEVEMENTS" ), _( "CONDUCTS" ), _( "SCORES" ), _( "KILLS" ) },
              static_cast<size_t>( selected_tab ) );

    switch( selected_tab ) {
        case scores_ui_tab::achievements:
            set_body( achievements_rml() );
            break;
        case scores_ui_tab::conducts:
            set_body( achievements_rml( true ) );
            break;
        case scores_ui_tab::scores:
            set_body( scores_rml() );
            break;
        case scores_ui_tab::kills:
            set_body( kills_rml() );
            break;
        case scores_ui_tab::num_tabs:
            break;
    }
    if( shown_tab != selected_tab ) {
        shown_tab = selected_tab;
        scroll_body( rml_ui::scroll::top );
    }
}
} // namespace

void show_scores_ui()
{
    scores_ui_impl ui;
    ui.init_data();
    ui.run();
}

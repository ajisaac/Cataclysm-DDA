#include "study_zone_ui.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cata_utility.h"
#include "character_id.h"
#include "game.h"
#include "input_context.h"
#include "localized_comparator.h"
#include "memory_fast.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "skill.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"


namespace
{
class study_zone_window : public rml_ui::window
{
    public:
        explicit study_zone_window( std::map<std::string, std::set<skill_id>> &npc_skill_preferences ) :
            rml_ui::window( _( "Study Zone Skill Preferences" ) ),
            npc_skill_preferences( npc_skill_preferences ) {
            ctxt = input_context( "STUDY_ZONE_UI" );
            ctxt.register_action( "CONFIRM" );
            ctxt.register_action( "FILTER" );
            ctxt.register_action( "QUIT" );
            // For the checkboxes and buttons
            ctxt.register_action( "SELECT" );
            ctxt.set_timeout( 10 );

            // Get all skills
            for( const Skill &s : Skill::skills ) {
                all_skills.push_back( s.ident() );
            }
            std::sort( all_skills.begin(), all_skills.end(),
            []( const skill_id & a, const skill_id & b ) {
                return localized_compare( a->name(), b->name() );
            } );

            // get followers
            std::set<character_id> follower_ids = g->get_follower_list();
            for( const character_id &id : follower_ids ) {
                shared_ptr_fast<npc> npc_ptr = overmap_buffer.find_npc( id );
                if( npc_ptr && !npc_ptr->is_hallucination() && npc_ptr->is_player_ally() ) {
                    npc_names.push_back( npc_ptr->name );
                }
            }
            // include NPCs that are already in preferences but not in follower list
            for( const auto &pair : npc_skill_preferences ) {
                if( std::find( npc_names.begin(), npc_names.end(), pair.first ) == npc_names.end() ) {
                    npc_names.push_back( pair.first );
                }
            }
            std::sort( npc_names.begin(), npc_names.end(), localized_compare );

            // for new NPCs set all checkboxes
            for( const std::string &npc_name : npc_names ) {
                if( npc_skill_preferences.find( npc_name ) == npc_skill_preferences.end() ) {
                    std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                    for( const skill_id &skill : all_skills ) {
                        npc_skills.insert( skill );
                    }
                    preferences_changed = true;
                }
            }
            show_filter( std::string() );
        }

        study_zone_ui_result execute() {
            while( true ) {
                ui_manager::redraw();
                std::string action = ctxt.handle_input();

                // The filter field has already taken any typing
                if( filtering ) {
                    skill_filter = filter_text();
                    if( action == "CONFIRM" || action == "QUIT" ) {
                        if( action == "QUIT" ) {
                            skill_filter.clear();
                            show_filter( skill_filter );
                        }
                        filtering = false;
                        stop_filter();
                        continue;
                    }
                }

                if( action == "SELECT" ) {
                    const int skill = hovered_index( "data-skill" );
                    const int column = hovered_index( "data-npc" );
                    if( skill >= 0 && column >= 0 ) {
                        toggle( skill, column );
                        continue;
                    }
                    action = resolve_click( action );
                }

                if( action == "CONFIRM" || action == "DONE" ) {
                    break;
                } else if( action == "QUIT" ) {
                    if( !skill_filter.empty() ) {
                        skill_filter.clear();
                        show_filter( skill_filter );
                    } else {
                        return study_zone_ui_result::canceled;
                    }
                } else if( action == "FILTER" ) {
                    filtering = true;
                    start_filter( skill_filter );
                } else if( action == "CHECK_ALL" || action == "CLEAR_ALL" ) {
                    set_all( action == "CHECK_ALL" );
                }
            }
            return preferences_changed ? study_zone_ui_result::changed : study_zone_ui_result::successful;
        }

    protected:
        std::vector<skill_id> filtered_skills() const {
            std::vector<skill_id> filtered;
            for( const skill_id &skill : all_skills ) {
                if( skill_filter.empty() || lcmatch( skill->name(), skill_filter ) ) {
                    filtered.push_back( skill );
                }
            }
            return filtered;
        }

        void toggle( int skill_index, int column ) {
            const std::vector<skill_id> filtered = filtered_skills();
            if( static_cast<size_t>( skill_index ) >= filtered.size() ||
                static_cast<size_t>( column ) >= npc_names.size() ) {
                return;
            }
            const skill_id &skill = filtered[skill_index];
            const std::string &npc_name = npc_names[column];
            std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
            if( npc_skills.count( skill ) > 0 ) {
                npc_skills.erase( skill );
            } else {
                npc_skills.insert( skill );
            }
            if( npc_skills.empty() ) {
                npc_skill_preferences.erase( npc_name );
            }
            preferences_changed = true;
        }

        // Check or clear every skill shown, for every NPC
        void set_all( bool checked ) {
            for( const std::string &npc_name : npc_names ) {
                std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                for( const skill_id &skill : filtered_skills() ) {
                    if( checked ) {
                        npc_skills.insert( skill );
                    } else {
                        npc_skills.erase( skill );
                    }
                }
                if( npc_skills.empty() ) {
                    npc_skill_preferences.erase( npc_name );
                }
            }
            preferences_changed = true;
        }

        void on_redraw() override {
            set_size( "90%; max-width: 1200dp", "98%" );

            // Skills as rows and npc names as columns
            const std::vector<skill_id> filtered = filtered_skills();
            std::string rml = "<div style=\"overflow-x: auto;\"><table class=\"grid bordered\"><thead><tr><th>" +
                              rml_ui::escape_rml( _( "Skill" ) ) + "</th>";
            for( const std::string &npc_name : npc_names ) {
                rml += "<th class=\"center\">" + rml_ui::escape_rml( npc_name ) + "</th>";
            }
            rml += "</tr></thead><tbody>";
            for( size_t row = 0; row < filtered.size(); row++ ) {
                const skill_id &skill = filtered[row];
                rml += "<tr><td>" + rml_ui::escape_rml( skill->name() ) + "</td>";
                for( size_t column = 0; column < npc_names.size(); column++ ) {
                    const auto iter = npc_skill_preferences.find( npc_names[column] );
                    const bool is_selected = iter != npc_skill_preferences.end() &&
                                             iter->second.count( skill ) > 0;
                    rml += string_format( "<td class=\"center item\" data-skill=\"%d\" data-npc=\"%d\">"
                                          "<span class=\"check%s\"></span></td>",
                                          row, column, is_selected ? " on" : "" );
                }
                rml += "</tr>";
            }
            set_body( rml + "</tbody></table></div>" );

            // filter input, buttons, and Done button
            set_footer( "<div class=\"hbox\" style=\"align-items: center;\"><div class=\"grow\">" +
                        rml_ui::markup_to_rml( rml_ui::keybinding_markup( ctxt, "FILTER",
                                               _( "Filter skills: " ), filtering ) ) + "</div>" +
                        rml_ui::button_rml( "CHECK_ALL", rml_ui::escape_rml( _( "Check All" ) ) ) +
                        rml_ui::button_rml( "CLEAR_ALL", rml_ui::escape_rml( _( "Clear All" ) ) ) +
                        rml_ui::button_rml( "DONE", rml_ui::escape_rml( _( "Done" ) ) ) + "</div>" );
        }

    private:
        std::map<std::string, std::set<skill_id>> &npc_skill_preferences;
        std::vector<skill_id> all_skills;
        std::vector<std::string> npc_names;
        std::string skill_filter;
        bool filtering = false;
        bool preferences_changed = false;
        input_context ctxt;
};
} // namespace

study_zone_ui_result query_study_zone_skills( std::map<std::string, std::set<skill_id>>
        &npc_skill_preferences )
{
    study_zone_window window( npc_skill_preferences );
    return window.execute();
}


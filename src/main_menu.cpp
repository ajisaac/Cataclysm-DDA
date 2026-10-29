#include "main_menu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <istream>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <string>

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#include "auto_pickup.h"
#include "avatar.h"
#include "cata_path.h"
#include "cata_scope_helpers.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character_id.h"
#include "color.h"
#include "debug.h"
#include "enums.h"
#include "filesystem.h"
#include "game.h"
#include "gamemode.h"
#include "get_version.h"
#include "help.h"
#include "imgui_demo.h"
#include "localized_comparator.h"
#include "mapbuffer.h"
#include "mapsharing.h"
#include "messages.h"
#include "music.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "popup.h"
#include "rml_markup.h"
#include "rml_ui.h"
#include "safemode_ui.h"
#include "scenario.h"
#include "sdlsound.h"
#include "sounds.h"
#include "string_formatter.h"
#include "text_snippets.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "ui_style_picker.h"
#include "wcwidth.h"
#include "worldfactory.h"

static const mod_id MOD_INFORMATION_dda( "dda" );
static const mod_id MOD_INFORMATION_dda_tutorial( "dda_tutorial" );

namespace
{
enum class main_menu_opts : int {
    MOTD = 0,
    NEWCHAR,
    LOADCHAR,
    WORLD,
    TUTORIAL,
    SETTINGS,
    HELP,
    CREDITS,
    QUIT,
    NUM_MENU_OPTS,
};
} // namespace

std::string main_menu::queued_world_to_load;
std::string main_menu::queued_save_id_to_load;

static int getopt( main_menu_opts o )
{
    return static_cast<int>( o );
}

void main_menu::on_move() const
{
    sfx::play_variant_sound( "menu_move", "default", 100 );
}

void main_menu::on_error()
{
    sfx::play_variant_sound( "menu_error", "default", 100 );
}

// The main menu's document. main_menu keeps the state and handles the input,
// and pushes what to show here before each redraw.
class main_menu_rml : public rml_ui::document
{
    public:
        main_menu_rml() : rml_ui::document( "main_menu.rml" ) {}

        // Show this content in the element with this id; returns whether it changed
        bool show( const std::string &id, const std::string &rml ) {
            std::string &last = shown[id];
            if( last == rml ) {
                return false;
            }
            last = rml;
            set_inner_rml( id, rml );
            return true;
        }

        void mark( const std::string &id, const std::string &name, bool active ) {
            set_class( id, name, active );
        }

        void scroll_text( rml_ui::scroll how ) {
            scroll( "text", how );
        }

        // The menu bar's tab, or the shown option, that the mouse is over, or -1
        int hovered_tab() const {
            return hovered_child( "menu" );
        }
        int hovered_option() const {
            return hovered_child( "sub" );
        }

    private:
        std::map<std::string, std::string> shown;
};

std::vector<std::string> main_menu::sub_options() const
{
    std::vector<std::string> options;
    switch( static_cast<main_menu_opts>( sel1 ) ) {
        case main_menu_opts::SETTINGS:
            for( const std::string &item : vSettingsSubItems ) {
                options.push_back( shortcut_text( c_yellow, item ) );
            }
            break;
        case main_menu_opts::NEWCHAR:
            for( const std::string &item : vNewGameSubItems ) {
                options.push_back( shortcut_text( c_yellow, item ) );
            }
            break;
        case main_menu_opts::LOADCHAR:
        case main_menu_opts::WORLD:
            if( sel1 == getopt( main_menu_opts::WORLD ) ) {
                options.push_back( colorize( _( "Create World" ), c_yellow ) );
            }
            for( const auto& [name, world] : world_generator->get_all_worlds() ) {
                const nc_color clr = name == "TUTORIAL" || name == "DEFENSE" ? c_light_cyan :
                                     c_white;
                const int saves = static_cast<int>( world->world_saves.size() );
                options.push_back( colorize( string_format( "%s (%d)", name, saves ), clr ) );
            }
            break;
        case main_menu_opts::MOTD:
        case main_menu_opts::CREDITS:
        case main_menu_opts::TUTORIAL:
        case main_menu_opts::HELP:
        case main_menu_opts::QUIT:
        default:
            break;
    }
    return options;
}

void main_menu::show_rml( main_menu_rml &doc )
{
    // ASCII art when there is some, or a line of text
    const bool art = mmenu_title.size() > 1;
    doc.mark( "title", "art", art );
    if( art ) {
        std::vector<std::string> lines;
        for( const std::string &line : mmenu_title ) {
            lines.push_back( rml_ui::markup_to_rml( line, c_white ) );
        }
        doc.show( "title", string_join( lines, "<br/>" ) );
    } else {
        doc.show( "title", rml_ui::markup_to_rml( mmenu_title[0], c_light_cyan ) );
    }
    doc.show( "version", rml_ui::markup_to_rml( string_format( _( "Version: %s" ),
              getVersionString() ), c_light_blue ) );

    const bool halloween = get_option<bool>( "SEASONAL_TITLE" ) &&
                           current_holiday == holiday::halloween;
    doc.mark( "spider", "hidden", !halloween );
    doc.mark( "graves", "hidden", !halloween );
    if( halloween ) {
        doc.show( "spider", rml_ui::markup_to_rml( halloween_spider(), c_white ) );
        doc.show( "graves", rml_ui::markup_to_rml( halloween_graves(), c_white ) );
    }

    // The MOTD and credits show as text, scrolled back to the top when it changes
    const main_menu_opts tab = static_cast<main_menu_opts>( sel1 );
    const bool text_tab = tab == main_menu_opts::MOTD || tab == main_menu_opts::CREDITS;
    doc.mark( "text-panel", "hidden", !text_tab );
    if( text_tab ) {
        //~ Message Of The Day
        doc.show( "text-title", rml_ui::markup_to_rml( tab == main_menu_opts::MOTD ? _( "MOTD" ) :
                  _( "Credits" ) ) );
        if( doc.show( "text", rml_ui::markup_to_rml( tab == main_menu_opts::MOTD ? mmenu_motd :
                      mmenu_credits, c_light_gray ) ) ) {
            doc.scroll_text( rml_ui::scroll::top );
        }
    }

    // The other tabs' options, as many as fit around the selection
    const std::vector<std::string> options = sub_options();
    const int option_count = static_cast<int>( options.size() );
    doc.mark( "sub-panel", "hidden", options.empty() );
    if( !options.empty() ) {
        // If sel2 somehow outgrew the options, clamp it back
        sel2 = std::clamp( sel2, 0, option_count - 1 );
        constexpr int max_shown = 12;
        const int shown_count = std::min( option_count, max_shown );
        if( sel2 < sub_opt_off ) {
            sub_opt_off = sel2;
        } else if( sel2 >= sub_opt_off + shown_count ) {
            sub_opt_off = sel2 - shown_count + 1;
        }
        sub_opt_off = std::clamp( sub_opt_off, 0, option_count - shown_count );

        std::string sub;
        for( int i = sub_opt_off; i < sub_opt_off + shown_count; ++i ) {
            sub += i == sel2 ? "<div class=\"option selected\">" : "<div class=\"option\">";
            sub += rml_ui::markup_to_rml( options[i], c_white ) + "</div>";
        }
        doc.show( "sub", sub );
        doc.mark( "sub-position", "hidden", option_count <= shown_count );
        doc.show( "sub-position", option_count <= shown_count ? std::string() :
                  rml_ui::markup_to_rml( string_format( _( "%1$d-%2$d of %3$d" ), sub_opt_off + 1,
                                         sub_opt_off + shown_count, option_count ) ) );
    }

    std::string menu;
    for( size_t i = 0; i < vMenuItems.size(); ++i ) {
        menu += static_cast<int>( i ) == sel1 ? "<div class=\"item selected\">" :
                "<div class=\"item\">";
        menu += rml_ui::markup_to_rml( shortcut_text( c_yellow, vMenuItems[i] ), c_white ) +
                "</div>";
    }
    doc.show( "menu", menu );

    if( tab == main_menu_opts::NEWCHAR && sel2 >= 0 &&
        static_cast<size_t>( sel2 ) < vNewGameHints.size() ) {
        doc.show( "hint", rml_ui::markup_to_rml( vNewGameHints[sel2], c_yellow ) );
    } else {
        doc.show( "hint", rml_ui::markup_to_rml(
                      _( "Bugs?  Suggestions?  Use links in MOTD to report them." ), c_red ) );
    }
    doc.show( "tip", rml_ui::markup_to_rml( string_format( _( "Tip of the day: %s" ), vdaytip ),
                                            c_light_cyan ) );
}

std::vector<std::string> main_menu::load_file( const std::string &path,
        const std::string &alt_text ) const
{
    std::vector<std::string> result;
    read_from_file_optional( path, [&result]( std::istream & fin ) {
        std::string line;
        while( std::getline( fin, line ) ) {
            if( !line.empty() && line[0] == '#' ) {
                continue;
            }
            result.push_back( line );
        }
    } );
    if( result.empty() ) {
        result.push_back( alt_text );
    }
    return result;
}

holiday main_menu::get_holiday_from_time()
{
    return ::get_holiday_from_time( 0, true );
}

void main_menu::init_strings()
{
    // ASCII Art
    mmenu_title = load_file( PATH_INFO::title( current_holiday ), _( "Cataclysm: Dark Days Ahead" ) );
    // MOTD
    auto motd = load_file( PATH_INFO::motd(), _( "No message today." ) );

    mmenu_motd.clear();
    for( const std::string &line : motd ) {
        mmenu_motd += ( line.empty() ? " " : line ) + "\n";
    }
    mmenu_motd = colorize( mmenu_motd, c_light_red );

    // Credits
    mmenu_credits.clear();
    read_from_file_optional( PATH_INFO::credits(), [&]( std::istream & stream ) {
        std::string line;
        while( std::getline( stream, line ) ) {
            if( line[0] != '#' ) {
                mmenu_credits += ( line.empty() ? " " : line ) + "\n";
            }
        }
    } );

    if( mmenu_credits.empty() ) {
        mmenu_credits = _( "No credits information found." );
    }

    // fill menu with translated menu items
    vMenuItems.clear();
    vMenuItems.emplace_back( pgettext( "Main Menu", "<M|m>OTD" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<N|n>ew Game" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "Lo<a|A>d" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<W|w>orld" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "T<u|U>torial Game" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "Se<t|T>tings" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "H<e|E|?>lp" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<C|c>redits" ) );
#if !defined(EMSCRIPTEN)
    vMenuItems.emplace_back( pgettext( "Main Menu", "<Q|q>uit" ) );
#endif

    // new game menu items
    vNewGameSubItems.clear();
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "C<u|U>stom Character" ) );
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "<P|p>reset Character" ) );
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "<R|r>andom Character" ) );
    if( !MAP_SHARING::isSharing() ) { // "Play Now" function doesn't play well together with shared maps
        vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game",
                                       "Play Now!  (<D|d>efault Scenario)" ) );
        vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "Play N<o|O>w!" ) );
    }
    vNewGameHints.clear();
    vNewGameHints.emplace_back(
        _( "Allows you to fully customize scenario, character's profession, stats, traits, skills and other parameters." ) );
    vNewGameHints.emplace_back( _( "Select from one of previously created character templates." ) );
    vNewGameHints.emplace_back(
        _( "Creates random character, but lets you preview the generated character and the scenario and change character and/or scenario if needed." ) );
    vNewGameHints.emplace_back(
        _( "Puts you right in the game, randomly choosing character's traits, profession, skills and other parameters.  Scenario is fixed to Evacuee." ) );
    vNewGameHints.emplace_back(
        _( "Puts you right in the game, randomly choosing scenario and character's traits, profession, skills and other parameters." ) );
    vNewGameHotkeys.clear();
    vNewGameHotkeys.reserve( vNewGameSubItems.size() );
    for( const std::string &item : vNewGameSubItems ) {
        vNewGameHotkeys.push_back( get_hotkeys( item ) );
    }

    // determine hotkeys from translated menu item text
    vMenuHotkeys.clear();
    for( const std::string &item : vMenuItems ) {
        vMenuHotkeys.push_back( get_hotkeys( item ) );
    }

    vWorldSubItems.clear();
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "Sh<o|O>w World Mods" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "Copy World Sett<i|I>ngs" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "Character to Tem<p|P>late" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "Toggle World <C|c>ompression" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "<D|d>elete World" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "<R|r>eset World" ) );

    vWorldHotkeys.clear();
    for( const std::string &item : vWorldSubItems ) {
        vWorldHotkeys.push_back( get_hotkeys( item ) );
    }

    vSettingsSubItems.clear();
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "<O|o>ptions" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Ke<y|Y>bindings" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "A<u|U>topickup" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Sa<f|F>emode" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Colo<r|R>s" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "ImGui <S|s>tyles" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "<I|i>mGui Demo Screen" ) );

    vSettingsHotkeys.clear();
    for( const std::string &item : vSettingsSubItems ) {
        vSettingsHotkeys.push_back( get_hotkeys( item ) );
    }

    try {
        g->load_core_data();
    } catch( const std::exception &err ) {
        debugmsg( err.what() );
        std::exit( 1 );
    }
    vdaytip = SNIPPET.random_from_category( "tip" ).value_or( translation() ).translated();
}

void main_menu::load_char_templates()
{
    templates.clear();

    for( std::string path : get_files_from_path( ".template", PATH_INFO::templatedir(), false,
            true ) ) {
        path.erase( path.find( ".template" ), std::string::npos );
        path.erase( 0, path.find_last_of( "\\/" ) + 1 );
        templates.push_back( path );
    }
    std::sort( templates.begin(), templates.end(), localized_compare );
}

bool main_menu::opening_screen()
{
    // set holiday based on local system time
    current_holiday = get_holiday_from_time();

    if( music::get_music_id() != music::music_id::title ) {
        music::deactivate_music_id_all();
    } else {
        play_music( music::get_music_id_string() );
    }

    world_generator->set_active_world( nullptr );
    world_generator->init();

    init_strings();

    load_char_templates();

    ctxt.register_cardinal();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );

    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );

    // for the menu shortcuts
    ctxt.register_action( "ANY_INPUT" );
    bool start = false;
    bool load_game = false;

    avatar &player_character = get_avatar();
    player_character = avatar();

    // Make [Load Game] the default cursor position if there's game save available
    if( !world_generator->get_all_worlds().empty() ) {
        std::vector<std::string> worlds = world_generator->all_worldnames();
        last_world_pos = world_generator->get_world_index( world_generator->last_world_name );
        if( last_world_pos >= worlds.size() ) {
            last_world_pos = 0;
        }
        sel1 = getopt( main_menu_opts::LOADCHAR );
        sel2 = last_world_pos;
    }

    background_pane background;
    main_menu_rml doc;
    // Filled in now too, for anything shown over it before the loop below
    show_rml( doc );

    if( !queued_world_to_load.empty() ) {
        WORLD *world_to_load{};
        try {
            save_t const &save_to_load = [&]() {
                if( queued_save_id_to_load.empty() ) {
                    world_to_load = world_generator->get_world( queued_world_to_load );
                    const std::vector<save_t> &world_saves = world_to_load->world_saves;
                    if( world_saves.empty() ) {
                        throw false;
                    }
                    return world_saves.front();
                }
                return save_t::from_save_id( queued_save_id_to_load );
            }
            ();
            start = main_menu::load_game( queued_world_to_load, save_to_load );
            queued_world_to_load.clear();
            queued_save_id_to_load.clear();
            if( start ) {
                load_game = true;
            }
        } catch( bool has_save ) {
            load_game = has_save;
            if( world_to_load ) {
                popup( _( "%s has no characters to load!" ), world_to_load->world_name );
            }
        }
    }

#if defined(EMSCRIPTEN)
    EM_ASM( window.dispatchEvent( new Event( 'menuready' ) ); );
#endif

    while( !start ) {
        show_rml( doc );
        ui_manager::redraw();
        std::string action = ctxt.handle_input();
        input_event sInput = ctxt.get_raw_input();

        // check automatic menu shortcuts
        bool match = false;
        for( int i = 0; static_cast<size_t>( i ) < vMenuHotkeys.size() && !match; ++i ) {
            for( const std::string &hotkey : vMenuHotkeys[i] ) {
                if( sInput.text == hotkey && sel1 != i ) {
                    sel1 = i;
                    sel2 = i == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
                    if( i == getopt( main_menu_opts::HELP ) ) {
                        action = "CONFIRM";
                    } else if( i == getopt( main_menu_opts::QUIT ) ) {
                        action = "QUIT";
                    }
                    match = true;
                    break;
                }
            }
        }
        if( sel1 == getopt( main_menu_opts::SETTINGS ) ) {
            for( int i = 0; !match && static_cast<size_t>( i ) < vSettingsSubItems.size(); ++i ) {
                for( const std::string &hotkey : vSettingsHotkeys[i] ) {
                    if( sInput.text == hotkey ) {
                        sel2 = i;
                        action = "CONFIRM";
                        match = true;
                        break;
                    }
                }
            }
        }
        if( sel1 == getopt( main_menu_opts::NEWCHAR ) ) {
            for( int i = 0; !match && static_cast<size_t>( i ) < vNewGameSubItems.size(); ++i ) {
                for( const std::string &hotkey : vNewGameHotkeys[i] ) {
                    if( sInput.text == hotkey ) {
                        sel2 = i;
                        action = "CONFIRM";
                        match = true;
                        break;
                    }
                }
            }
        }

        // Hovering a tab or an option selects it, and clicking picks it
        if( action == "SELECT" || action == "MOUSE_MOVE" ) {
            const int tab = doc.hovered_tab();
            const int option = doc.hovered_option();
            if( tab >= 0 ) {
                if( sel1 != tab ) {
                    sel1 = tab;
                    sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
                    on_move();
                }
                if( action == "SELECT" &&
                    ( sel1 == getopt( main_menu_opts::HELP ) || sel1 == getopt( main_menu_opts::QUIT ) ) ) {
                    action = "CONFIRM";
                }
            } else if( option >= 0 ) {
                if( sel2 != sub_opt_off + option ) {
                    on_move();
                }
                sel2 = sub_opt_off + option;
                if( action == "SELECT" ) {
                    action = "CONFIRM";
                }
            }
        }

        // also check special keys
        if( action == "QUIT" ) {
#if !defined(EMSCRIPTEN)
            if( query_yn( _( "Really quit?" ) ) ) {
                return false;
            }
#endif
        } else if( action == "LEFT" || action == "PREV_TAB" || action == "RIGHT" || action == "NEXT_TAB" ) {
            sel1 = inc_clamp_wrap( sel1, action == "RIGHT" || action == "NEXT_TAB",
                                   static_cast<int>( main_menu_opts::NUM_MENU_OPTS ) );
            sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
            on_move();
        } else if( action == "UP" || action == "DOWN" ||
                   action == "PAGE_UP" || action == "PAGE_DOWN" ||
                   action == "SCROLL_UP" || action == "SCROLL_DOWN" ) {
            int max_item_count = 0;
            int min_item_val = 0;
            main_menu_opts opt = static_cast<main_menu_opts>( sel1 );
            switch( opt ) {
                case main_menu_opts::MOTD:
                case main_menu_opts::CREDITS:
                    // The wheel scrolls the text without help, in RmlUi
                    if( action == "UP" ) {
                        doc.scroll_text( rml_ui::scroll::line_up );
                    } else if( action == "DOWN" ) {
                        doc.scroll_text( rml_ui::scroll::line_down );
                    } else if( action == "PAGE_UP" ) {
                        doc.scroll_text( rml_ui::scroll::page_up );
                    } else if( action == "PAGE_DOWN" ) {
                        doc.scroll_text( rml_ui::scroll::page_down );
                    }
                    break;
                case main_menu_opts::LOADCHAR:
                    max_item_count = world_generator->get_all_worlds().size();
                    break;
                case main_menu_opts::WORLD:
                    // extra 1 = "Create New World"
                    max_item_count = world_generator->get_all_worlds().size() + 1;
                    break;
                case main_menu_opts::NEWCHAR:
                    max_item_count = vNewGameSubItems.size();
                    break;
                case main_menu_opts::SETTINGS:
                    max_item_count = vSettingsSubItems.size();
                    break;
                case main_menu_opts::TUTORIAL:
                case main_menu_opts::HELP:
                case main_menu_opts::QUIT:
                default:
                    break;
            }
            if( max_item_count > 0 ) {
                if( action == "UP" || action == "PAGE_UP" || action == "SCROLL_UP" ) {
                    sel2--;
                    if( sel2 < min_item_val ) {
                        sel2 = max_item_count - 1;
                    }
                } else if( action == "DOWN" || action == "PAGE_DOWN" || action == "SCROLL_DOWN" ) {
                    sel2++;
                    if( sel2 >= max_item_count ) {
                        sel2 = min_item_val;
                    }
                }
                on_move();
            }
        } else if( action == "CONFIRM" ) {
            switch( static_cast<main_menu_opts>( sel1 ) ) {
                case main_menu_opts::HELP:
                    get_help().display_help();
                    break;
                case main_menu_opts::QUIT:
                    return false;
                case main_menu_opts::TUTORIAL:
                    if( MAP_SHARING::isSharing() ) {
                        on_error();
                        popup( _( "Tutorial doesn't work with shared maps." ) );
                    } else {
                        on_out_of_scope cleanup( [&player_character]() {
                            g->gamemode.reset();
                            player_character = avatar();
                            world_generator->set_active_world( nullptr );
                        } );
                        g->gamemode = get_special_game( special_game_type::TUTORIAL );
                        // check world
                        WORLD *world = world_generator->make_new_world( special_game_type::TUTORIAL );
                        if( world == nullptr ) {
                            break;
                        }
                        world->active_mod_order.clear();
                        world->active_mod_order.emplace_back( MOD_INFORMATION_dda );
                        world->active_mod_order.emplace_back( MOD_INFORMATION_dda_tutorial );
                        world_generator->set_active_world( world );
                        try {
                            g->setup();
                        } catch( const std::exception &err ) {
                            debugmsg( "Error: %s", err.what() );
                            break;
                        }
                        if( !g->gamemode->init() ) {
                            break;
                        }
                        cleanup.cancel();
                        start = true;
                        if( g->gametype() == special_game_type::TUTORIAL ) {
                            load_game = true;
                        }
                    }
                    break;
                case main_menu_opts::SETTINGS:
                    if( sel2 == 0 ) {        /// Options
                        get_options().show( false );
                        // The language may have changed- gracefully handle this.
                        init_strings();
                    } else if( sel2 == 1 ) { /// Keybindings
                        input_context ctxt_default = get_default_mode_input_context();
                        ctxt_default.display_menu();
                    } else if( sel2 == 2 ) { /// Autopickup
                        get_auto_pickup().show();
                    } else if( sel2 == 3 ) { /// Safemode
                        get_safemode().show();
                    } else if( sel2 == 4 ) { /// Colors
                        all_colors.show_gui();
                    } else if( sel2 == 5 ) {
                        style_picker picker;
                        picker.show();
                    } else if( sel2 == 6 ) { /// ImGui demo
                        imgui_demo_ui demo;
                        demo.run();
                    }
                    break;
                case main_menu_opts::WORLD:
                    sel2 = std::min<int>( sel2, world_generator->get_all_worlds().size() );
                    world_tab( sel2 > 0 ? world_generator->get_world_name( sel2 - 1 ) : "" );
                    break;
                case main_menu_opts::LOADCHAR:
                    if( static_cast<std::size_t>( sel2 ) < world_generator->get_all_worlds().size() ) {
                        start = load_character_tab( world_generator->get_world_name( sel2 ) );
                        if( start ) {
                            load_game = true;
                        }
                    } else {
                        popup( _( "No world to load." ) );
                    }
                    break;
                case main_menu_opts::NEWCHAR:
                    start = new_character_tab();
                    break;
                case main_menu_opts::MOTD:
                case main_menu_opts::CREDITS:
                default:
                    break;
            }
        }
    }
    if( start && !load_game && get_scenario() ) {
        add_msg( get_scenario()->description( player_character.male ) );

        if( get_option<std::string>( "ETERNAL_WEATHER" ) != "normal" ) {
            if( player_character.posz() >= 0 ) {
                add_msg( _( "You feel as if this %1$s will last forever…" ),
                         get_options().get_option( "ETERNAL_WEATHER" ).getValueName() );
            }
        }
    }
    return true;
}

bool main_menu::new_character_tab()
{
    avatar &pc = get_avatar();
    // Preset character templates
    if( sel2 == 1 ) {
        if( templates.empty() ) {
            on_error();
            popup( _( "No templates found!" ) );
            return false;
        }
        while( true ) {
            uilist mmenu( _( "Choose a preset character template" ), {} );
            mmenu.border_color = c_white;
            int opt_val = 0;
            for( const std::string &tmpl : templates ) {
                mmenu.entries.emplace_back( opt_val++, true, MENU_AUTOASSIGN, tmpl );
            }
            mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
            mmenu.query();
            opt_val = mmenu.ret;
            if( opt_val < 0 || static_cast<size_t>( opt_val ) >= templates.size() ) {
                return false;
            }

            std::string res = query_popup()
                              .context( "LOAD_DELETE_CANCEL" ).default_color( c_white )
                              .message( _( "What to do with template \"%s\"?" ), templates[opt_val] )
                              .option( "LOAD" ).option( "DELETE" ).option( "CANCEL" ).cursor( 0 )
                              .query().action;
            if( res == "DELETE" &&
                query_yn( _( "Are you sure you want to delete %s?" ), templates[opt_val] ) ) {
                const auto path = PATH_INFO::templatedir() + templates[opt_val] + ".template";
                if( !remove_file( path ) ) {
                    popup( _( "Sorry, something went wrong." ) );
                } else {
                    templates.erase( templates.begin() + opt_val );
                }
            } else if( res == "LOAD" ) {
                on_out_of_scope cleanup( [&pc]() {
                    pc = avatar();
                    world_generator->set_active_world( nullptr );
                } );
                g->gamemode = nullptr;
                WORLD *world = world_generator->pick_world();
                if( world == nullptr ) {
                    continue;
                }
                if( !world->world_saves.empty() ) {
                    if( !query_yn(
                            _( "Many game features will not work correctly with multiple characters in the same world.  Create a new character anyway?" ) ) ) {
                        return false;
                    }
                }

                world_generator->set_active_world( world );
                try {
                    g->setup();
                } catch( const std::exception &err ) {
                    debugmsg( "Error: %s", err.what() );
                    continue;
                }
                if( !pc.create( character_type::TEMPLATE, templates[opt_val] ) ) {
                    load_char_templates();
                    MAPBUFFER.clear();
                    overmap_buffer.clear();
                    return false;
                }
                if( !g->start_game() ) {
                    return false;
                }
                cleanup.cancel();
                return true;
            }

            if( templates.empty() ) {
                return false;
            }
        }
    } else { ///Non-template options
        on_out_of_scope cleanup( [&pc]() {
            pc = avatar();
            world_generator->set_active_world( nullptr );
        } );
        g->gamemode = nullptr;
        // First load the mods, this is done by
        // loading the world.
        // Pick a world, suppressing prompts if it's "play now" mode.
        const bool is_play_now = sel2 == 3 || sel2 == 4;
        WORLD *world = world_generator->pick_world( !is_play_now, is_play_now );
        if( world == nullptr ) {
            return false;
        }
        if( !world->world_saves.empty() ) {
            if( !query_yn(
                    _( "Many game features will not work correctly with multiple characters in the same world.  Create a new character anyway?" ) ) ) {
                return false;
            }
        }
        world_generator->set_active_world( world );
        try {
            g->setup();
        } catch( const std::exception &err ) {
            debugmsg( "Error: %s", err.what() );
            return false;
        }
        character_type play_type = character_type::CUSTOM;
        switch( sel2 ) {
            case 0:
                play_type = character_type::CUSTOM;
                break;
            case 2:
                play_type = character_type::RANDOM;
                break;
            case 3:
                play_type = character_type::NOW;
                break;
            case 4:
                play_type = character_type::FULL_RANDOM;
                break;
        }
        if( !pc.create( play_type ) ) {
            load_char_templates();
            MAPBUFFER.clear();
            overmap_buffer.clear();
            return false;
        }

        if( !g->start_game() ) {
            return false;
        }
        cleanup.cancel();
        return true;
    }
    return false;
}

bool main_menu::load_game( std::string const &worldname, save_t const &savegame )
{
    avatar &pc = get_avatar();
    on_out_of_scope cleanup( [&pc]() {
        pc = avatar();
        world_generator->set_active_world( nullptr );
    } );

    g->gamemode = nullptr;
    WORLD *world = world_generator->get_world( worldname );
    world_generator->last_world_name = world->world_name;
    world_generator->last_character_name = savegame.decoded_name();
    world_generator->save_last_world_info();
    world_generator->set_active_world( world );

    try {
        g->setup();
    } catch( const std::exception &err ) {
        debugmsg( "Error: %s", err.what() );
        return false;
    }

    if( g->load( savegame ) ) {
        cleanup.cancel();
        return true;
    }

    return false;
}

static std::optional<std::chrono::seconds> get_playtime_from_save( const WORLD *world,
        const save_t &save )
{
    cata_path playtime_file = world->folder_path() / ( save.base_path() + ".pt" );
    std::optional<std::chrono::seconds> pt_seconds;
    if( file_exist( playtime_file ) ) {
        read_from_file( playtime_file, [&pt_seconds]( std::istream & fin ) {
            if( fin.eof() ) {
                return;
            }
            std::chrono::seconds::rep dur_seconds = 0;
            fin.imbue( std::locale::classic() );
            fin >> dur_seconds;
            pt_seconds = std::chrono::seconds( dur_seconds );
        } );
    }
    return pt_seconds;
}

bool main_menu::load_character_tab( const std::string &worldname )
{
    WORLD *cur_world = world_generator->get_world( worldname );
    savegames = cur_world->world_saves;
    if( MAP_SHARING::isSharing() ) {
        auto new_end = std::remove_if( savegames.begin(), savegames.end(), []( const save_t &str ) {
            return str.decoded_name() != MAP_SHARING::getUsername();
        } );
        savegames.erase( new_end, savegames.end() );
    }

    if( savegames.empty() ) {
        on_error();
        //~ %s = world name
        popup( _( "%s has no characters to load!" ), worldname );
        return false;
    }

    uilist mmenu;
    mmenu.title = string_format( _( "Load character from \"%s\"" ), worldname );
    mmenu.border_color = c_white;
    int opt_val = 0;
    for( const save_t &s : savegames ) {
        std::optional<std::chrono::seconds> playtime = get_playtime_from_save( cur_world, s );
        std::string save_str = s.decoded_name();
        std::string playtime_str;
        if( playtime ) {
            std::chrono::seconds::rep tmp_sec = playtime->count();
            int pt_sec = static_cast<int>( tmp_sec % 60 );
            int pt_min = static_cast<int>( tmp_sec % 3600 ) / 60;
            int pt_hrs = static_cast<int>( tmp_sec / 3600 );
            playtime_str = string_format( "<color_c_light_blue>[%02d:%02d:%02d]</color>",
                                          pt_hrs, pt_min, static_cast<int>( pt_sec ) );
        }
        // TODO: Replace this API to allow adding context without an empty description.
        mmenu.entries.emplace_back( opt_val++, true, MENU_AUTOASSIGN, save_str, "", playtime_str );
    }
    mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
    mmenu.query();
    opt_val = mmenu.ret;
    if( opt_val < 0 || static_cast<size_t>( opt_val ) >= savegames.size() ) {
        return false;
    }

    return main_menu::load_game( worldname, savegames[opt_val] );
}

void main_menu::world_tab( const std::string &worldname )
{
    // Create world
    if( sel2 == 0 ) {
        WORLD *world = world_generator->make_new_world();
        // NOLINTNEXTLINE(cata-use-localized-sorting)
        if( world != nullptr && world->world_name < world_generator->all_worldnames()[last_world_pos] ) {
            last_world_pos++;
        }
        return;
    }

    uilist mmenu( string_format( _( "Manage world \"%s\"" ), worldname ), {} );
    mmenu.border_color = c_white;
    int opt_val = 0;
    std::array<char, 6> hotkeys = { 'm', 's', 't', 'c', 'd', 'r' };
    for( const std::string &it : vWorldSubItems ) {
        mmenu.entries.emplace_back( opt_val, true, hotkeys[opt_val],
                                    remove_color_tags( shortcut_text( c_white, it ) ) );
        ++opt_val;
    }
    mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
    mmenu.query();
    opt_val = mmenu.ret;
    if( opt_val < 0 || static_cast<size_t>( opt_val ) >= vWorldSubItems.size() ) {
        return;
    }

    auto clear_world = [this, &worldname]( bool do_delete ) {
        // NOLINTNEXTLINE(cata-use-localized-sorting)
        if( last_world_pos > 0 && worldname <= world_generator->all_worldnames()[last_world_pos] ) {
            last_world_pos--;
        }
        world_generator->delete_world( worldname, do_delete );
        savegames.clear();
        MAPBUFFER.clear();
        overmap_buffer.clear();
        if( do_delete ) {
            sel2 = 0; // reset to create world selection
        }
    };

    switch( opt_val ) {
        case 0: // Active World Mods
            world_generator->show_active_world_mods(
                world_generator->get_world( worldname )->active_mod_order );
            break;
        case 1: // Copy World settings
            world_generator->make_new_world( true, worldname );
            break;
        case 2: // Character to Template
            if( load_character_tab( worldname ) ) {
                avatar &pc = get_avatar();
                pc.setID( character_id(), true );
                pc.reset_all_missions();
                pc.character_to_template( pc.name );
                pc = avatar();
                MAPBUFFER.clear();
                overmap_buffer.clear();
                load_char_templates();
            }
            break;
        case 3: // Toggle save compression
            if( world_generator->get_world( worldname )->has_compression_enabled() ) {
                if( query_yn( _( "Disable save compression?" ) ) ) {
                    world_generator->get_world( worldname )->set_compression_enabled( false );
                }
            } else {
                if( query_yn( _( "Enable save compression?" ) ) ) {
                    world_generator->get_world( worldname )->set_compression_enabled( true );
                }
            }
            break;
        case 4: // Delete World
            if( query_yn( _( "Delete the world and all saves within?" ) ) ) {
                clear_world( true );
            }
            break;
        case 5: // Reset World
            if( query_yn( _( "Remove all saves and regenerate world?" ) ) ) {
                clear_world( false );
            }
            break;
        default:
            break;
    }
}

std::string main_menu::halloween_spider()
{
    static const std::string spider =
        "\\ \\ \\/ / / / / / / /\n"
        " \\ \\/\\/ / / / / / /\n"
        "\\ \\/__\\/ / / / / /\n"
        " \\/____\\/ / / / /\n"
        "\\/______\\/ / / /\n"
        "/________\\/ / /\n"
        "__________\\/ /\n"
        "___________\\/\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "        |\n"
        "  , .   |  . ,\n" // NOLINT(cata-text-style)
        "  { | ,--, | }\n" // NOLINT(cata-text-style)
        "   \\\\{~~~~}//\n"
        "  /_/ {<color_c_red>..</color>} \\_\\\n"
        "  { {      } }\n"
        "  , ,      , ."; // NOLINT(cata-text-style)

    return spider;
}

std::string main_menu::halloween_graves()
{
    static const std::string graves =
        "                    _\n"
        "        -q       __(\")_\n"
        "         (\\      \\_  _/\n"
        " .-.   .-''\"'.     |/\n" // NOLINT(cata-text-style)
        "|RIP|  | RIP |   .-.\n"
        "|   |  |     |  |RIP|\n"
        ";   ;  |     | ,'---',"; // NOLINT(cata-text-style)

    return graves;
}

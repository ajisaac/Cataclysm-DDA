#include "worldfactory.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <exception>
#include <iterator>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

#include "cata_utility.h"
#include "catacharset.h"
#include "char_validity_check.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "enums.h"
#include "filesystem.h"
#include "input.h"
#include "input_context.h"
#include "input_popup.h"
#include "json.h"
#include "json_loader.h"
#include "mod_manager.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "popup.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "sounds.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "text_snippets.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"
#include "zzip.h"
#include "zzip_stack.h"

// single instance of world generator
std::unique_ptr<worldfactory> world_generator;

/**
  * Max utf-8 character worldname length.
  * 0 index is inclusive.
  */
static const int max_worldname_len = 32;

namespace
{
/**
 * A world creation or selection window, from data/raw/rmlui/worldgen.rml;
 * its screen fills it on every redraw.
 */
class worldgen_window : public rml_ui::window
{
    public:
        worldgen_window( const std::string &title,
                         const std::function<void( worldgen_window & )> &update ) :
            rml_ui::window( title, "worldgen.rml" ), update( update ) {}

        void reveal( const std::string &id ) {
            scroll_into_view( id );
        }

    protected:
        void on_redraw() override {
            update( *this );
        }

    private:
        std::function<void( worldgen_window & )> update;
};

/** The width the world creation windows take, as the terminal ones did. */
std::string worldgen_width()
{
    return rml_ui::term_width( std::max( FULL_SCREEN_WIDTH, TERMX / 2 ) );
}
} // namespace

save_t::save_t( const std::string &name ): name( name ) {}

std::string save_t::decoded_name() const
{
    return name;
}

std::string save_t::base_path() const
{
    return base64_encode( name );
}

save_t save_t::from_save_id( const std::string &save_id )
{
    return save_t( save_id );
}

save_t save_t::from_base_path( const std::string_view base_path )
{
    return save_t( base64_decode( base_path ) );
}

static std::string get_next_valid_worldname()
{
    return SNIPPET.expand( "<world_name>" );
}

WORLD::WORLD()
{
    world_name = get_next_valid_worldname();
    WORLD_OPTIONS = get_options().get_world_defaults();

    world_saves.clear();
    active_mod_order = world_generator->get_mod_manager().get_default_mods();
}

WORLD::WORLD( const std::string &name )
{
    world_name = name;
    WORLD_OPTIONS = get_options().get_world_defaults();

    world_saves.clear();
    active_mod_order = world_generator->get_mod_manager().get_default_mods();
}

void WORLD::COPY_WORLD( const WORLD *world_to_copy )
{
    world_name = world_to_copy->world_name + "_copy";
    WORLD_OPTIONS = world_to_copy->WORLD_OPTIONS;
    active_mod_order = world_to_copy->active_mod_order;
    is_compressed = world_to_copy->is_compressed;
}

cata_path WORLD::folder_path() const
{
    return PATH_INFO::savedir_path() / world_name;
}

bool WORLD::save_exists( const save_t &name ) const
{
    return std::find( world_saves.begin(), world_saves.end(), name ) != world_saves.end();
}

void WORLD::add_save( const save_t &name )
{
    if( !save_exists( name ) ) {
        world_saves.push_back( name );
    }
}

worldfactory::worldfactory()
    : active_world( nullptr )
    , mman_ui( *mman )
{
    // prepare tab display order
    tabs.emplace_back(
    [this]( WORLD * w, bool b ) {
        return show_worldgen_tab_modselection( w, b );
    } );
    tabs.emplace_back(
    [this]( WORLD * w, bool b ) {
        return show_worldgen_tab_options( w, b );
    } );
}

worldfactory::~worldfactory() = default;

WORLD *worldfactory::add_world( std::unique_ptr<WORLD> retworld )
{
    if( !retworld->save() ) {
        return nullptr;
    }
    if( get_option<bool>( "WORLD_COMPRESSION2" ) ) {
        cata_path dictionary_folder = PATH_INFO::compression_folder_path();
        cata_path maps_dict = dictionary_folder / "maps.dict";
        cata_path mmr_dict = dictionary_folder / "mmr.dict";
        cata_path overmaps_dict = dictionary_folder / "overmaps.dict";

        if( !copy_file( maps_dict, retworld->folder_path() / "maps.dict" ) ||
            !copy_file( mmr_dict, retworld->folder_path() / "mmr.dict" ) ||
            !copy_file( overmaps_dict, retworld->folder_path() / "overmaps.dict" ) ) {
            return nullptr;
        }
    }
    return ( all_worlds[ retworld->world_name ] = std::move( retworld ) ).get();
}

WORLD *worldfactory::make_new_world( const std::vector<mod_id> &mods )
{
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>();
    retworld->active_mod_order = mods;
    retworld->create_timestamp();
    return add_world( std::move( retworld ) );
}

WORLD *worldfactory::make_new_world( const std::string &name, const std::vector<mod_id> &mods )
{
    if( !is_lexically_valid( std::filesystem::u8path( name ) ) ) {
        return nullptr;
    }
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>( name );
    retworld->active_mod_order = mods;
    retworld->create_timestamp();
    return add_world( std::move( retworld ) );
}

WORLD *worldfactory::make_new_world( bool show_prompt, const std::string &world_to_copy )
{
    // World to return after generating
    std::unique_ptr<WORLD> retworld = std::make_unique<WORLD>();

    if( !world_to_copy.empty() ) {
        retworld->COPY_WORLD( world_generator->get_world( world_to_copy ) );
    }

    if( show_prompt ) {
        if( world_to_copy.empty() ) {
            if( show_worldgen_basic( retworld.get() ) < 0 ) {
                return nullptr;
            }
        } else if( show_worldgen_advanced( retworld.get() ) < 0 ) {
            return nullptr;
        }
    }

    for( const mod_id &checked_mod : retworld->active_mod_order ) {
        std::optional<mod_id> conflict = mman_ui->find_mod_conflict( checked_mod,
                                         retworld->active_mod_order );
        if( conflict ) {
            popup( _( "Unable to create new world: %s conflicts with %s." ),
                   checked_mod->name(), ( *conflict )->name() );
            return nullptr;
        }
    }

    retworld->create_timestamp();

    return add_world( std::move( retworld ) );
}

static std::optional<std::string> prompt_world_name( const std::string &title,
        const std::string &cur_worldname )
{
    text_input_popup popup( 50, cur_worldname );
    popup.set_max_input_length( max_worldname_len );
    popup.set_label( title );

    input_context ctxt( "STRING_INPUT" );
    popup.set_description( string_format(
                               _( "Press [<color_c_yellow>%s</color>] to randomize the world name." ),
                               ctxt.get_desc( "PICK_RANDOM_WORLDNAME", 1U ) ) );

    popup.add_callback( callback_input{ "PICK_RANDOM_WORLDNAME" }, [&popup]() {
        popup.set_text( get_next_valid_worldname() );
        return true;
    } );
    std::string message = popup.query();
    return message;
}

int worldfactory::show_worldgen_advanced( WORLD *world )
{
    // Each tab is a window of its own, showing the tabs across its top
    int curtab = 0;

    const size_t numtabs = tabs.size();
    bool done = false;
    while( !done ) {
        while( static_cast<size_t>( curtab ) < numtabs ) {
            curtab += tabs[curtab]( world, true );
        }
        if( curtab >= 0 ) {
            std::optional<std::string> ret = prompt_world_name( _( "Choose a new name for this world." ),
                                             world->world_name );
            if( !ret.has_value() ) {
                // return to settings tab
                curtab = 1;
            } else if( ret.value().empty() ) {
                // no name entered
                if( query_yn( _( "World name is empty.  Randomize the name?" ) ) ) {
                    world->world_name = pick_random_name();
                }
            } else {
                // done, generate world
                world->world_name = ret.value();
                done = true;
            }
        } else if( curtab < 0 ) {
            break;
        }
    }
    return curtab;
}

WORLD *worldfactory::make_new_world( special_game_type special_type )
{
    std::string worldname;
    switch( special_type ) {
        case special_game_type::TUTORIAL:
            worldname = "TUTORIAL";
            break;
        default:
            return nullptr;
    }

    // Look through all worlds and see if a world named worldname already exists. If so, then just return it instead of
    // making a new world.
    if( has_world( worldname ) ) {
        return all_worlds[worldname].get();
    }

    std::unique_ptr<WORLD> special_world = std::make_unique<WORLD>();
    special_world->world_name = worldname;

    special_world->WORLD_OPTIONS["WORLD_END"].setValue( "delete" );

    special_world->create_timestamp();

    if( !special_world->save() ) {
        return nullptr;
    }

    return ( all_worlds[worldname] = std::move( special_world ) ).get();
}

void worldfactory::set_active_world( WORLD *world )
{
    world_generator->active_world = world;
    if( world ) {
        get_options().set_world_options( &world->WORLD_OPTIONS );
    } else {
        get_options().set_world_options( nullptr );
    }
}

bool WORLD::save() const
{
    if( !assure_dir_exist( folder_path() ) ) {
        debugmsg( "Unable to create or open world[%s] directory for saving", world_name );
        DebugLog( D_ERROR, DC_ALL ) << "Unable to create or open world[" << world_name <<
                                    "] directory for saving";
        return false;
    }

    if( !save_timestamp() ) {
        return false;
    }

    const cata_path savefile = folder_path() / PATH_INFO::worldoptions();
    const bool saved = write_to_file( savefile, [&]( std::ostream & fout ) {
        JsonOut jout( fout );

        jout.start_array();

        for( const auto &elem : WORLD_OPTIONS ) {
            // Skip hidden option because it is set by mod and should not be saved
            if( !elem.second.getDefaultText().empty() ) {
                jout.start_object();

                jout.member( "info", elem.second.getTooltip() );
                jout.member( "default", elem.second.getDefaultText( false ) );
                jout.member( "name", elem.first );
                jout.member( "value", elem.second.getValue( true ) );

                jout.end_object();
            }
        }

        jout.end_array();
    }, _( "world data" ) );
    if( !saved ) {
        return false;
    }

    world_generator->get_mod_manager().save_mods_list( this );
    return true;
}

void worldfactory::init()
{
    load_last_world_info();

    all_worlds.clear();

    // The validity of a world is determined by the existence of any
    // option files or the master save file.
    static const auto is_save_dir = []( const std::string & maybe_save_dir ) {
        return file_exist( maybe_save_dir + "/" + PATH_INFO::worldoptions() ) ||
               file_exist( maybe_save_dir + "/" + SAVE_MASTER );
    };

    const auto add_existing_world = [&]( const std::string & world_dir ) {
        // get the save files
        auto world_sav_files = get_files_from_path( SAVE_EXTENSION, world_dir, false );
        // split the save file names between the directory and the extension
        for( auto &world_sav_file : world_sav_files ) {
            const size_t start_of_file = world_dir.size() + 1;
            size_t save_index = world_sav_file.find( SAVE_EXTENSION, start_of_file );
            world_sav_file = world_sav_file.substr( start_of_file,
                                                    save_index - start_of_file );
        }

        // the directory name is the name of the world
        std::string worldname;
        size_t name_index = world_dir.find_last_of( "/\\" );
        worldname = world_dir.substr( name_index + 1 );

        // create and store the world
        all_worlds[worldname] = std::make_unique<WORLD>();
        // give the world a name
        all_worlds[worldname]->world_name = worldname;

        bool save = false;

        // load timestamp or create for legacy world
        if( !all_worlds[worldname]->load_timestamp() ) {
            all_worlds[worldname]->create_timestamp();
            save = true;
        }

        // add sav files
        for( auto &world_sav_file : world_sav_files ) {
            all_worlds[worldname]->world_saves.push_back( save_t::from_base_path( world_sav_file ) );
        }
        mman->load_mods_list( all_worlds[worldname].get() );

        // load options into the world
        if( !all_worlds[worldname]->load_options() ) {
            all_worlds[worldname]->WORLD_OPTIONS = get_options().get_world_defaults();
            all_worlds[worldname]->WORLD_OPTIONS["WORLD_END"].setValue( "delete" );
            save = true;
        }

        if( save ) {
            all_worlds[worldname]->save();
        }
    };

    // This returns files as well, but they are going to be discarded later as
    // we look for files *within* these dirs. If it's a file, there won't be
    // any files inside it and is_save_dir will return false.
    for( const std::string &dir : get_files_from_path( "", PATH_INFO::savedir(), false ) ) {
        if( !is_save_dir( dir ) ) {
            continue;
        }
        add_existing_world( dir );
    }
}

bool worldfactory::has_world( const std::string &name ) const
{
    return all_worlds.count( name ) > 0;
}

const std::map<std::string, std::unique_ptr<WORLD>> &worldfactory::get_all_worlds() const
{
    return all_worlds;
}

std::vector<std::string> worldfactory::all_worldnames() const
{
    std::vector<std::string> result;
    result.reserve( all_worlds.size() );
    for( const auto &elem : all_worlds ) {
        result.push_back( elem.first );
    }
    return result;
}

WORLD *worldfactory::pick_world( bool show_prompt, bool empty_only )
{
    std::vector<std::string> world_names = all_worldnames();

    // Filter out special worlds (TUTORIAL | DEFENSE) from world_names.
    for( std::vector<std::string>::iterator it = world_names.begin(); it != world_names.end(); ) {
        if( *it == "TUTORIAL" || *it == "DEFENSE" ||
            ( empty_only && !get_world( *it )->world_saves.empty() ) ) {
            it = world_names.erase( it );
        } else {
            ++it;
        }
    }
    // If there is only one world to pick from, autoreturn it.
    if( world_names.size() == 1 ) {
        return get_world( world_names[0] );
    }
    // If there are no worlds to pick from, immediately try to make one.
    else if( world_names.empty() ) {
        return make_new_world( show_prompt );
    }
    // If we're skipping prompts, return the world with 0 save if there is one
    else if( !show_prompt ) {
        for( const std::string &name : world_names ) {
            if( get_world( name )->world_saves.empty() ) {
                return get_world( name );
            }
        }
        // if there isn't any, adhere to old logic: return the alphabetically first one
        return get_world( world_names[0] );
    }

    int sel = 0;
    // The selection as last shown, to keep it in view
    int shown_sel = -1;

    const auto on_move = []( bool is_error ) {
        sfx::play_variant_sound( is_error ? "menu_error" : "menu_move", "default", 100 );
    };

    worldgen_window window( _( "World selection" ), [&]( worldgen_window & win ) {
        win.set_size( worldgen_width(), "96%" );
        win.set_header( "<div class=\"muted\">" + rml_ui::escape_rml(
                            _( "Pick a world to enter game" ) ) + "</div>" );
        std::string rml;
        for( size_t i = 0; i < world_names.size(); ++i ) {
            const std::string &world_name = world_names[i];
            const size_t saves_num = get_world( world_name )->world_saves.size();
            rml += string_format( "<div id=\"wg-world-%d\" class=\"item line wg-row%s\" data-row=\"%d\">",
                                  static_cast<int>( i ), static_cast<int>( i ) == sel ? " selected" : "", static_cast<int>( i ) ) +
                   "<span class=\"wg-num\">" + std::to_string( i + 1 ) + "</span>" +
                   rml_ui::colored_rml( string_format( "%s (%lu)", world_name, saves_num ), c_white ) + "</div>";
        }
        win.set_body( rml );
        if( sel != shown_sel ) {
            shown_sel = sel;
            win.reveal( "wg-world-" + std::to_string( sel ) );
        }
    } );

    input_context ctxt( "PICK_WORLD_DIALOG" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "CONFIRM" );
    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    while( true ) {
        ui_manager::redraw();

        std::string action = ctxt.handle_input();
        const int recmax = static_cast<int>( world_names.size() );
        const int scroll_rate = recmax > 20 ? 10 : 3;

        // handle mouse click
        if( action == "SELECT" ) {
            // the close button
            const std::string clicked = window.resolve_click( action );
            if( clicked != action ) {
                action = clicked;
            }
        }
        if( action == "SELECT" || action == "MOUSE_MOVE" ) {
            const int row = window.hovered_index( "data-row" );
            if( row >= 0 && row < recmax ) {
                if( sel != row ) {
                    on_move( false );
                    sel = row;
                }
                if( action == "SELECT" ) {
                    action = "CONFIRM";
                }
            }
        }

        if( action == "QUIT" ) {
            break;
        } else if( navigate_ui_list( action, sel, scroll_rate, recmax, true ) ) {
            on_move( recmax < 2 );
        } else if( action == "NEXT_TAB" || action == "PREV_TAB" ) {
            // A page at a time, as the pages of the list were
            sel = clamp( sel + ( action == "NEXT_TAB" ? scroll_rate : -scroll_rate ), 0, recmax - 1 );
            on_move( false );
        } else if( action == "CONFIRM" ) {
            return get_world( world_names[sel] );
        }
    }

    return nullptr;
}

void worldfactory::remove_world( const std::string &worldname )
{
    auto it = all_worlds.find( worldname );
    if( it != all_worlds.end() ) {
        WORLD *wptr = it->second.get();
        if( active_world == wptr ) {
            get_options().set_world_options( nullptr );
            active_world = nullptr;
        }
        all_worlds.erase( it );
    }
}

void worldfactory::load_last_world_info()
{
    cata_path lastworld_path = PATH_INFO::lastworld();
    if( !file_exist( lastworld_path ) ) {
        return;
    }

    try {
        JsonValue jsin = json_loader::from_path( lastworld_path );
        JsonObject data = jsin.get_object();
        last_world_name = data.get_string( "world_name" );
        last_character_name = data.get_string( "character_name" );
    } catch( std::exception const &e ) {
        DebugLog( D_INFO, DC_ALL ) <<  e.what();
        last_world_name = std::string{};
        last_character_name = std::string{};
    }
}

void worldfactory::save_last_world_info() const
{
    write_to_file( PATH_INFO::lastworld(), [&]( std::ostream & file ) {
        JsonOut jsout( file, true );
        jsout.start_object();
        jsout.member( "world_name", last_world_name );
        jsout.member( "character_name", last_character_name );
        jsout.end_object();
    }, _( "last world info" ) );
}

std::string worldfactory::pick_random_name()
{
    // TODO: add some random worldname parameters to name generator
    return get_next_valid_worldname();
}

int worldfactory::show_worldgen_tab_options( WORLD *world, bool with_tabs )
{
    get_options().set_world_options( &world->WORLD_OPTIONS );
    const std::string action = get_options().show( false, true, with_tabs );
    get_options().set_world_options( nullptr );
    if( action == "PREV_TAB" ) {
        return -1;

    } else if( action == "NEXT_TAB" ) {
        return 1;

    } else if( action == "QUIT" ) {
        return -999;
    }

    return 0;
}

std::string worldfactory::mod_list_rml( size_t cursor, const std::vector<mod_id> &mods,
        bool focused, const std::string &text_if_empty, const std::string &id_prefix, bool show_shift,
        const std::vector<mod_id> &potential_conflicts ) const
{
    if( mods.empty() ) {
        return "<div class=\"wg-empty\">" + rml_ui::colored_rml( text_if_empty, c_red ) + "</div>";
    }

    std::string rml;
    std::string last_category_name;
    for( size_t i = 0; i < mods.size(); ++i ) {
        const mod_id &mod_entry_id = mods[i];

        // a header for each category
        std::string category_name = _( "MISSING MODS" );
        if( mod_entry_id.is_valid() ) {
            category_name = mod_entry_id->obsolete ? _( "OBSOLETE MODS" ) :
                            mod_entry_id->category.second.translated();
        }
        if( last_category_name != category_name ) {
            last_category_name = category_name;
            rml += "<div class=\"line wg-category\">" + rml_ui::colored_rml( category_name,
                    c_magenta ) + "</div>";
        }

        std::string mod_entry_name;
        nc_color mod_entry_color = c_white;
        if( mod_entry_id.is_valid() ) {
            const MOD_INFORMATION &mod = *mod_entry_id;
            mod_entry_name = mod.name();
            if( mod.obsolete ) {
                mod_entry_color = c_dark_gray;
            }
        } else {
            mod_entry_color = c_light_red;
            mod_entry_name = _( "N/A" );

        }
        mod_entry_name += string_format( _( " [%s]" ), mod_entry_id.str() );
        std::optional<mod_id> conflict = mman_ui->find_mod_conflict( mod_entry_id,
                                         potential_conflicts );
        if( conflict ) {
            mod_entry_name += string_format( _( " --- conflicts with %s" ),
                                             ( *conflict )->name() );
            mod_entry_color = c_pink;
        }

        // which way the mod can move in the load order
        std::string shift_rml;
        if( show_shift ) {
            std::string shift_display;
            if( mman_ui->can_shift_up( i, mods ) ) {
                shift_display += "<color_blue>+</color> ";
            } else {
                shift_display += "<color_dark_gray>+</color> ";
            }

            if( mman_ui->can_shift_down( i, mods ) ) {
                shift_display += "<color_blue>-</color>";
            } else {
                shift_display += "<color_dark_gray>-</color>";
            }
            shift_rml = "<span class=\"wg-shift\">" + rml_ui::markup_to_rml( shift_display ) + "</span>";
        }

        std::string row_class;
        if( i == cursor ) {
            row_class = focused ? " selected" : " wg-cursor";
        }
        rml += string_format( "<div id=\"%s%d\" class=\"item line wg-mod%s\" data-mod=\"%d\">", id_prefix,
                              static_cast<int>( i ), row_class, static_cast<int>( i ) ) + shift_rml +
               "<span class=\"wg-mod-name\">" + rml_ui::colored_rml( mod_entry_name, mod_entry_color ) +
               "</span></div>";
    }
    return rml;
}

void worldfactory::show_active_world_mods( const std::vector<mod_id> &world_mods )
{
    int cursor = 0;
    // The cursor as last shown, to keep it in view
    int shown_cursor = -1;
    const size_t num_mods = world_mods.size();

    input_context ctxt( "DEFAULT" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SEC_SELECT" );

    worldgen_window window( _( "Active world mods" ), [&]( worldgen_window & win ) {
        win.set_size( worldgen_width(), "80%" );
        win.set_body( mod_list_rml( static_cast<size_t>( cursor ), world_mods, true,
                                    _( "--NO ACTIVE MODS--" ), "wg-mod-" ) );
        if( cursor != shown_cursor ) {
            shown_cursor = cursor;
            win.reveal( "wg-mod-" + std::to_string( cursor ) );
        }
    } );

    while( true ) {
        ui_manager::redraw();

        const std::string action = ctxt.handle_input();
        const int recmax = static_cast<int>( num_mods );
        const int scroll_rate = recmax > 20 ? 10 : 3;

        if( !world_mods.empty() && action == "MOUSE_MOVE" ) {
            const int row = window.hovered_index( "data-mod" );
            if( row >= 0 && row < recmax ) {
                cursor = row;
            }
        }

        if( navigate_ui_list( action, cursor, scroll_rate, recmax, true ) ) {
            // The window scrolls to it
        } else if( action == "QUIT" || action == "CONFIRM" ||
                   action == "SELECT" || action == "SEC_SELECT" ) {
            break;
        }
    }
}

int worldfactory::show_worldgen_tab_modselection( WORLD *world, bool with_tabs )
{
    // Use active_mod_order of the world,
    // saves us from writing 'world->active_mod_order' all the time.
    std::vector<mod_id> &active_mod_order = world->active_mod_order;
    {
        std::vector<mod_id> tmp_mod_order;
        // clear active_mod_order and re-add all the mods, his ensures
        // that changes (like changing dependencies) get updated
        tmp_mod_order.swap( active_mod_order );
        for( auto &elem : tmp_mod_order ) {
            mman_ui->try_add( elem, active_mod_order );
        }
    }

    input_context ctxt( "MODMANAGER_DIALOG" );
    ctxt.register_navigate_ui_list();
    ctxt.register_action( "LEFT", to_translation( "Switch to other list" ) );
    ctxt.register_action( "RIGHT", to_translation( "Switch to other list" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "NEXT_CATEGORY_TAB" );
    ctxt.register_action( "PREV_CATEGORY_TAB" );
    if( with_tabs ) {
        ctxt.register_action( "NEXT_TAB" );
        ctxt.register_action( "PREV_TAB" );
    }
    ctxt.register_action( "CONFIRM", to_translation( "Activate / deactivate mod" ) );
    ctxt.register_action( "MOVE_MOD_UP" );
    ctxt.register_action( "MOVE_MOD_DOWN" );
    ctxt.register_action( "SAVE_DEFAULT_MODS" );
    ctxt.register_action( "VIEW_MOD_DESCRIPTION" );
    ctxt.register_action( "FILTER" );
    // for mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    std::string current_filter = "init me!";
    // Whether the filter field is taking typing
    bool filtering = false;

    std::vector<std::string> headers;
    headers.emplace_back( _( "Mod List" ) );
    headers.emplace_back( _( "Mod Load Order" ) );

    size_t active_header = 0;
    std::array<size_t, 2> cursel = {0, 0};
    size_t iCurrentTab = 0;
    // The selected row as last shown, to keep it in view
    std::string shown_row;

    struct mod_tab {
        std::string id;
        std::vector<mod_id> mods;
        std::vector<mod_id> mods_unfiltered;

        explicit mod_tab( std::string id, std::vector<mod_id> mods, std::vector<mod_id> mods_unfiltered ) :
            id( std::move( id ) ), mods( std::move( mods ) ), mods_unfiltered( std::move( mods_unfiltered ) ) {}
    };
    std::vector<mod_tab> all_tabs;

    for( const std::pair<std::string, translation> &tab : get_mod_list_tabs() ) {
        all_tabs.emplace_back( tab.first, std::vector<mod_id> {}, std::vector<mod_id> {} );
    }

    const std::map<std::string, std::string> &cat_tab_map = get_mod_list_cat_tab();
    for( const mod_id &mod : mman->get_usable_mods() ) {
        int cat_idx = mod->category.first;
        const std::string &cat_id = get_mod_list_categories()[cat_idx].first;

        std::string dest_tab = "tab_default";
        const auto iter = cat_tab_map.find( cat_id );
        if( iter != cat_tab_map.end() ) {
            dest_tab = iter->second;
        }

        for( mod_tab &tab : all_tabs ) {
            if( tab.id == dest_tab ) {
                tab.mods_unfiltered.push_back( mod );
                break;
            }
        }
    }

    // Helper function for determining the currently selected mod
    const auto get_selected_mod = [&]() -> const MOD_INFORMATION* {
        const std::vector<mod_id> &current_tab_mods = all_tabs[iCurrentTab].mods;
        if( current_tab_mods.empty() )
        {
            return nullptr;
        } else if( active_header == 0 )
        {
            if( !current_tab_mods.empty() ) {
                return &current_tab_mods[cursel[0]].obj();
            }
            return nullptr;
        } else if( !active_mod_order.empty() )
        {
            return &active_mod_order[cursel[1]].obj();
        }
        return nullptr;
    };

    // Helper function for applying filter to mod tabs
    const auto apply_filter = [&]( const std::string & filter_str ) {
        if( filter_str == current_filter ) {
            return;
        }
        const MOD_INFORMATION *selected_mod = nullptr;
        if( active_header == 0 ) {
            selected_mod = get_selected_mod();
        }
        for( mod_tab &tab : all_tabs ) {
            if( filter_str.empty() ) {
                tab.mods = tab.mods_unfiltered;
            } else {
                tab.mods.clear();
                for( const mod_id &mod : tab.mods_unfiltered ) {
                    std::string name = ( *mod ).name();
                    if( lcmatch( name, filter_str ) ) {
                        tab.mods.push_back( mod );
                    }
                }
            }
        }
        cursel[0] = 0;
        // Try to restore cursor position
        const std::vector<mod_id> &curr_tab = all_tabs[iCurrentTab].mods;
        for( size_t i = 0; i < curr_tab.size(); i++ ) {
            if( &*curr_tab[i] == selected_mod ) {
                cursel[0] = i;
                break;
            }
        }
        current_filter = filter_str;
    };
    apply_filter( "" );

    worldgen_window window( _( "World Mods" ), [&]( worldgen_window & win ) {
        win.set_size( worldgen_width(), "96%" );
        win.set_body_scrolls( false );
        win.set_tabs( with_tabs ? worldgen_tab_names() : std::vector<std::string>(), 0 );

        // The mod categories, each a button
        std::string categories;
        for( size_t i = 0; i < get_mod_list_tabs().size(); i++ ) {
            categories += string_format( "<span class=\"wg-tab%s\" data-category=\"%d\">",
                                         iCurrentTab == i ? " active" : "", static_cast<int>( i ) ) +
                          rml_ui::escape_rml( get_mod_list_tabs()[i].second.translated() ) + "</span>";
        }
        win.set_header( "<div class=\"wg-tabs\">" + categories + "</div>" );

        // The mods to pick from beside the load order, then the selected mod's description
        const mod_tab &current_tab = all_tabs[iCurrentTab];
        const char *msg = current_tab.mods_unfiltered.empty() ?
                          _( "--NO AVAILABLE MODS--" ) : _( "--NO RESULTS FOUND--" );
        std::string body = "<div class=\"panes\">";
        for( size_t i = 0; i < headers.size(); ++i ) {
            body += string_format( "<div class=\"pane wg-pane%s\" data-pane=\"%d\">",
                                   active_header == i ? " active" : "", static_cast<int>( i ) ) +
                    "<div class=\"wg-pane-head\">" + rml_ui::colored_rml( headers[i], c_cyan ) + "</div>" +
                    ( i == 0 ?
                      mod_list_rml( cursel[0], current_tab.mods, active_header == 0, msg, "wg-list-", false,
                                    active_mod_order ) :
                      mod_list_rml( cursel[1], active_mod_order, active_header == 1, _( "--NO ACTIVE MODS--" ),
                                    "wg-active-", true ) ) + "</div>";
        }
        body += "</div><div id=\"wg-desc\">";
        if( const MOD_INFORMATION *selmod = get_selected_mod() ) {
            body += rml_ui::markup_to_rml( mman_ui->get_information( selmod ), c_white );
        }
        win.set_body( body + "</div>" );

        // The filter, and how to get around
        std::string footer;
        if( !filtering ) {
            const char *help = current_filter.empty() ? _( "[%s] Filter" ) : _( "[%s] Filter: " );
            footer += "<div>" + rml_ui::button_rml( "FILTER", rml_ui::colored_rml( string_format( help,
                                                    ctxt.get_desc( "FILTER" ) ), c_light_gray ) +
                                                    rml_ui::colored_rml( current_filter, c_white ) ) + "</div>";
        }
        footer += "<div>" + rml_ui::markup_to_rml( string_format(
                      _( "[<color_yellow>%s</color>] = save <color_cyan>Mod Load Order</color> as default <color_red>|</color> "
                         "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch Main-Tab <color_red>|</color> "
                         "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch "
                         "<color_cyan>Mod List</color> and <color_cyan>Mod Load Order</color> <color_red>|</color> "
                         "[<color_yellow>%s</color>/<color_yellow>%s</color>] = switch <color_cyan>Mod List</color> Tab <color_red>|</color> "
                         "[<color_yellow>%s</color>] = keybindings" ),
                      ctxt.get_desc( "SAVE_DEFAULT_MODS" ),
                      ctxt.get_desc( "PREV_TAB" ),
                      ctxt.get_desc( "NEXT_TAB" ),
                      ctxt.get_desc( "LEFT" ),
                      ctxt.get_desc( "RIGHT" ),
                      ctxt.get_desc( "PREV_CATEGORY_TAB" ),
                      ctxt.get_desc( "NEXT_CATEGORY_TAB" ),
                      ctxt.get_desc( "HELP_KEYBINDINGS" ) ), c_light_gray ) + "</div>";
        win.set_footer( footer );

        const std::string row = active_header == 0 ? "wg-list-" + std::to_string( cursel[0] ) :
                                "wg-active-" + std::to_string( cursel[1] );
        if( row != shown_row ) {
            shown_row = row;
            win.reveal( row );
        }
    } );

    const auto set_filter = [&]() {
        // current_filter is modified by apply_filter(), we have to copy the value
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        const std::string old_filter = current_filter;

        // The window's filter field takes the typing, and the lists filter as it changes
        input_context filter_ctxt( "STRING_INPUT", keyboard_mode::keychar );
        filter_ctxt.register_action( "TEXT.QUIT" );
        filter_ctxt.register_action( "TEXT.CONFIRM" );
        filter_ctxt.register_action( "ANY_INPUT" );
        filter_ctxt.set_timeout( 50 );
        filtering = true;
        window.start_filter( current_filter );

        for( ;; ) {
            ui_manager::redraw();
            const std::string filter_action = filter_ctxt.handle_input();
            apply_filter( window.filter_text() );

            if( filter_action == "TEXT.QUIT" ) {
                apply_filter( old_filter );
                break;
            } else if( filter_action == "TEXT.CONFIRM" ) {
                break;
            }
        }

        window.hide_filter();
        filtering = false;
    };

    int tab_output = 0;
    while( tab_output == 0 ) {
        ui_manager::redraw();

        std::string action = ctxt.handle_input();
        size_t recmax = active_header == 0 ? static_cast<int>( all_tabs[iCurrentTab].mods.size() ) :
                        static_cast<int>( active_mod_order.size() );
        size_t scroll_rate = recmax > 20 ? 10 : 3;

        // Mouse selection
        if( action == "SELECT" ) {
            // the filter and the close button
            const std::string clicked = window.resolve_click( action );
            if( clicked != action ) {
                action = clicked;
            }
        }
        if( action == "MOUSE_MOVE" || action == "SELECT" ) {
            const int top_tab = window.hovered_tab();
            const int category = window.hovered_index( "data-category" );
            const int pane = window.hovered_index( "data-pane" );
            const int mod = window.hovered_index( "data-mod" );
            if( action == "SELECT" && with_tabs && top_tab >= 0 ) {
                // World Options, the tab after this one
                tab_output = top_tab;
                continue;
            } else if( action == "SELECT" && category >= 0 ) {
                if( static_cast<size_t>( category ) != iCurrentTab ) {
                    iCurrentTab = clamp<int>( category, 0, get_mod_list_tabs().size() - 1 );
                    active_header = 0;
                    cursel[0] = 0;
                }
                continue;
            } else if( pane >= 0 && pane < 2 && mod >= 0 ) {
                active_header = pane;
                cursel[active_header] = mod;
                if( action == "SELECT" ) {
                    action = "CONFIRM";
                }
            }
        }

        if( navigate_ui_list( action, cursel[active_header], scroll_rate, recmax, true ) ) {
            // The window scrolls to it
        } else if( action == "LEFT" || action == "RIGHT" ) {
            active_header = inc_clamp_wrap( active_header, action == "RIGHT", headers.size() );
        } else if( action == "CONFIRM" ) {
            const std::vector<mod_id> &current_tab_mods = all_tabs[iCurrentTab].mods;
            if( active_header == 0 && !current_tab_mods.empty() ) {
                // try-add
                mman_ui->try_add( current_tab_mods[cursel[0]], active_mod_order );
            } else if( active_header == 1 && !active_mod_order.empty() ) {
                // try-rem
                mman_ui->try_rem( cursel[1], active_mod_order );
                if( active_mod_order.empty() ) {
                    // switch back to other list, we can't change
                    // anything in the empty active mods list.
                    active_header = 0;
                }
            }
        } else if( action == "MOVE_MOD_DOWN" ) {
            if( active_header == 1 && active_mod_order.size() > 1 ) {
                mman_ui->try_shift( '+', cursel[1], active_mod_order );
            }
        } else if( action == "MOVE_MOD_UP" ) {
            if( active_header == 1 && active_mod_order.size() > 1 ) {
                mman_ui->try_shift( '-', cursel[1], active_mod_order );
            }
        } else if( action == "NEXT_CATEGORY_TAB" ) {
            if( active_header == 0 ) {
                if( ++iCurrentTab >= get_mod_list_tabs().size() ) {
                    iCurrentTab = 0;
                }

                cursel[0] = 0;
            }

        } else if( action == "PREV_CATEGORY_TAB" ) {
            if( active_header == 0 ) {
                if( --iCurrentTab > get_mod_list_tabs().size() ) {
                    iCurrentTab = get_mod_list_tabs().size() - 1;
                }

                cursel[0] = 0;
            }
        } else if( action == "NEXT_TAB" ) {
            tab_output = 1;
        } else if( action == "PREV_TAB" ) {
            tab_output = -1;
        } else if( action == "SAVE_DEFAULT_MODS" ) {
            if( mman->set_default_mods( active_mod_order ) ) {
                popup( _( "Saved list of active mods as default" ) );
            }
        } else if( action == "VIEW_MOD_DESCRIPTION" ) {
            if( const MOD_INFORMATION *selmod = get_selected_mod() ) {
                popup( "%s", mman_ui->get_information( selmod ) );
            }
        } else if( action == "QUIT" ) {
            tab_output = -999;
        } else if( action == "FILTER" ) {
            set_filter();
        }
        // RESOLVE INPUTS
        if( active_mod_order.empty() ) {
            cursel[1] = 0;
        }

        if( active_header == 1 ) {
            if( active_mod_order.empty() ) {
                cursel[1] = 0;
            } else {
                // If it goes below 0, it'll loop back to max (or at least, greater than AMO size*10.
                if( cursel[1] > active_mod_order.size() * 10 ) {
                    cursel[1] = 0;
                }
                // If it goes above AMO.size(), cap to size.
                else if( cursel[1] >= active_mod_order.size() ) {
                    cursel[1] = active_mod_order.size() - 1;
                }
            }
        }
        // end RESOLVE INPUTS
    }
    return tab_output;
}

static std::string get_opt_slider( int width, int current, int max, bool no_color,
                                   bool no_selector = false )
{
    int new_cur = clamp<int>( std::round( ( width * current ) / static_cast<float>( max ) ),
                              0, width - 1 );
    if( no_selector ) {
        new_cur = -2;
    }

    std::string ret;
    for( int i = 0; i < width; i++ ) {
        char ch = '-';
        if( i == new_cur - 1 ) {
            ch = '<';
        } else if( i == new_cur + 1 ) {
            ch = '>';
        } else if( i == new_cur ) {
            ch = '|';
        }
        if( !no_color && ch != '-' ) {
            ret.append( colorize( std::string( 1, ch ), c_yellow ) );
        } else {
            ret.append( 1, ch );
        }
    }

    return ret;
}

int worldfactory::show_worldgen_basic( WORLD *world )
{
    input_context ctxt( "WORLDGEN_CONFIRM_DIALOG" );
    // dialog actions
    ctxt.register_action( "WORLDGEN_CONFIRM.QUIT" );
    ctxt.register_action( "PICK_RANDOM_WORLDNAME" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "PICK_MODS" );
    ctxt.register_action( "ADVANCED_SETTINGS" );
    ctxt.register_action( "FINALIZE" );
    ctxt.register_action( "RANDOMIZE" );
    ctxt.register_action( "RESET" );
    ctxt.register_leftright();
    ctxt.register_navigate_ui_list();
    // mouse selection
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );

    bool noname = false;
    bool custom_opts = false;

    std::vector<option_slider_id> wg_sliders; // option sliders
    std::vector<int> wg_slevels; // current slider levels
    std::string worldname = world->world_name;
    int sel_opt = 0;
    // The selected option as last shown, to keep it in view
    int shown_opt = -1;

    for( const option_slider &osl : option_slider::get_all() ) {
        if( osl.context() == "WORLDGEN" ) {
            wg_sliders.emplace_back( osl.id );
            wg_slevels.emplace_back( osl.default_level() );
        }
    }
    const std::vector<int> wg_slvl_default = wg_slevels; // save default slider levels

    //~ Title text for the world creation menu.  The < and > characters decorate the border.
    worldgen_window window( _( "< Create World >" ), [&]( worldgen_window & win ) {
        win.set_size( worldgen_width(), "96%" );
        win.set_close_action( "WORLDGEN_CONFIRM.QUIT" );

        const auto get_clr = []( const nc_color & base, bool hi ) {
            return hi ? hilite( base ) : base;
        };

        // World name
        std::string rml = string_format( "<div id=\"wg-opt-0\" class=\"item line%s\" data-opt=\"0\">",
                                         sel_opt == 0 ? " selected" : "" ) +
                          rml_ui::colored_rml( _( "World name:" ), c_white ) + " ";
        if( noname ) {
            rml += rml_ui::colored_rml( _( "________NO NAME ENTERED!________" ), c_light_gray );
        } else {
            rml += rml_ui::colored_rml( worldname, c_pink );
        }
        rml += "</div><div class=\"gap\"></div>";

        // Slider options, each with buttons either side to move it
        for( int i = 0; i < static_cast<int>( wg_sliders.size() ); i++ ) {
            const bool selected = i == sel_opt - 1;
            const std::string sl_txt = get_opt_slider( 30, wg_slevels[i], wg_sliders[i]->count() - 1,
                                       selected, custom_opts );
            rml += string_format( "<div id=\"wg-opt-%d\" class=\"item wg-slider%s\" data-opt=\"%d\">", i + 1,
                                  selected ? " selected" : "", i + 1 ) +
                   "<div>" + rml_ui::colored_rml( wg_sliders[i]->name().translated(), c_white ) + "</div>" +
                   string_format( "<span class=\"button\" data-slider=\"%d\">&lt;</span>", i * 2 ) +
                   "<span class=\"wg-slider-bar\">" + rml_ui::markup_to_rml( sl_txt, c_white ) + "</span>" +
                   string_format( "<span class=\"button\" data-slider=\"%d\">&gt;</span>", i * 2 + 1 ) +
                   rml_ui::colored_rml( custom_opts ? _( "Custom" ) :
                                        wg_sliders[i]->level_name( wg_slevels[i] ).translated(), c_white ) + "</div>";
        }

        // Finish, reset and randomize
        int opt_num = wg_sliders.size() + 1;
        std::string buttons;
        const auto add_button = [&]( const char *action, const char *label ) {
            const bool hi = sel_opt == opt_num;
            const nc_color acc_clr = get_clr( c_yellow, hi );
            const nc_color acc_clr2 = get_clr( c_light_green, hi );
            const nc_color base_clr = get_clr( c_white, hi );
            const std::string btn_txt = string_format( "%s%s%s %s %s", colorize( "[", acc_clr ),
                                        colorize( ctxt.get_desc( action, 1U ), acc_clr2 ),
                                        colorize( "][", acc_clr ), label, colorize( "]", acc_clr ) );
            buttons += string_format( "<span id=\"wg-opt-%d\" class=\"wg-button\" data-opt=\"%d\">", opt_num,
                                      opt_num ) + rml_ui::markup_to_rml( btn_txt, base_clr ) + "</span>";
            opt_num++;
        };
        add_button( "FINALIZE", _( "Finish" ) );
        add_button( "RESET", _( "Reset" ) );
        add_button( "RANDOMIZE", _( "Randomize" ) );
        rml += "<div class=\"wg-buttons\">" + buttons + "</div>";
        win.set_body( rml );

        // Hint text, then the advanced settings
        std::string hint_txt =
            string_format( _( "Press [<color_yellow>%s</color>] to pick a random name for your world.\n"
                              "Navigate options with [<color_yellow>directional keys</color>] "
                              "and confirm with [<color_yellow>%s</color>].\n"
                              "Press [<color_yellow>%s</color>] to see additional control information." ),
                           ctxt.get_desc( "PICK_RANDOM_WORLDNAME", 1U ), ctxt.get_desc( "CONFIRM", 1U ),
                           ctxt.get_desc( "HELP_KEYBINDINGS", 1U ) );
        if( !custom_opts && sel_opt > 0 && sel_opt <= static_cast<int>( wg_sliders.size() ) ) {
            hint_txt = wg_sliders[sel_opt - 1]->level_desc( wg_slevels[sel_opt - 1] ).translated();
        }
        win.set_footer( "<div class=\"wg-hint\">" + rml_ui::markup_to_rml( hint_txt, c_light_gray ) +
                        "</div><div>" + rml_ui::colored_rml( _( "Advanced settings:" ), c_light_gray ) + "</div>" +
                        rml_ui::button_rml( "ADVANCED_SETTINGS", rml_ui::markup_to_rml( string_format(
                                                _( "[<color_yellow>%s</color>] - Advanced options" ),
                                                ctxt.get_desc( "ADVANCED_SETTINGS", 1U ) ), c_light_gray ) ) +
                        rml_ui::button_rml( "PICK_MODS", rml_ui::markup_to_rml( string_format(
                                                _( "[<color_yellow>%s</color>] - Open mod manager" ),
                                                ctxt.get_desc( "PICK_MODS", 1U ) ), c_light_gray ) ) );

        if( sel_opt != shown_opt ) {
            shown_opt = sel_opt;
            win.reveal( "wg-opt-" + std::to_string( sel_opt ) );
        }
    } );

    do {
        ui_manager::redraw();

        std::string action = ctxt.handle_input();
        // Handle mouse input
        if( action == "SELECT" ) {
            // the advanced settings, the mod manager and the close button
            const std::string clicked = window.resolve_click( action );
            if( clicked != action ) {
                action = clicked;
            }
        }
        if( action == "MOUSE_MOVE" || action == "SELECT" ) {
            const int opt = window.hovered_index( "data-opt" );
            const int slider = window.hovered_index( "data-slider" );
            if( opt >= 0 && opt <= static_cast<int>( wg_sliders.size() + 3 ) ) {
                sel_opt = opt;
            }
            if( action == "SELECT" ) {
                if( slider >= 0 && slider < static_cast<int>( wg_sliders.size() * 2 ) ) {
                    sel_opt = slider / 2 + 1;
                    action = slider % 2 == 0 ? "LEFT" : "RIGHT";
                } else if( opt >= 0 ) {
                    action = "CONFIRM";
                }
            }
        }

        // Button shortcuts
        if( action == "FINALIZE" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 1;
            ui_manager::redraw();
        } else if( action == "RESET" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 2;
            ui_manager::redraw();
        } else if( action == "RANDOMIZE" ) {
            action = "CONFIRM";
            sel_opt = wg_sliders.size() + 3;
            // no confirmation prompt, no need to redraw the ui
        }

        // Handle other inputs
        if( action == "CONFIRM" ) {
            if( sel_opt == 0 ) {
                // rename
                std::optional<std::string> ret = prompt_world_name( _( "World name:" ), worldname );
                if( !ret.value_or( "" ).empty() ) {
                    world->world_name = worldname = ret.value();
                }
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 1 ) ) {
                // finish
                if( worldname.empty() ) {
                    noname = true;
                    if( !query_yn( _( "Are you SURE you're finished?  World name will be randomly generated." ) ) ) {
                        noname = false;
                        continue;
                    } else {
                        noname = false;
                        world->world_name = pick_random_name();
                        if( !valid_worldname( world->world_name ) ) {
                            continue;
                        }
                        return 1;
                    }
                } else if( valid_worldname( worldname ) && query_yn( _( "Are you SURE you're finished?" ) ) ) {
                    world->world_name = worldname;
                    return 1;
                }
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 2 ) &&
                       query_yn( _( "Are you sure you want to reset this world?" ) ) ) {
                // reset
                world->WORLD_OPTIONS = get_options().get_world_defaults();
                world->world_saves.clear();
                world->active_mod_order = world_generator->get_mod_manager().get_default_mods();
                wg_slevels = wg_slvl_default;
                custom_opts = false;
            } else if( sel_opt == static_cast<int>( wg_sliders.size() + 3 ) ) {
                // randomize
                for( int i = 0; i < static_cast<int>( wg_sliders.size() ); i++ ) {
                    wg_slevels[i] = wg_sliders[i]->random_level();
                }
            }
        } else if( navigate_ui_list( action, sel_opt, 1, wg_sliders.size() + 2, true ) ) {
            // The window scrolls to it
        } else if( action == "LEFT" || action == "RIGHT" ) {
            if( sel_opt > 0 && sel_opt <= static_cast<int>( wg_sliders.size() ) ) {
                if( custom_opts && query_yn( _( "Currently using customized advanced options.  "
                                                "Reset world options to defaults?" ) ) ) {
                    world->WORLD_OPTIONS = get_options().get_world_defaults();
                    wg_slevels = wg_slvl_default;
                    custom_opts = false;
                    continue;
                } else if( custom_opts ) {
                    continue;
                }
                int lvl = wg_slevels[sel_opt - 1] + ( action == "LEFT" ? -1 : 1 );
                wg_slevels[sel_opt - 1] = clamp<int>( lvl, 0, wg_sliders[sel_opt - 1]->count() - 1 );
                wg_sliders[sel_opt - 1]->apply_opts( wg_slevels[sel_opt - 1], world->WORLD_OPTIONS );
            } else if( sel_opt > static_cast<int>( wg_sliders.size() ) ) {
                if( action == "LEFT" && sel_opt > static_cast<int>( wg_sliders.size() + 1 ) ) {
                    sel_opt--;
                } else if( action == "RIGHT" && sel_opt < static_cast<int>( wg_sliders.size() + 3 ) ) {
                    sel_opt++;
                }
            }
        } else if( action == "PICK_MODS" ) {
            show_worldgen_tab_modselection( world, false );
        } else if( action == "ADVANCED_SETTINGS" ) {
            options_manager::options_container WOPTIONS_OLD = world->WORLD_OPTIONS;
            show_worldgen_tab_options( world, false );
            for( auto &iter : WOPTIONS_OLD ) {
                if( iter.second != world->WORLD_OPTIONS[iter.first] ) {
                    custom_opts = true;
                    break;
                }
            }
        } else if( action == "PICK_RANDOM_WORLDNAME" ) {
            world->world_name = worldname = pick_random_name();
        } else if( action == "WORLDGEN_CONFIRM.QUIT" &&
                   query_yn( _( "Do you want to abort World Generation?" ) ) ) {
            world->world_name = worldname;
            return -999;
        }
    } while( true );

    return 0;
}

std::vector<std::string> worldfactory::worldgen_tab_names()
{
    return { _( "World Mods" ), _( "World Options" ) };
}

bool worldfactory::valid_worldname( const std::string &name, bool automated ) const
{
    std::string msg;

    if( name.empty() ) {
        msg = _( "World name cannot be empty!" );
    } else if( name == "save" || name == "TUTORIAL" || name == "DEFENSE" ) {
        msg = string_format( _( "%s is a reserved name!" ), name );
    } else if( has_world( name ) ) {
        msg = string_format( _( "A world named %s already exists!" ), name );
    } else {
        // just check the raw bytes because unicode characters are always acceptable
        bool allowed = true;
        for( const char ch : name ) {
            // Convert to unsigned char because `std::isprint` is undefined for
            // values unrepresentable by unsigned char which is not EOF.
            const unsigned char uc = static_cast<unsigned char>( ch );
            if( !is_char_allowed( uc ) ) {
                if( std::isprint( uc ) ) {
                    msg = string_format( _( "World name contains invalid character: '%c'" ), uc );
                } else {
                    msg = string_format( _( "World name contains invalid character: 0x%x" ), uc );
                }
                allowed = false;
                break;
            }
        }
        if( allowed ) {
            return true;
        }
    }
    if( !automated ) {
        popup( msg, PF_GET_KEY );
    }
    return false;
}

bool WORLD::create_timestamp()
{
#if defined( TIME_UTC ) && !defined( MACOSX ) && !defined(__ANDROID__)
    std::timespec t;
    if( std::timespec_get( &t, TIME_UTC ) != TIME_UTC ) {
        return false;
    }
#else
    // MinGW-w64 with pthread, MacOS, Android, etc
    timespec t;
    if( clock_gettime( CLOCK_REALTIME, &t ) != 0 ) {
        return false;
    }
#endif

    std::array<char, sizeof( "yyyymmddHHMMSS" )> ts;
    // Using UTC time instead of local time with time zone offset, because %z
    // returns the localized time zone name instead of the time zone offset on
    // MinGW-w64, which does not conform to the standard.
    const std::size_t ts_len = strftime( ts.data(), ts.size(), "%Y%m%d%H%M%S",
                                         std::gmtime( &t.tv_sec ) );
    if( !ts_len ) {
        return false;
    }

    std::ostringstream str;
    str.imbue( std::locale::classic() );
    str << std::string_view( ts.data(), ts_len );
    str << std::setw( 9 ) << std::setfill( '0' ) << t.tv_nsec;
    timestamp = str.str();
    return true;
}

bool WORLD::save_timestamp() const
{
    if( timestamp.empty() ) {
        return true;
    }

    const cata_path path = folder_path() / PATH_INFO::world_timestamp();
    return write_to_file( path, [this]( std::ostream & file ) {
        JsonOut jsout( file );
        jsout.write( timestamp );
    }, _( "world timestamp" ) );
}

bool WORLD::load_timestamp()
{
    const cata_path path = folder_path() / PATH_INFO::world_timestamp();
    return read_from_file_optional_json( path, [this]( const JsonValue & jv ) {
        const std::string ts = jv.get_string();
        // Sanitize the string since it is used in paths
        for( const char ch : ts ) {
            if( ch < '0' || ch > '9' ) {
                jv.throw_error( "Invalid character encountered in world timestamp." );
            }
        }
        timestamp = ts;
    } );
}

void WORLD::load_options( const JsonArray &options_json )
{
    options_manager &opts = get_options();

    for( JsonObject jo : options_json ) {
        jo.allow_omitted_members();
        const std::string name = opts.migrateOptionName( jo.get_string( "name" ) );
        const std::string value = opts.migrateOptionValue( jo.get_string( "name" ),
                                  jo.get_string( "value" ) );

        if( opts.has_option( name ) && opts.get_option( name ).getPage() == "world_default" ) {
            WORLD_OPTIONS[ name ].setValue( value );
        }
    }
}

bool WORLD::load_options()
{
    WORLD_OPTIONS = get_options().get_world_defaults();

    const cata_path path = folder_path() / PATH_INFO::worldoptions();
    return read_from_file_optional_json( path, [this]( const JsonValue & jsin ) {
        this->load_options( jsin );
    } );
}

void load_world_option( const JsonObject &jo )
{
    JsonArray arr = jo.get_array( "options" );
    if( arr.empty() ) {
        jo.throw_error_at( "options", "no options specified" );
    }
    for( const std::string line : arr ) {
        get_options().get_option( line ).setValue( "true" );
    }
}

//load external option from json
void load_external_option( const JsonObject &jo )
{
    std::string name = jo.get_string( "name" );
    std::string stype = jo.get_string( "stype" );
    bool stub = jo.get_bool( "stub", false );
    // This is a hack to aid in migrating options to external without overriding already-set options.
    // See doc/JSON/OPTIONS.md for more information.
    if( stub ) {
        jo.allow_omitted_members();
        return;
    }
    options_manager &opts = get_options();
    if( !opts.has_option( name ) ) {
        opts.add_external( name, "external_options", stype );
    }
    options_manager::cOpt &opt = opts.get_option( name );
    // TODO: Hook up to cata_variant instead?
    if( stype == "float" ) {
        opt.setValue( static_cast<float>( jo.get_float( "value" ) ) );
    } else if( stype == "int" ) {
        opt.setValue( jo.get_int( "value" ) );
    } else if( stype == "bool" ) {
        if( jo.get_bool( "value" ) ) {
            opt.setValue( "true" );
        } else {
            opt.setValue( "false" );
        }
    } else if( stype == "string" || stype == "string_input" ) {
        opt.setValue( jo.get_string( "value" ) );
    } else {
        jo.throw_error_at( "stype", "Unknown or unsupported stype for external option" );
    }
    options_manager::update_options_cache();
}

bool WORLD::has_compression_enabled() const
{
    if( !is_compressed.has_value() ) {
        cata_path world_folder_path = folder_path();
        is_compressed.emplace(
            std::filesystem::exists( ( world_folder_path / "maps.dict" ).get_unrelative_path() ) ||
            std::filesystem::exists( ( world_folder_path / "mmr.dict" ).get_unrelative_path() ) ||
            std::filesystem::exists( ( world_folder_path / "overmaps.dict" ).get_unrelative_path() ) );
    }
    return is_compressed.value();
}

bool WORLD::set_compression_enabled( bool enabled )
{
    // Return immediately if we're already in the desired state.
    if( enabled == is_compressed ) {
        return true;
    }
    static_popup popup;
    cata_path world_folder_path = folder_path();
    if( enabled ) {
        cata_path dictionary_folder = PATH_INFO::compression_folder_path();
        cata_path maps_dict = dictionary_folder / "maps.dict";
        cata_path mmr_dict = dictionary_folder / "mmr.dict";
        cata_path overmaps_dict = dictionary_folder / "overmaps.dict";

        std::vector<cata_path> folders_to_clean;
        std::vector<cata_path> files_to_clean;

        std::vector<cata_path> dimension_folders = get_directories( world_folder_path / "dimensions" );
        dimension_folders.push_back( world_folder_path );

        copy_file( maps_dict, world_folder_path / "maps.dict" );
        copy_file( overmaps_dict, world_folder_path / "overmaps.dict" );
        copy_file( mmr_dict, world_folder_path / "mmr.dict" );

        for( const cata_path &dimension_folder : dimension_folders ) {
            {
                std::vector<cata_path> maps_folders = get_directories( dimension_folder / "maps" );
                std::filesystem::path maps_dict_path = maps_dict.get_unrelative_path();
                size_t done = 0;
                for( const cata_path &map_folder : maps_folders ) {
                    popup.message( _( "Compressing maps [%d/%d]" ), done++, maps_folders.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();
                    if( !zzip::create_from_folder( ( map_folder + zzip_suffix ).get_unrelative_path(),
                                                   map_folder.get_unrelative_path(), maps_dict_path ) ) {
                        return false;
                    }
                }
                folders_to_clean = std::move( maps_folders );
            }
            {
                std::vector<cata_path> overmaps = get_files_from_path( "o.", dimension_folder );
                files_to_clean.reserve( files_to_clean.size() + overmaps.size() );
                size_t done = 0;
                std::error_code ec;
                assure_dir_exist( dimension_folder / zzip_overmap_directory );
                std::filesystem::path world_folder_unrelative_path = dimension_folder.get_unrelative_path();
                for( const cata_path &overmap : overmaps ) {
                    // Some random other files might have `o.` in the name. We only care about the actual
                    // overmap files whose names start with `o.`.
                    std::filesystem::path overmap_file_path = overmap.get_unrelative_path();
                    std::filesystem::path overmap_file_name = overmap_file_path.filename();
                    if( overmap_file_name.generic_u8string().find( "o." ) != 0 ) {
                        continue;
                    }
                    popup.message( _( "Compressing overmaps [%d/%d]" ), done++, overmaps.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    // Each overmap gets put into its own zzip indexed by its own file name.
                    std::optional<zzip> overmap_zzip = zzip::create_from_folder_with_files( (
                                                           dimension_folder / zzip_overmap_directory / overmap_file_name + zzip_suffix ).get_unrelative_path(),
                                                       world_folder_unrelative_path, { overmap_file_path }, 0,
                                                       overmaps_dict.get_unrelative_path() );
                    if( !overmap_zzip ) {
                        return false;
                    }
                    files_to_clean.push_back( overmap );
                }
            }
            {
                std::vector<cata_path> character_map_memories;
                size_t done = 0;
                // Each of these is a folder for per-character map memory.
                // We compress each into a zzip_stack, with the same folder name for simplicity.
                // Each map memory region file inside the folders is compressed separately.
                character_map_memories = get_files_from_path( ".mm1", dimension_folder, false, true );
                for( const cata_path &character_map_memory_folder : character_map_memories ) {
                    popup.message( _( "Compressing map memory [%d/%d]" ), done++, character_map_memories.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    std::vector<cata_path> character_map_memory_files = get_files_from_path( ".mmr",
                            character_map_memory_folder, false, true );
                    std::filesystem::path mmr_path = character_map_memory_folder.get_unrelative_path();
                    for( const cata_path &map_memory : character_map_memory_files ) {
                        std::filesystem::path map_memory_filename = map_memory.get_unrelative_path().filename();
                        std::shared_ptr<zzip_stack> map_memory_zzip = zzip_stack::create_from_folder_with_files(
                                    character_map_memory_folder.get_unrelative_path(),
                                    mmr_path, { mmr_path / map_memory_filename }, 0,
                                    mmr_dict.get_unrelative_path() );
                        if( !map_memory_zzip ) {
                            return false;
                        }
                    }

                    files_to_clean.insert( files_to_clean.end(), character_map_memory_files.begin(),
                                           character_map_memory_files.end() );
                }
            }
            {
                std::vector<cata_path> saves;
                size_t done = 0;
                std::error_code ec;
                saves = get_files_from_path( ".sav", dimension_folder );
                for( const cata_path &save : saves ) {
                    popup.message( _( "Compressing main save files [%d/%d]" ), done++, saves.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    // Each save gets put into its own zzip indexed by its own file name.
                    std::filesystem::path save_file_path = save.get_unrelative_path();
                    std::filesystem::path save_file_name = save_file_path.filename();

                    // But first, we have to do some surgery.
                    // Current the first line of a save file is a hokey line like
                    // # version <number>
                    // This is outdated from a time when the file had to be loaded linearly
                    // in text. We now parse to a binary format and can random access anything
                    // in constant time. The new format encodes the version as a regular json
                    // member. The compressed save load path requires that. So first we test if
                    // the first line is this format and we insert it manually textually if it is.
                    std::string savefile_contents = read_entire_file( save_file_path );
                    if( savefile_contents.empty() ) {
                        // Eh just skip it.
                        continue;
                    }
                    if( savefile_contents[0] == '#' ) {
                        // Parse the version header.
                        std::string temp_savefile_contents = std::move( savefile_contents );
                        savefile_contents.clear();
                        size_t newline = temp_savefile_contents.find( '\n' );
                        size_t char_after_open_brace = temp_savefile_contents.find_first_not_of( '{', newline + 1 );
                        std::string_view header{ temp_savefile_contents.data(), newline };
                        std::string_view savefile_json{ temp_savefile_contents.data() + char_after_open_brace, temp_savefile_contents.size() - char_after_open_brace };
                        int temp_savefile_version = std::strtol( header.data() + header.find_last_of( ' ' ), nullptr, 10 );
                        savefile_contents.reserve( savefile_json.size() + 32 ); // 30 for text and 2 for digits.
                        savefile_contents.append( "{\"savegame_loading_version\":" );
                        savefile_contents.append( std::to_string( temp_savefile_version ) );
                        savefile_contents.append( ",\n" );
                        savefile_contents.append( savefile_json );
                    };

                    std::optional<zzip> save_zzip = zzip::load( ( dimension_folder / save_file_name +
                                                    zzip_suffix ).get_unrelative_path() );
                    if( !save_zzip ) {
                        return false;
                    }
                    save_zzip->add_file( save_file_name, savefile_contents );
                    files_to_clean.push_back( save );
                }
            }

            size_t done = 0;
            size_t to_do = folders_to_clean.size() + files_to_clean.size();
            for( const cata_path &folder : folders_to_clean ) {
                popup.message( _( "Cleaning up [%d/%d]" ), done++, to_do );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                std::error_code ec;
                std::filesystem::remove_all( folder.get_unrelative_path(), ec );
            }
            for( const cata_path &file : files_to_clean ) {
                popup.message( _( "Cleaning up [%d/%d]" ), done++, to_do );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                std::error_code ec;
                std::filesystem::remove( file.get_unrelative_path(), ec );
            }
        }
    } else {
        std::vector<cata_path> dimension_folders = get_directories( world_folder_path / "dimensions" );
        dimension_folders.push_back( world_folder_path );

        cata_path maps_dict = world_folder_path / "maps.dict";
        cata_path overmaps_dict = world_folder_path / "overmaps.dict";
        cata_path mmr_dict = world_folder_path / "mmr.dict";

        for( const cata_path &dimension_folder : dimension_folders ) {

            std::vector<cata_path> zzips_to_clean;

            std::vector<cata_path> maps_zzips = get_files_from_path( "zzip", dimension_folder / "maps", false,
                                                true );
            std::vector<cata_path> overmap_zzips = get_files_from_path( "zzip",
                                                   dimension_folder / zzip_overmap_directory,
                                                   false, true );
            std::vector<cata_path> character_map_memory_folders = get_files_from_path( ".mm1",
                    dimension_folder, false, true );
            std::vector<cata_path> save_zzips = get_files_from_path( ".sav" + std::string( zzip_suffix ),
                                                dimension_folder,
                                                false, true );

            zzips_to_clean.reserve( maps_zzips.size() + overmap_zzips.size() +
                                    character_map_memory_folders.size() * 3 );

            size_t done = 0;
            {
                std::filesystem::path maps_dict_path = maps_dict.get_unrelative_path();
                for( const cata_path &map_zzip : maps_zzips ) {
                    popup.message( _( "Decompressing maps [%d/%d]" ), done++, maps_zzips.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();
                    std::filesystem::path zzip_path = map_zzip.get_unrelative_path();
                    std::filesystem::path dest_folder_name = zzip_path.parent_path() / zzip_path.stem();
                    if( !zzip::extract_to_folder( zzip_path, dest_folder_name, maps_dict_path ) ) {
                        return false;
                    }
                }
                zzips_to_clean.insert( zzips_to_clean.end(), maps_zzips.begin(), maps_zzips.end() );
            }
            {
                size_t done = 0;
                std::filesystem::path overmaps_dict_path = overmaps_dict.get_unrelative_path();
                zzips_to_clean.reserve( zzips_to_clean.size() + overmap_zzips.size() );
                std::filesystem::path dest_folder_name = dimension_folder.get_unrelative_path();
                for( cata_path &overmap_zzip : overmap_zzips ) {

                    popup.message( _( "Decompressing overmaps [%d/%d]" ), done++, overmap_zzips.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    std::filesystem::path zzip_path = overmap_zzip.get_unrelative_path();
                    if( !zzip::extract_to_folder( zzip_path, dest_folder_name, overmaps_dict_path ) ) {
                        return false;
                    }
                    zzips_to_clean.push_back( std::move( overmap_zzip ) );
                }
                zzips_to_clean.push_back( dimension_folder / zzip_overmap_directory );
            }
            {
                size_t done = 0;
                std::filesystem::path mmr_dict_path = mmr_dict.get_unrelative_path();
                std::vector<cata_path> character_map_memory_zzips;
                for( const cata_path &character_map_memory_zzip : character_map_memory_folders ) {
                    popup.message( _( "Decompressing map memory [%d/%d]" ), done++,
                                   character_map_memory_folders.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    std::filesystem::path zzip_path = character_map_memory_zzip.get_unrelative_path();
                    // We reuse the same folder for the map memory files.
                    const std::filesystem::path &dest_folder_name = zzip_path;
                    if( !zzip_stack::extract_to_folder( zzip_path, dest_folder_name, mmr_dict_path ) ) {
                        return false;
                    }

                    character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                            dest_folder_name.filename().concat( ".cold" + std::string( // NOLINT(cata-u8-path)
                                        zzip_suffix ) ) ); // NOLINT(cata-u8-path)
                    character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                            dest_folder_name.filename().concat( ".warm" + std::string( // NOLINT(cata-u8-path)
                                        zzip_suffix ) ) ); // NOLINT(cata-u8-path)
                    character_map_memory_zzips.emplace_back( character_map_memory_zzip /
                            dest_folder_name.filename().concat( ".hot" + std::string( zzip_suffix ) ) ); // NOLINT(cata-u8-path)
                }
                zzips_to_clean.insert( zzips_to_clean.end(), character_map_memory_zzips.begin(),
                                       character_map_memory_zzips.end() );
            }
            {
                size_t done = 0;
                std::filesystem::path dest_folder_name = dimension_folder.get_unrelative_path();
                for( cata_path &save_zzip : save_zzips ) {

                    popup.message( _( "Decompressing main save files [%d/%d]" ), done++, overmap_zzips.size() );
                    ui_manager::redraw();
                    refresh_display();
                    inp_mngr.pump_events();

                    std::filesystem::path zzip_path = save_zzip.get_unrelative_path();
                    if( !zzip::extract_to_folder( zzip_path, dest_folder_name ) ) {
                        return false;
                    }
                    zzips_to_clean.push_back( std::move( save_zzip ) );
                }
            }
            remove_file( maps_dict );
            remove_file( overmaps_dict );
            remove_file( mmr_dict );
            done = 0;
            for( const cata_path &zzip_to_clean : zzips_to_clean ) {
                popup.message( _( "Cleaning up [%d/%d]" ), done++, zzips_to_clean.size() );
                ui_manager::redraw();
                refresh_display();
                inp_mngr.pump_events();
                std::error_code ec;
                std::filesystem::remove( zzip_to_clean.get_unrelative_path(), ec );
            }
        }
    }
    is_compressed = enabled;
    return true;
}

mod_manager &worldfactory::get_mod_manager()
{
    return *mman;
}

WORLD *worldfactory::get_world( const std::string &name )
{
    const auto iter = all_worlds.find( name );
    if( iter == all_worlds.end() ) {
        debugmsg( "Requested non-existing world %s, prepare for crash", name );
        return nullptr;
    }
    return iter->second.get();
}

std::string worldfactory::get_world_name( const size_t index )
{
    size_t i = 0;
    for( const auto &elem : all_worlds ) {
        if( i == index ) {
            return elem.first;
        }
        i++;
    }
    return "";
}

size_t worldfactory::get_world_index( const std::string &name )
{
    size_t i = 0;
    for( const auto &elem : all_worlds ) {
        if( elem.first == name ) {
            return i;
        }
        i++;
    }
    return 0;
}

// Helper predicate to exclude files from deletion when resetting a world directory.
static bool isForbidden( const cata_path &candidate )
{
    std::filesystem::path candidate_path = candidate.get_unrelative_path();
    std::string filename = candidate_path.filename().generic_u8string();
    return filename == PATH_INFO::worldoptions()
           || filename == "mods.json"
           || candidate_path.extension().generic_u8string() == ".dict";
}

void worldfactory::delete_world( const std::string &worldname, const bool delete_folder )
{
    cata_path worldpath = get_world( worldname )->folder_path();
    std::set<std::filesystem::path> directory_paths;

    if( delete_folder ) {
        std::filesystem::remove_all( worldpath.get_unrelative_path() );
        remove_world( worldname );
        return;
    }

    // Clear out everything except options and mods and compression dictionaries.
    // It would be easier to delete and recreate the world, but some people,
    // like the author of this code, use symlinks to have world contents located
    // 'elsewhere', and doing so would break such use cases.
    auto file_paths = get_files_from_path( "", worldpath, true, true );
    auto end = std::remove_if( file_paths.begin(), file_paths.end(), isForbidden );
    file_paths.erase( end, file_paths.end() );

    for( cata_path &file_path : file_paths ) {
        std::filesystem::path folder_path = file_path.get_unrelative_path().parent_path();
        while( folder_path.filename() != std::filesystem::u8path( worldname ) ) {
            directory_paths.insert( folder_path );
            folder_path = folder_path.parent_path();
        }
    }

    for( cata_path &file : file_paths ) {
        remove_file( file );
    }

    // Trying to remove a non-empty parent directory before a child
    // directory will fail.  Removing directories in reverse order
    // will prevent this situation from arising.
    for( auto it = directory_paths.rbegin(); it != directory_paths.rend(); ++it ) {
        remove_directory( *it );
    }
    get_world( worldname )->world_saves.clear();
}

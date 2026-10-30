#include "loading_ui.h"

#include <algorithm>
#include <string>
#include <vector>

#include "cached_options.h"
#include "cata_path.h"
#include "catacharset.h"
#include "color.h"
#include "filesystem.h"
#include "input.h"
#include "mod_manager.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "rml_markup.h"
#include "rml_ui.h"
#include "rng.h"
#include "sdl_wrappers.h"
#include "sdltiles.h"
#include "string_formatter.h"
#include "text_snippets.h"
#include "ui_manager.h"
#include "worldfactory.h"

namespace
{
// The loading screen, from data/raw/rmlui/loading.rml: a splash image, a tip
// below it and what is loading below that.
class loading_screen : public rml_ui::document
{
    public:
        loading_screen() : rml_ui::document( "loading.rml" ) {}

        void set_splash( const cata_path &image ) {
            if( SDL_Surface_Ptr surf = load_image( image.get_unrelative_path().u8string().c_str() ) ) {
                image_size = point( surf->w, surf->h );
            }
            set_attribute( "splash", "src", rml_ui::file_source( image ) );
        }

        void set_tip( const std::string &text ) {
            tip = text;
            set_inner_rml( "tip", rml_ui::markup_to_rml( colorize( tip, c_cyan ) ) );
        }

        void set_message( const std::string &text ) {
            set_inner_rml( "message", rml_ui::escape_rml( text ) );
        }

    protected:
        void on_redraw() override {
            const point view = rml_ui::viewport_px();
            if( view == laid_out_for || image_size.x <= 0 || image_size.y <= 0 ) {
                return;
            }
            laid_out_for = view;
            // if our screen space is at least twice as large as minimal, we will use 1.5x font size
            const float dp = rml_ui::dp_ratio();
            const bool large_hint_size = view.y / dp > 2 * 384;
            set_class( "tip", "large", large_hint_size );

            // Scale the image to the room left by the text below it, which is
            // the tip wrapped to the screen's width and a line of loading
            // message, preserving its aspect ratio; it scales both "up" and "down"
            const float tip_font_px = ( large_hint_size ? 24.f : 16.f ) * dp;
            const float max_width = view.x * 0.96f;
            const int tip_lines = 1 + static_cast<int>( utf8_width( tip ) * 0.56f * tip_font_px /
                                  std::max( 1.f, max_width ) );
            const float max_height = view.y * 0.96f - 1.3f * ( tip_lines * tip_font_px + 2 * 16.f * dp );
            const float longest_side_ratio = std::max( image_size.x / max_width,
                                             image_size.y / std::max( 1.f, max_height ) );
            set_attribute( "splash", "style", string_format( "width: %.0fpx; height: %.0fpx;",
                           image_size.x / longest_side_ratio, image_size.y / longest_side_ratio ) );
        }

    private:
        std::string tip;
        point image_size;
        point laid_out_for;
};

struct ui_state {
    // Before the screen, so it is drawn below it
    background_pane bg;
    loading_screen screen;
    cata_path chosen_load_img;
};
} // namespace

static ui_state *gLUI = nullptr;

static void update_state( const std::string &context, const std::string &step )
{
    if( gLUI == nullptr ) {
        gLUI = new struct ui_state;

        // get snippet text
        gLUI->screen.set_tip( SNIPPET.random_from_category( "tip" ).value_or(
                                  translation() ).translated() );

        // get image
        std::vector<cata_path> imgs;
        std::vector<mod_id> &active_mod_list = world_generator->active_world->active_mod_order;
        bool only_mod_loading_screens = false;
        for( mod_id &some_mod : active_mod_list ) {
            // We haven't migrated mods yet
            // TODO: Move mod migration before this function?
            if( !some_mod.is_valid() ) {
                continue;
            }
            const MOD_INFORMATION &mod = *some_mod;
            if( mod.disable_other_loading_screens ) {  // Clear any already loaded screens, use only from the mod that has asked for this behaviour in the modinfo.
                only_mod_loading_screens = true;
                imgs.clear();
            }
            for( const std::string &img_name : mod.loading_images ) {
                // There may be more than one file matching the name, so we need to get all of them
                for( cata_path &img_path : get_files_from_path( img_name, mod.path, true ) ) {
                    imgs.emplace_back( img_path );
                }
            }
            if( only_mod_loading_screens ) {
                break;
            }
        }
        // We haven't selected a loading image yet, let's do so.
        if( gLUI->chosen_load_img == cata_path() ) {
            if( imgs.empty() ||
                !only_mod_loading_screens ) { // No modded screens available or not disabled in the mods, so let's add vanilla ones.
                const std::string img_name_pattern = "loading_img";
                const cata_path load_screens_directory = PATH_INFO::gfxdir() / "loading_screens";
                for( cata_path &img_path : get_files_from_path( img_name_pattern, load_screens_directory, true ) ) {
                    imgs.emplace_back( img_path );
                }
            }
            if( !imgs.empty() ) {
                gLUI->chosen_load_img = random_entry( imgs );
            }
        }
        if( !( gLUI->chosen_load_img == cata_path() ) ) {
            // RmlUi loads the texture itself as it draws, and again after a renderer reset
            gLUI->screen.set_splash( gLUI->chosen_load_img );
        }
    }
    gLUI->screen.set_message( string_format( "%s %s", context, step ) );
}

void loading_ui::show( const std::string &context, const std::string &step )
{
    if( test_mode ) {
        return;
    }
    update_state( context, step );
    drain_renderer_recovery();
    ui_manager::redraw();
    refresh_display();
    inp_mngr.pump_events();
}

void loading_ui::done()
{
    if( gLUI != nullptr ) {
        delete gLUI;
        gLUI = nullptr;
    }
}

#pragma once
#ifndef CATA_SRC_MAIN_MENU_H
#define CATA_SRC_MAIN_MENU_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "cuboid_rectangle.h"
#include "cursesdef.h"
#include "enums.h"
#include "input_context.h"
#include "input_enums.h"
#include "point.h"
#include "worldfactory.h"

class main_menu_rml;

/**
 * The title screen, drawn with RmlUi from data/raw/rmlui/main_menu.rml. Input
 * and actions stay here; the document only shows the state pushed to it.
 */
class main_menu
{
    public:
        main_menu() : ctxt( "MAIN_MENU", keyboard_mode::keychar ) { }
        // Shows the main menu and returns whether a game was started or not
        bool opening_screen();

        static std::string queued_world_to_load;
        static std::string queued_save_id_to_load;
    private:
        // ASCII art that says "Cataclysm Dark Days Ahead"
        std::vector<std::string> mmenu_title;
        std::string mmenu_motd;
        std::string mmenu_credits;
        std::vector<std::string> vMenuItems; // MOTD, New Game, Load Game, etc.
        std::vector<std::string> vWorldSubItems;
        std::vector<std::string> vNewGameSubItems;
        std::vector<std::string> vNewGameHints;
        std::vector< std::vector<std::string> > vWorldHotkeys;
        std::vector<std::string> vSettingsSubItems;
        std::vector< std::vector<std::string> > vSettingsHotkeys;
        std::vector< std::vector<std::string> > vMenuHotkeys; // hotkeys for the vMenuItems
        std::vector< std::vector<std::string> > vNewGameHotkeys;
        std::string vdaytip; //tip of the day

        /**
         * Does what it sounds like, but this function also exists in order to gracefully handle
         * the case where the player goes to the 'Settings' tab and changes the language.
        */
        void init_strings();
        /** Helper function for @ref init_strings */
        std::vector<std::string> load_file( const std::string &path,
                                            const std::string &alt_text ) const;

        // Play a sound whenever the user moves left or right in the main menu or its tabs
        void on_move() const;

        // Play a sound *once* when an error occurs in the main menu or its tabs
        void on_error();

        // Tab functions. They return whether a game was started or not. The ones that can never
        // start a game have a void return type.
        bool load_game( std::string const &worldname, save_t const &savegame );
        bool new_character_tab();
        bool load_character_tab( const std::string &worldname );
        void world_tab( const std::string &worldname );

        /*
         * Load character templates from template folder
         */
        void load_char_templates();

        // These variables are shared between @opening_screen and the tab functions.
        // TODO: But this is an ugly short-term solution.
        input_context ctxt;
        int sel1 = 1;
        int sel2 = 1;
        size_t last_world_pos = 0;
        // First of the tab's options shown, when they are too many to show at once
        int sub_opt_off = 0;
        std::vector<std::string> templates;
        std::vector<save_t> savegames;

        // The options under the selected tab, as game markup, or empty for a tab without
        std::vector<std::string> sub_options() const;
        // Push the menu's state to its document, before a redraw
        void show_rml( main_menu_rml &doc );
        holiday get_holiday_from_time();

        holiday current_holiday = holiday::none;

        static std::string halloween_spider();
        std::string halloween_graves();
};

#endif // CATA_SRC_MAIN_MENU_H


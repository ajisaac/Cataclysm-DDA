#pragma once
#ifndef CATA_SRC_FACTION_UI_H
#define CATA_SRC_FACTION_UI_H

#include <string>
#include <vector>

#include "input_context.h"
#include "rml_window.h"
#include "translations.h"
#include "type_id.h"

class basecamp;
class faction;
class npc;

enum class tab_mode : int {
    TAB_MYFACTION = 0,
    TAB_FOLLOWERS,
    TAB_OTHERFACTIONS,
    TAB_CREATURES,
    NUM_TABS
};

enum class radio_contact_result : int {
    ALPHA_NO_RADIO,
    BETA_NO_RADIO,
    BOTH_NO_RADIO,
    TOO_FAR,
    YES,
    last
};

class faction_ui : public rml_ui::window
{
    public:
        faction_ui() : rml_ui::window( _( "Faction" ) ) {}

        bool execute();

        std::string hint_rml() const;

        // Each list applies any pending move or click to its selection, so
        // build it before the display of what it selected
        std::string your_faction_tab_rml();
        std::string your_factions_list_rml();
        std::string your_faction_display() const;

        std::string your_followers_tab_rml();
        std::string your_followers_list_rml();
        std::string your_follower_display();

        std::string other_factions_tab_rml();
        std::string other_factions_list_rml();
        std::string other_faction_display();

        // i think creature tab better be migrated to diary and adjacent UI
        std::string creatures_tab_rml();
        std::string creature_list_rml();
        std::string creature_display() const;

        void radio_the_faction();

    protected:
        void on_redraw() override;

    private:
        // Rows of a list, the selected one marked, with the index `selected`
        std::string list_rml( const std::vector<std::string> &names, int selected );
        // Move the selection in a list of this many entries by any pending move
        // or click, starting from `selected`
        int apply_pending( int selected, int count );

        input_context ctxt;
        tab_mode selected_tab = tab_mode::TAB_MYFACTION;
        // The tab last shown, to scroll a new one's info to its top
        tab_mode shown_tab = tab_mode::NUM_TABS;
        // Moves and clicks on the list, for the next redraw to apply
        int pending_move = 0;
        int pending_click = -1;
        // The row of the current list that is selected, and whether to bring it into view
        int selected_row = 0;
        bool reveal_selected = true;
        // hack, hide the window if picked talking to someone
        bool hide_ui = false;

        basecamp *picked_camp = nullptr;
        npc *picked_follower = nullptr;
        const faction *picked_faction = nullptr;
        const mtype_id *picked_creature = nullptr;
};

#endif // CATA_SRC_FACTION_UI_H

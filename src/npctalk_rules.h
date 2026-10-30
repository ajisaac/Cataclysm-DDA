#pragma once
#ifndef CATA_SRC_NPCTALK_RULES_H
#define CATA_SRC_NPCTALK_RULES_H

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "input_context.h"
#include "rml_window.h"

class npc;
struct input_event;

class follower_rules_ui
{
        friend class follower_rules_ui_impl;
    public:
        void draw_follower_rules_ui( npc *guy );
};

class follower_rules_ui_impl : public rml_ui::window
{
    public:
        follower_rules_ui_impl();

        void set_npc_pointer_to( npc *new_guy );
        void run();

    protected:
        void on_redraw() override;

    private:
        // One line of the rules page. Clicking it, or confirming it while it
        // has the cursor, activates it; a heading has no activation.
        struct rule_row {
            std::string rml;
            std::function<void()> activate;
            // The rule's Default button, if it has one
            std::function<void()> reset;
        };
        // A hotkey and what pressing it does
        using rule_hotkey = std::pair<input_event, std::function<void()>>;

        // Lay out the rules page as it stands now
        void build_rules( std::vector<rule_row> &rows, std::vector<rule_hotkey> &hotkeys );
        std::string rules_rml( const std::vector<rule_row> &rows ) const;
        std::string transfer_rml() const;
        // Handle input on the import/export page; false to close the screen
        bool transfer_input( const std::string &action );
        // Copy one group of rules between the follower and another, or all of
        // them for a column of -1
        void transfer_rules( npc &role_model, int column );
        std::vector<npc *> other_allies() const;

        npc *guy = nullptr;
        std::string get_parsed( std::string initial_string );
        std::string hotkey_rml( const input_event &hotkey ) const;

        // makes a checkbox for the rule
        template<typename T>
        void checkbox( const T &this_rule, const input_event &assigned_hotkey,
                       std::vector<rule_row> &rows, std::vector<rule_hotkey> &hotkeys );

        // makes one radio button per option in the map
        template<typename T>
        void radio_group( const char *title, T *rule, std::map<T, std::string> &values,
                          const input_event &assigned_hotkey, std::vector<rule_row> &rows,
                          std::vector<rule_hotkey> &hotkeys );

        input_context ctxt;
        // The row with the keyboard cursor, and whether to bring it into view
        int cursor = 0;
        bool reveal_cursor = false;
        bool exporting_rules = false;
        bool in_popup = false;
};

#endif // CATA_SRC_NPCTALK_RULES_H

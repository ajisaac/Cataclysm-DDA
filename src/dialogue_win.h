#pragma once
#ifndef CATA_SRC_DIALOGUE_WIN_H
#define CATA_SRC_DIALOGUE_WIN_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "color.h"
#include "output.h"
#include "rml_window.h"

class dialogue_view;
class input_context;

struct talk_data {
    nc_color color;
    std::string hotkey_desc;
    std::string text;
};

/**
 * NPC conversation dialogue window.
 */
class dialogue_window
{
    public:
        dialogue_window() = default;
        dialogue_window( const dialogue_window & ) = delete;
        dialogue_window &operator=( const dialogue_window & ) = delete;

        /**
         * Open the window, which shows the conversation until the returned
         * window is destroyed.
         * @param npc_name Gives the name for the title, when it is drawn.
         */
        std::unique_ptr<rml_ui::window> open( const std::function<std::string()> &npc_name );

        /**
         * Move the selected response, or scroll the history, for this action.
         * A click on a response turns `action` into CONFIRM, and a click on a
         * button into the button's action.
         */
        void handle_scrolling( std::string &action, input_context &ctxt );

        /** Adds a message to the conversation history. */
        void add_to_history( const std::string &text );
        /** Adds a message to the conversation history for a given speaker. */
        void add_to_history( const std::string &text, const std::string &speaker_name,
                             nc_color speaker_color );
        /** Adds a separator to the conversation history. */
        void add_history_separator();

        void set_responses( const std::vector<talk_data> &new_responses );

        void set_up_scrolling( input_context &ctxt ) const;

        /** sets debug info to draw for a response */
        void set_responses_debug( const std::vector<std::string> &new_responses );

        /** Unhighlights all messages. */
        void clear_history_highlights();
        bool is_computer = false;
        bool is_not_conversation = false;
        // Remote conversation (intercom, radio). Hides physical-presence
        // actions (Look at, Assess, etc.) but keeps normal dialogue style.
        // If remote_name is set, it replaces the NPC name in the header.
        bool is_remote = false;
        std::string remote_name;
        bool show_dynamic_line_conditionals = true;
        bool show_dynamic_line_effects = true;
        bool show_response_conditionals = true;
        bool show_response_effects = true;
        //copy of dialogue::show_all_responses
        bool show_all_responses = false;
        int sel_response = 0;
        std::string debug_topic_name;
    private:
        friend class dialogue_view;

        /** The open window, if there is one; it sets and clears this itself. */
        dialogue_view *view = nullptr;

        struct history_message {
            inline history_message( nc_color c, const std::string &t ) : color( c ), text( t ) {}

            nc_color color; // Text color when highlighted
            std::string text;
        };

        void add_to_history( const std::string &text, nc_color color );

        /**
         * This contains the exchanged words, it is basically like the global message log.
         *
         * Each responses of the player character and the NPC are added as are information about
         * what each of them does (e.g. the npc drops their weapon).
         * This will be displayed in the dialog window and should already be translated.
         */
        std::vector<history_message> history;
        /** Whether history has been added to since the window last showed it. */
        bool history_changed = true;
        /** Number of history messages to highlight. */
        int num_lines_highlighted = 0;
        std::vector<talk_data> responses;
        /** Whether the selected response should be scrolled into view. */
        bool reveal_response = true;
        std::vector<std::string> responses_debug;

        nc_color default_color() const;
        std::string title_markup( const std::string &name ) const;
        std::string debug_flags_rml() const;
        std::string body_rml() const;
};
#endif // CATA_SRC_DIALOGUE_WIN_H


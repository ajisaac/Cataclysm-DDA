#pragma once
#ifndef CATA_SRC_INPUT_POPUP_H
#define CATA_SRC_INPUT_POPUP_H

#include <climits>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "color.h"
#include "coords_fwd.h"
#include "input_context.h"
#include "point.h"
#include "rml_ui.h"
#include "translation.h"

struct callback_input {
    std::string action;
    int key = INT_MIN;
    std::optional<translation> description;

    callback_input( const std::string &action, int key,
                    std::optional<translation> description = std::nullopt ) : action( action ), key( key ),
        description( std::move( description ) ) {};
    explicit callback_input( const std::string &action,
                             std::optional<translation> description = std::nullopt ) : action( action ),
        description( std::move( description ) ) {};
    explicit callback_input( int key,
                             std::optional<translation> description = std::nullopt ) : key( key ),
        description( std::move( description ) ) {};

    bool operator==( const callback_input &rhs ) const {
        return ( !action.empty() && action == rhs.action ) ||
               ( key != INT_MIN && key == rhs.key );
    }
};

/**
 * A popup asking for one line of input, drawn with RmlUi from
 * data/raw/rmlui/input_popup.rml. RmlUi edits the text field itself; actions
 * such as confirm, cancel and the custom callbacks still come from `ctxt`.
 */
class input_popup : public rml_ui::document
{
    public:
        /**
         * @param width Popup width in characters, or <= 0 to fit the content.
         * @param title Shown above everything else; empty for none.
         */
        explicit input_popup( int width, const std::string &title = "" );

        // todo: monofont is a hack for easy support of old descriptions and should be replaced by a more universal solution
        void set_description( const std::string &desc, const nc_color &default_color = c_green,
                              bool monofont = false );
        void set_label( const std::string &label, const nc_color &default_color = c_light_red );
        void set_max_input_length( int length );
        int get_max_input_length() const;
        void add_callback( const callback_input &input, const std::function<bool()> &callback_func );
        bool cancelled() const;

    protected:
        void on_redraw() override;

        /// Put this text in the field on the next redraw.
        void show_text( const std::string &text );
        /// What is in the field now, as typed so far, or `fallback` if there
        /// is no field to read, as when RmlUi is not running.
        std::string field_text( const std::string &fallback ) const;

        bool handle_custom_callbacks( const std::string &action );

        input_context ctxt;
        std::string label;
        nc_color label_default_color = c_light_red;
        int max_input_length = 0;
        bool is_cancelled = false;
        std::vector<std::pair<callback_input, std::function<bool()>>> callbacks;

    private:
        // Set whenever anything shown, other than the field's text, changes.
        bool content_dirty = true;
        std::optional<std::string> pending_text;

        std::string title;
        std::string description;
        nc_color description_default_color = c_green;
        int width;
        bool description_monofont = false;
};

class string_input_popup_imgui : public input_popup
{
    public:
        explicit string_input_popup_imgui( int width, const std::string &old_input = "",
                                           const std::string &title = "" );

        std::string query();
        // tries to parse input into a tripoint_abs_omt
        std::optional<tripoint_abs_omt> query_coordinate( bool loop = false );
        void set_identifier( const std::string &ident );
        void set_text( const std::string &txt );
        void use_uilist_history( bool use_uilist );
    private:
        void add_to_history( const std::string &value ) const;
        void show_history();
        // Step through the history in the field itself, when not using the uilist.
        void step_history( bool up );
        std::string text;
        // currently entered input for history handling
        std::string session_input;
        std::string old_input;
        std::string identifier;
        bool is_uilist_history = true;
        uint64_t max_history_size = 100;
        int history_index = 0;
};

// todo: make generic for any numeric type?
template<typename T>
class number_input_popup : public input_popup
{
    public:
        explicit number_input_popup( int width, T old_value = 0, const std::string &title = "" );
        T query();
        void set_step_size( std::optional<T> step, std::optional<T> fast_step );
    protected:
        void on_redraw() override;
    private:
        T step_amount( bool fast ) const;
        void step_by( T amount );
        // Parse the field into `value`, keeping the last good value if it does not parse.
        void read_field();
        T value;
        T old_value;
        std::optional<T> step_size = std::nullopt;
        std::optional<T> fast_step_size = std::nullopt;
};
#endif // CATA_SRC_INPUT_POPUP_H

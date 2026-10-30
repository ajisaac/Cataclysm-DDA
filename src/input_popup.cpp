#include "input_popup.h"

#include <cstddef>
#include <locale>
#include <sstream>
#include <type_traits>

#include "coordinates.h"
#include "input_enums.h"
#include "ret_val.h"
#include "rml_markup.h"
#include "string_formatter.h"
#include "translations.h"
#include "try_parse_integer.h"
#include "uilist.h"
#include "ui_manager.h"
#include "uistate.h"

// Roughly the width of a digit in the popup's font, to turn the character
// widths callers give into something RmlUi can lay out.
static constexpr float char_width_em = 0.56f;

input_popup::input_popup( int width, const std::string &title ) :
    rml_ui::document( "input_popup.rml" ),
    title( title ),
    width( width )
{
    ctxt = input_context( "STRING_INPUT", keyboard_mode::keychar );
    ctxt.register_action( "TEXT.CONFIRM" );
    ctxt.register_action( "TEXT.QUIT" );
    ctxt.register_action( "TEXT.CLEAR" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.set_timeout( 10 );
    focus( "input" );
}

void input_popup::set_description( const std::string &desc, const nc_color &default_color,
                                   bool monofont )
{
    description = desc;
    description_default_color = default_color;
    description_monofont = monofont;
    content_dirty = true;
}

void input_popup::set_label( const std::string &label, const nc_color &default_color )
{
    this->label = label;
    label_default_color = default_color;
    content_dirty = true;
}

void input_popup::set_max_input_length( int length )
{
    max_input_length = length;
    content_dirty = true;
}

int input_popup::get_max_input_length() const
{
    return max_input_length;
}

static void register_input( input_context &ctxt, const callback_input &input )
{
    if( input.description ) {
        ctxt.register_action( input.action, *input.description );
    } else {
        ctxt.register_action( input.action );
    }
}

void input_popup::add_callback( const callback_input &input,
                                const std::function<bool()> &callback_func )
{
    if( !input.action.empty() ) {
        register_input( ctxt, input );
    }
    callbacks.emplace_back( input, callback_func );
}

bool input_popup::cancelled() const
{
    return is_cancelled;
}

void input_popup::on_redraw()
{
    if( content_dirty ) {
        content_dirty = false;
        set_class( "title", "hidden", title.empty() );
        set_inner_rml( "title", rml_ui::markup_to_rml( title ) );
        set_class( "description", "hidden", description.empty() );
        set_class( "description", "mono", description_monofont );
        set_inner_rml( "description",
                       rml_ui::markup_to_rml( description, description_default_color ) );
        set_class( "label", "hidden", label.empty() );
        set_inner_rml( "label", rml_ui::markup_to_rml( label, label_default_color ) );
        set_attribute( "popup", "style",
                       width > 0 ? string_format( "width: %.1fem;", width * char_width_em ) : "" );
        set_attribute( "input", "maxlength",
                       max_input_length > 0 ? std::to_string( max_input_length ) : "-1" );
        // Shrink the field to match a short limit.
        set_attribute( "input", "style", max_input_length > 0 ?
                       string_format( "flex: 0 0 auto; width: %.1fem;",
                                      ( max_input_length + 1 ) * char_width_em ) : "" );
    }
    if( pending_text ) {
        set_value( "input", *pending_text );
        pending_text.reset();
    }
}

void input_popup::show_text( const std::string &text )
{
    pending_text = text;
}

std::string input_popup::field_text( const std::string &fallback ) const
{
    if( pending_text ) {
        return *pending_text;
    }
    return get_value( "input" ).value_or( fallback );
}

bool input_popup::handle_custom_callbacks( const std::string &action )
{

    input_event ev = ctxt.get_raw_input();
    int key = ev.get_first_input();

    bool next_loop = false;
    for( const auto &cb : callbacks ) {
        if( cb.first == callback_input( action, key ) && cb.second() ) {
            next_loop = true;
            break;
        }
    }

    return next_loop;
}

text_input_popup::text_input_popup( int width, const std::string &old_input,
        const std::string &title ) :
    input_popup( width, title ),
    text( old_input ),
    old_input( old_input )
{
    // Up opens the uilist history, or with it off, up and down step through
    // the history in the field
    ctxt.register_action( "HISTORY_UP" );
    ctxt.register_action( "HISTORY_DOWN" );
    show_text( text );
}

void text_input_popup::set_identifier( const std::string &ident )
{
    identifier = ident;
}

void text_input_popup::set_text( const std::string &txt )
{
    text = txt;
    show_text( txt );
}

void text_input_popup::show_history()
{
    if( identifier.empty() ) {
        return;
    }
    std::vector<std::string> &hist = uistate.gethistory( identifier );
    uilist hmenu;
    hmenu.title = _( "d: delete history" );
    hmenu.allow_anykey = true;
    for( size_t h = 0; h < hist.size(); h++ ) {
        hmenu.addentry( h, true, -2, hist[h] );
    }
    if( !text.empty() && ( hmenu.entries.empty() ||
                           hmenu.entries[hist.size() - 1].txt != text ) ) {
        hmenu.addentry( hist.size(), true, -2, text );
    }

    if( !hmenu.entries.empty() ) {
        hmenu.selected = hmenu.entries.size() - 1;

        bool finished = false;
        do {
            hmenu.query();
            if( hmenu.ret >= 0 && hmenu.entries[hmenu.ret].txt != text ) {
                set_text( hmenu.entries[hmenu.ret].txt );
                if( static_cast<size_t>( hmenu.ret ) < hist.size() ) {
                    hist.erase( hist.begin() + hmenu.ret );
                    add_to_history( text );
                }
                finished = true;
            } else if( hmenu.ret == UILIST_UNBOUND && hmenu.ret_evt.get_first_input() == 'd' ) {
                hist.clear();
                finished = true;
            } else if( hmenu.ret != UILIST_UNBOUND ) {
                finished = true;
            }
        } while( !finished );
    }
}

void text_input_popup::step_history( bool up )
{
    if( identifier.empty() ) {
        return;
    }

    std::vector<std::string> &hist = uistate.gethistory( identifier );

    if( hist.empty() ) {
        return;
    }

    if( hist.size() >= max_history_size ) {
        hist.erase( hist.begin(), hist.begin() + ( hist.size() - max_history_size ) );
    }

    if( up ) {
        if( history_index >= static_cast<int>( hist.size() ) ) {
            return;
        } else if( history_index == 0 ) {
            session_input = text;

            //avoid showing the same result twice (after reopen filter window without reset)
            if( hist.size() > 1 && session_input == hist.back() ) {
                history_index += 1;
            }
        }
    } else {
        if( history_index == 1 ) {
            set_text( session_input );
            //show initial string entered and 'return'
            history_index = 0;
            return;
        } else if( history_index == 0 ) {
            return;
        }
    }

    history_index += up ? 1 : -1;
    set_text( hist[hist.size() - history_index] );
}

void text_input_popup::add_to_history( const std::string &value ) const
{
    if( !identifier.empty() && !value.empty() ) {
        std::vector<std::string> &hist = uistate.gethistory( identifier );
        if( hist.empty() || hist[hist.size() - 1] != value ) {
            hist.push_back( value );
        }
    }
}

std::string text_input_popup::query()
{
    is_cancelled = false;

    while( true ) {
        ui_manager::redraw_invalidated();

        std::string action = ctxt.handle_input();
        // RmlUi has already applied any typing to the field
        text = field_text( text );
        if( handle_custom_callbacks( action ) ) {
            continue;
        }

        if( action == "TEXT.CONFIRM" ) {
            add_to_history( text );
            return text;
        } else if( action == "TEXT.QUIT" ) {
            break;
        } else if( action == "HISTORY_UP" && is_uilist_history ) {
            show_history();
        } else if( ( action == "HISTORY_UP" || action == "HISTORY_DOWN" ) && !is_uilist_history ) {
            step_history( action == "HISTORY_UP" );
        } else if( action == "TEXT.CLEAR" ) {
            set_text( std::string() );
        }
    }

    is_cancelled = true;
    return old_input;
}

std::optional<tripoint_abs_omt> text_input_popup::query_coordinate( bool loop )
{
    do {
        const std::string &queried_string = query();
        ret_val<tripoint_abs_omt> result = try_parse_coordinate_abs( queried_string );
        if( cancelled() || queried_string.empty() ) {
            return std::nullopt;
        }
        if( result.success() ) {
            return result.value();
        }
    } while( loop );

    return tripoint_abs_omt::zero;
}

void text_input_popup::use_uilist_history( bool use_uilist )
{
    is_uilist_history = use_uilist;
}

template<typename T>
static std::string format_number( T value )
{
    if constexpr( std::is_integral_v<T> ) {
        return std::to_string( value );
    } else {
        return string_format( "%g", value );
    }
}

template<typename T>
static std::optional<T> parse_number( const std::string &text )
{
    if constexpr( std::is_integral_v<T> ) {
        const ret_val<T> parsed = try_parse_integer<T>( text, false );
        if( parsed.success() ) {
            return parsed.value();
        }
        return std::nullopt;
    } else {
        std::istringstream conv( text );
        conv.imbue( std::locale::classic() );
        T parsed = 0;
        conv >> parsed;
        if( conv && ( conv >> std::ws ).eof() ) {
            return parsed;
        }
        return std::nullopt;
    }
}

template<typename T>
number_input_popup<T>::number_input_popup( int width, T old_value, const std::string &title ) :
    input_popup( width, title ),
    value( old_value ),
    old_value( old_value )
{
    // Up and down step the value, page up and down by the fast step, and so
    // does clicking the step buttons
    ctxt.register_action( "HISTORY_UP" );
    ctxt.register_action( "HISTORY_DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "SELECT" );
    show_text( format_number( value ) );
}

template<typename T>
T number_input_popup<T>::step_amount( bool fast ) const
{
    // The defaults the older popup had: 1 and 100 for whole numbers, and no
    // stepping for floats.
    if constexpr( std::is_integral_v<T> ) {
        return fast ? fast_step_size.value_or( 100 ) : step_size.value_or( 1 );
    } else {
        return fast ? fast_step_size.value_or( 0 ) : step_size.value_or( 0 );
    }
}

template<typename T>
void number_input_popup<T>::step_by( T amount )
{
    if( amount == 0 ) {
        return;
    }
    value += amount;
    show_text( format_number( value ) );
}

template<typename T>
void number_input_popup<T>::read_field()
{
    if( std::optional<T> parsed = parse_number<T>( field_text( format_number( value ) ) ) ) {
        value = *parsed;
    }
}

template<typename T>
void number_input_popup<T>::on_redraw()
{
    input_popup::on_redraw();
    set_class( "steps", "hidden", step_amount( false ) == 0 );
}

template<typename T>
T number_input_popup<T>::query()
{
    is_cancelled = false;

    while( true ) {
        ui_manager::redraw_invalidated();
        std::string action = ctxt.handle_input();
        // RmlUi has already applied any typing to the field
        read_field();

        if( handle_custom_callbacks( action ) ) {
            continue;
        }

        if( action == "TEXT.CONFIRM" ) {
            return value;
        } else if( action == "TEXT.QUIT" ) {
            break;
        } else if( action == "TEXT.CLEAR" ) {
            show_text( std::string() );
        } else if( action == "HISTORY_UP" || action == "HISTORY_DOWN" ) {
            step_by( action == "HISTORY_UP" ? step_amount( false ) : -step_amount( false ) );
        } else if( action == "PAGE_UP" || action == "PAGE_DOWN" ) {
            step_by( action == "PAGE_UP" ? step_amount( true ) : -step_amount( true ) );
        } else if( action == "SELECT" ) {
            // The step buttons are "−" then "+"
            const int button = hovered_child( "steps" );
            if( button >= 0 ) {
                step_by( button == 0 ? -step_amount( false ) : step_amount( false ) );
            }
        }
    }

    is_cancelled = true;
    return old_value;
}

template<typename T>
void number_input_popup<T>::set_step_size( std::optional<T> step, std::optional<T> fast_step )
{
    step_size = step;
    fast_step_size = fast_step;
}

template class number_input_popup<int>;
template class number_input_popup<float>;

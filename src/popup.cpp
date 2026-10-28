#include "popup.h"

#include <array>
#include <memory>

#include "cached_options.h"
#include "color.h"
#include "input_context.h"
#include "rml_markup.h"
#include "rml_ui.h"
#include "string_formatter.h"
#include "ui_manager.h"

class query_popup_impl : public rml_ui::document
{
        query_popup *parent;
        // Set by query_popup whenever its message, options or layout change.
        bool content_dirty = true;
        std::vector<std::string> labels;
        short shown_selection = -1;

        std::vector<std::string> option_labels() const;
        static std::string button_id( int ind );
        void show_buttons();

    public:
        short keyboard_selected_option = 0;

        explicit query_popup_impl( query_popup *parent ) :
            rml_ui::document( "query_popup.rml" ), parent( parent ) {}

        /// Rebuild the message and buttons on the next redraw.
        void mark_dirty() {
            content_dirty = true;
        }

        int get_mouse_selected_option() const {
            return hovered_child( "buttons" );
        }

    protected:
        void on_redraw() override;
};

void query_popup_impl::on_redraw()
{
    if( content_dirty ) {
        content_dirty = false;
        set_class( "root", "on-top", parent->ontop );
        set_class( "root", "full-screen", parent->fullscr );
        set_inner_rml( "message", "<span class=\"" +
                       rml_ui::color_to_rml_classes( parent->default_text_color ) + "\">" +
                       rml_ui::markup_to_rml( parent->text ) + "</span>" );
        labels = option_labels();
        show_buttons();
    }
    // Moved with a class rather than by rebuilding the buttons, which would
    // also lose the mouse's hover.
    if( keyboard_selected_option != shown_selection ) {
        set_class( button_id( shown_selection ), "selected", false );
        set_class( button_id( keyboard_selected_option ), "selected", true );
        shown_selection = keyboard_selected_option;
    }
}

std::vector<std::string> query_popup_impl::option_labels() const
{
    input_context ctxt( parent->category, parent->pref_kbd_mode );
    std::vector<std::string> ret;
    ret.reserve( parent->options.size() );
    for( const query_popup::query_option &opt : parent->options ) {
        ret.emplace_back( ctxt.get_desc( opt.action, ctxt.get_action_name( opt.action ),
                                         opt.filter ) );
    }
    return ret;
}

std::string query_popup_impl::button_id( int ind )
{
    return "button-" + std::to_string( ind );
}

void query_popup_impl::show_buttons()
{
    std::string rml;
    for( size_t ind = 0; ind < labels.size(); ++ind ) {
        rml += "<div class=\"button\" id=\"" + button_id( static_cast<int>( ind ) ) + "\">";
        rml += rml_ui::markup_to_rml( labels[ind] );
        rml += "</div>";
    }
    set_inner_rml( "buttons", rml );
    // The new buttons carry no selection yet.
    shown_selection = -1;
}

query_popup::query_popup()
    : cur( 0 ), default_text_color( c_white ), anykey( false ), cancel( false ),
      ontop( false ), fullscr( false ), pref_kbd_mode( keyboard_mode::keycode )
{
}

query_popup &query_popup::context( const std::string &cat )
{
    invalidate_ui();
    category = cat;
    return *this;
}

query_popup &query_popup::option( const std::string &opt )
{
    invalidate_ui();
    options.emplace_back( opt, []( const input_event & ) {
        return true;
    } );
    return *this;
}

query_popup &query_popup::option( const std::string &opt,
                                  const std::function<bool( const input_event & )> &filter )
{
    invalidate_ui();
    options.emplace_back( opt, filter );
    return *this;
}

query_popup &query_popup::allow_anykey( bool allow )
{
    // Change does not affect cache, do not invalidate the window
    anykey = allow;
    return *this;
}

query_popup &query_popup::allow_cancel( bool allow )
{
    // Change does not affect cache, do not invalidate the window
    cancel = allow;
    return *this;
}

query_popup &query_popup::on_top( bool top )
{
    invalidate_ui();
    ontop = top;
    return *this;
}

query_popup &query_popup::full_screen( bool full )
{
    invalidate_ui();
    fullscr = full;
    return *this;
}

query_popup &query_popup::cursor( size_t pos )
{
    // Change does not affect cache, do not invalidate window
    cur = pos;
    std::shared_ptr<query_popup_impl> impl = p_impl.lock();
    if( impl ) {
        impl->keyboard_selected_option = short( cur );
    }
    return *this;
}

query_popup &query_popup::default_color( const nc_color &d_color )
{
    // The message is converted with this color, so it has to be rebuilt
    invalidate_ui();
    default_text_color = d_color;
    return *this;
}

query_popup &query_popup::preferred_keyboard_mode( const keyboard_mode mode )
{
    invalidate_ui();
    pref_kbd_mode = mode;
    return *this;
}

void query_popup::invalidate_ui() const
{
    std::shared_ptr<query_popup_impl> ui = p_impl.lock();
    if( ui ) {
        ui->mark_dirty();
    }
}

query_popup::result query_popup::query_once()
{

    if( !anykey && !cancel && options.empty() ) {
        return { false, "ERROR", {} };
    }

    if( test_mode ) {
        return { false, "ERROR", {} };
    }

    std::shared_ptr<query_popup_impl> impl = create_or_get_impl();

    input_context ctxt( category, pref_kbd_mode );
    if( cancel || !options.empty() ) {
        ctxt.register_action( "HELP_KEYBINDINGS" );
    }
    if( !options.empty() ) {
        ctxt.register_action( "CONFIRM" );
        for( const query_popup::query_option &opt : options ) {
            ctxt.register_action( opt.action );
        }
        // Mouse movement and button
        ctxt.register_action( "SELECT" );
        ctxt.register_action( "MOUSE_MOVE" );
        ctxt.register_action( "LEFT" );
        ctxt.register_action( "RIGHT" );
    }
    if( anykey ) {
        ctxt.register_action( "ANY_INPUT" );
        // Mouse movement, button, and wheel
        ctxt.register_action( "COORDINATE" );
    }
    if( cancel ) {
        ctxt.register_action( "QUIT" );
    }

    result res;
    // Assign outside construction of `res` to ensure execution order
    res.wait_input = !anykey;
    do {
        ui_manager::redraw();
        res.action = ctxt.handle_input( 50 );
        res.evt = ctxt.get_raw_input();

        // If we're tracking mouse movement
        if( !options.empty() && res.action == "SELECT" && impl->get_mouse_selected_option() != -1 ) {
            // Left-click to confirm selection
            res.action = "CONFIRM";
            cur = size_t( impl->get_mouse_selected_option() );
        } else if( res.action == "CONFIRM" && impl->keyboard_selected_option != -1 ) {
            cur = size_t( impl->keyboard_selected_option );
        }
    } while(
        // Always ignore mouse movement
        ( res.evt.type == input_event_t::mouse &&
          res.evt.get_first_input() == static_cast<int>( MouseInput::Move ) ) ||
        // Ignore window losing focus in SDL
        ( res.evt.type == input_event_t::keyboard_char && res.evt.sequence.empty() ) ||
        res.evt.type == input_event_t::timeout
    );

    if( cancel && res.action == "QUIT" ) {
        res.wait_input = false;
    } else if( res.action == "CONFIRM" ) {
        if( cur < options.size() ) {
            res.wait_input = false;
            res.action = options[cur].action;
        }
    } else if( res.action == "LEFT" ) {
        if( impl->keyboard_selected_option > 0 ) {
            impl->keyboard_selected_option--;
        } else {
            impl->keyboard_selected_option = short( options.size() - 1 );
        }
    } else if( res.action == "RIGHT" ) {
        if( impl->keyboard_selected_option < short( options.size() - 1 ) ) {
            impl->keyboard_selected_option++;
        } else {
            impl->keyboard_selected_option = 0;
        }
    } else if( res.action == "HELP_KEYBINDINGS" ) {
        // Keybindings may have changed, regenerate the labels
        impl->mark_dirty();
    } else {
        for( size_t ind = 0; ind < options.size(); ++ind ) {
            if( res.action == options[ind].action ) {
                impl->keyboard_selected_option = ind;
                if( options[ind].filter( res.evt ) ) {
                    res.wait_input = false;
                    break;
                }
            }
        }
    }

    return res;
}

std::shared_ptr<query_popup_impl> query_popup::create_or_get_impl()
{
    std::shared_ptr<query_popup_impl> impl = p_impl.lock();
    if( !impl ) {
        p_impl = impl = std::make_shared<query_popup_impl>( this );
        impl->keyboard_selected_option = short( cur );
    }
    return impl;
}
query_popup::result query_popup::query()
{
    std::shared_ptr<query_popup_impl> ui = create_or_get_impl();

    result res;
    do {
        res = query_once();
    } while( res.wait_input );
    return res;
}

std::string query_popup::wait_text( const std::string &text, const nc_color &bar_color )
{
    static const std::array<std::string, 4> phase_icons = {{ "|", "/", "-", "\\" }};
    static size_t phase = phase_icons.size() - 1;
    phase = ( phase + 1 ) % phase_icons.size();
    return string_format( " %s %s", colorize( phase_icons[phase], bar_color ), text );
}

std::string query_popup::wait_text( const std::string &text )
{
    return wait_text( text, c_light_green );
}

query_popup::result::result()
    : wait_input( false ), action( "ERROR" )
{
}

query_popup::result::result( bool wait_input, const std::string &action, const input_event &evt )
    : wait_input( wait_input ), action( action ), evt( evt )
{
}

query_popup::query_option::query_option(
    const std::string &action,
    const std::function<bool( const input_event & )> &filter )
    : action( action ), filter( filter )
{
}

static_popup::static_popup()
{
    ui = create_or_get_impl();
}

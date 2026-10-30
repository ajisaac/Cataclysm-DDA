#include "uilist.h"

#include <cctype>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <set>

#include "avatar.h"
#include "cached_options.h" // IWYU pragma: keep
#include "cata_assert.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "game.h"
#include "input.h"
#include "memory_fast.h"
#include "output.h"
#include "input_popup.h"
#include "rml_markup.h"
#include "rml_window.h"
#include "translations.h"
#include "ui_manager.h"

#if defined(__ANDROID__)
#include <jni.h>
#include "options.h"
#endif

#if !defined(__ANDROID__)
#include "sdl_wrappers.h"
#endif

// The height of a row, as `#list .entry` has it in uilist.rml.
static constexpr float row_height_dp = 24.f;
// Roughly a line of the menu's other text, for estimating the space the
// title, text and description take from the rows.
static constexpr float line_height_dp = 20.f;

static int line_count( const std::string &text )
{
    return static_cast<int>( std::count( text.begin(), text.end(), '\n' ) ) + 1;
}

uilist_impl::uilist_impl( uilist &parent ) :
    rml_ui::document( "uilist.rml" ),
    parent( parent )
{
}

void uilist_impl::mark_resized()
{
    layout_dirty = true;
}

int uilist_impl::hovered_entry() const
{
    const int row = hovered_child( "list" );
    const int fentry = parent.vshift + row;
    return row >= 0 && fentry < static_cast<int>( parent.fentries.size() ) ? fentry : -1;
}

int uilist_impl::hovered_category() const
{
    return hovered_child( "tabs" );
}

bool uilist_impl::hovering_panel() const
{
    // #body holds #left, #list-column and #right, in that order
    const int part = hovered_child( "body" );
    return part == 0 || part == 2;
}

void uilist_impl::show( const std::string &id, const std::string &rml )
{
    std::string &last = shown[id];
    if( last != rml ) {
        last = rml;
        set_inner_rml( id, rml );
    }
}

void uilist_impl::apply_layout()
{
    const point view = rml_ui::viewport_px();
    const float dp = rml_ui::dp_ratio();

    // desired_bounds is in terminal cells: a negative position centers on
    // that axis, and a size of at most 1 on both axes is a fraction of the
    // screen.
    std::string root_style;
    std::string menu_style;
    // As tall as the menu lets itself get
    float menu_height_px = 0.9f * view.y;
    if( parent.desired_bounds ) {
        const uilist_bounds &b = *parent.desired_bounds;
        if( b.x >= 0.f ) {
            root_style += "justify-content: flex-start;";
            menu_style += "margin-left: " + rml_ui::term_width( b.x ) + ";";
        }
        if( b.y >= 0.f ) {
            root_style += "align-items: flex-start;";
            menu_style += "margin-top: " + rml_ui::term_height( b.y ) + ";";
        }
        if( b.w > 0.f && b.w <= 1.f && b.h > 0.f && b.h <= 1.f ) {
            menu_style += string_format( "width: %.0f%%; height: %.0f%%; max-width: none;",
                                         b.w * 100.f, b.h * 100.f );
            menu_height_px = b.h * view.y;
        } else {
            if( b.w > 1.f ) {
                menu_style += "width: " + rml_ui::term_width( b.w ) + "; max-width: none;";
            }
            if( b.h > 1.f ) {
                menu_style += "height: " + rml_ui::term_height( b.h ) + ";";
                menu_height_px = std::min( b.h / std::max( 1, TERMY ), 0.95f ) * view.y;
            }
        }
    }
    set_attribute( "root", "style", root_style );
    set_attribute( "menu", "style", menu_style );

    // Rows that fit: the menu's height, less an estimate of everything else
    // in it. Lines here are unwrapped, so a long wrapped text can still push
    // the bottom row out of sight.
    int other_lines = 2;
    if( !parent.title.empty() ) {
        other_lines += 2;
    }
    if( !parent.text.empty() ) {
        other_lines += line_count( parent.text ) + 1;
    }
    if( !parent.categories.empty() ) {
        other_lines += 2;
    }
    if( parent.desc_enabled ) {
        int desc_lines = line_count( parent.footer_text );
        for( const uilist_entry &entry : parent.entries ) {
            desc_lines = std::max( desc_lines, line_count( entry.desc ) );
        }
        other_lines += desc_lines + 1;
    }
    const float rows_px = menu_height_px - other_lines * line_height_dp * dp;
    const int rows = std::max( 1, static_cast<int>( rows_px / ( row_height_dp * dp ) ) );
    parent.vmax = std::min( static_cast<int>( parent.entries.size() ), rows );
}

std::string uilist_impl::rows_rml() const
{
    const uilist &menu = parent;
    std::string rml;
    const int end = std::min( menu.vshift + menu.vmax, static_cast<int>( menu.fentries.size() ) );
    for( int fentry = menu.vshift; fentry < end; ++fentry ) {
        const uilist_entry &entry = menu.entries[menu.fentries[fentry]];
        const nc_color color = entry.enabled || entry.force_color ? entry.text_color :
                               menu.disabled_color;
        rml += fentry == menu.fselected ? "<div class=\"entry selected\">" :
               "<div class=\"entry\">";
        rml += "<span class=\"hotkey\">";
        if( entry.hotkey ) {
            rml += rml_ui::markup_to_rml( entry.hotkey->short_description(), menu.hotkey_color );
        }
        rml += "</span><span class=\"label\">";
        rml += rml_ui::markup_to_rml( entry.txt, color, menu._color_error );
        rml += "</span>";
        if( !entry.ctxt.empty() ) {
            rml += "<span class=\"column\">";
            rml += rml_ui::markup_to_rml( entry.ctxt, color, menu._color_error );
            rml += "</span>";
        }
        rml += "</div>";
    }
    return rml;
}

void uilist_impl::on_redraw()
{
    uilist &menu = parent;
    if( !menu.started ) {
        menu.setup();
    }
    if( menu.callback != nullptr ) {
        menu.callback->update( &menu );
    }

    // A tab picked with the keyboard or mouse since the last redraw
    if( menu.switch_to_category < menu.categories.size() &&
        menu.switch_to_category != menu.current_category ) {
        menu.current_category = menu.switch_to_category;
        menu.filterlist();
    }
    menu.switch_to_category = -1;

    if( layout_dirty ) {
        layout_dirty = false;
        apply_layout();
    }

    // Keep the selection among the rows shown
    const int count = static_cast<int>( menu.fentries.size() );
    if( menu.fselected < menu.vshift ) {
        menu.vshift = menu.fselected;
    } else if( menu.fselected >= menu.vshift + menu.vmax ) {
        menu.vshift = menu.fselected - menu.vmax + 1;
    }
    menu.vshift = std::clamp( menu.vshift, 0, std::max( 0, count - menu.vmax ) );
    menu.need_to_scroll = false;

    set_class( "root", "hidden", hidden );

    set_class( "title", "hidden", menu.title.empty() );
    show( "title", rml_ui::markup_to_rml( menu.title, menu._color_error ) );
    set_class( "text", "hidden", menu.text.empty() );
    show( "text", rml_ui::markup_to_rml( menu.text, menu._color_error ) );

    set_class( "tabs", "hidden", menu.categories.empty() );
    std::string tabs;
    for( size_t i = 0; i < menu.categories.size(); ++i ) {
        tabs += i == menu.current_category ? "<div class=\"tab active\">" : "<div class=\"tab\">";
        tabs += rml_ui::markup_to_rml( menu.categories[i].second ) + "</div>";
    }
    show( "tabs", tabs );

    show( "list", rows_rml() );
    set_class( "position", "hidden", count <= menu.vmax );
    show( "position", count <= menu.vmax ? std::string() :
          string_format( _( "%1$d-%2$d of %3$d" ), menu.vshift + 1,
                         std::min( menu.vshift + menu.vmax, count ), count ) );

    std::string left;
    std::string right;
    std::string right_footer;
    if( menu.callback != nullptr ) {
        left = menu.callback->left_panel_rml( &menu );
        right = menu.callback->right_panel_rml( &menu );
        right_footer = menu.callback->right_panel_footer_rml( &menu );
    }
    set_class( "left", "hidden", left.empty() );
    show( "left-content", left );
    set_class( "right", "hidden", right.empty() && right_footer.empty() );
    set_class( "body", "with-right", !right.empty() || !right_footer.empty() );
    show( "right-content", right );
    set_class( "right-footer", "hidden", right_footer.empty() );
    show( "right-footer", right_footer );
    // After the content, which a new selection may just have replaced
    if( pending_scroll ) {
        scroll( "right-content", *pending_scroll );
        pending_scroll.reset();
    }

    std::string description;
    if( menu.desc_enabled ) {
        if( !menu.footer_text.empty() ) {
            description = menu.footer_text;
        } else if( menu.previewing >= 0 &&
                   menu.previewing < static_cast<int>( menu.entries.size() ) ) {
            description = menu.entries[menu.previewing].desc;
        }
    }
    set_class( "desc", "hidden", !menu.desc_enabled );
    show( "desc", rml_ui::markup_to_rml( description, menu._color_error ) );
}

catacurses::window new_centered_win( int nlines, int ncols )
{
    int height = std::min( nlines, TERMY );
    int width = std::min( ncols, TERMX );
    point pos( ( TERMX - width ) / 2, ( TERMY - height ) / 2 );
    return catacurses::newwin( height, width, pos );
}

/**
* \defgroup UI "The UI Menu."
* @{
*/

static std::optional<input_event> hotkey_from_char( const int ch )
{
    if( ch == MENU_AUTOASSIGN ) {
        return std::nullopt;
    } else if( ch <= 0 || ch == ' ' ) {
        return input_event();
    }
    switch( input_manager::actual_keyboard_mode( keyboard_mode::keycode ) ) {
        case keyboard_mode::keycode:
            if( ch >= 'A' && ch <= 'Z' ) {
                return input_event( std::set<keymod_t>( { keymod_t::shift } ),
                                    ch - 'A' + 'a', input_event_t::keyboard_code );
            } else {
                return input_event( ch, input_event_t::keyboard_code );
            }
        case keyboard_mode::keychar:
            return input_event( ch, input_event_t::keyboard_char );
    }
    return input_event();
}

uilist_entry::uilist_entry( const std::string &txt )
    : retval( -1 ), enabled( true ), hotkey( std::nullopt ), txt( txt ),
      text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const std::string &txt, const std::string &desc )
    : retval( -1 ), enabled( true ), hotkey( std::nullopt ), txt( txt ),
      desc( desc ), text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const std::string &txt, const int key )
    : retval( -1 ), enabled( true ), hotkey( hotkey_from_char( key ) ), txt( txt ),
      text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const std::string &txt, const std::optional<input_event> &key )
    : retval( -1 ), enabled( true ), hotkey( key ), txt( txt ),
      text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled, const int key,
                            const std::string &txt )
    : retval( retval ), enabled( enabled ), hotkey( hotkey_from_char( key ) ), txt( txt ),
      text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled,
                            const std::optional<input_event> &key,
                            const std::string &txt )
    : retval( retval ), enabled( enabled ), hotkey( key ), txt( txt ),
      text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled, const int key,
                            const std::string &txt, const std::string &desc )
    : retval( retval ), enabled( enabled ), hotkey( hotkey_from_char( key ) ), txt( txt ),
      desc( desc ), text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled,
                            const std::optional<input_event> &key, const std::string &txt, const std::string &desc )
    : retval( retval ), enabled( enabled ), hotkey( key ), txt( txt ),
      desc( desc ), text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled, const int key,
                            const std::string &txt, const std::string &desc,
                            const std::string &column )
    : retval( retval ), enabled( enabled ), hotkey( hotkey_from_char( key ) ), txt( txt ),
      desc( desc ), ctxt( column ), text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled,
                            const std::optional<input_event> &key,
                            const std::string &txt, const std::string &desc,
                            const std::string &column )
    : retval( retval ), enabled( enabled ), hotkey( key ), txt( txt ),
      desc( desc ), ctxt( column ), text_color( c_red_red )
{

}

uilist_entry::uilist_entry( const int retval, const bool enabled, const int key,
                            const std::string &txt,
                            const nc_color &keycolor, const nc_color &txtcolor )
    : retval( retval ), enabled( enabled ), hotkey( hotkey_from_char( key ) ), txt( txt ),
      hotkey_color( keycolor ), text_color( txtcolor )
{

}

const std::string &uilist_entry::_txt_nocolor()
{
    if( _txt_nocolor_cached.empty() && !txt.empty() ) {
        _txt_nocolor_cached = remove_color_tags( txt );
    }
    return _txt_nocolor_cached;
}

const std::string &uilist_entry::_ctxt_nocolor()
{
    if( _ctxt_nocolor_cached.empty() && !ctxt.empty() ) {
        _ctxt_nocolor_cached = remove_color_tags( ctxt );
    }
    return _ctxt_nocolor_cached;
}

uilist::size_scalar &uilist::size_scalar::operator=( auto_assign )
{
    fun = nullptr;
    return *this;
}

uilist::size_scalar &uilist::size_scalar::operator=( const int val )
{
    fun = [val]() -> int {
        return val;
    };
    return *this;
}

uilist::size_scalar &uilist::size_scalar::operator=( const std::function<int()> &fun )
{
    this->fun = fun;
    return *this;
}

uilist::pos_scalar &uilist::pos_scalar::operator=( auto_assign )
{
    fun = nullptr;
    return *this;
}

uilist::pos_scalar &uilist::pos_scalar::operator=( const int val )
{
    fun = [val]( int ) -> int {
        return val;
    };
    return *this;
}

uilist::pos_scalar &uilist::pos_scalar::operator=( const std::function<int( int )> &fun )
{
    this->fun = fun;
    return *this;
}

uilist::uilist()
{
    init();
}

uilist::uilist( const std::string &msg, const std::vector<uilist_entry> &opts )
{
    init();
    text = msg;
    entries = opts;
    query();
}

uilist::uilist( const std::string &msg, const std::vector<std::string> &opts )
{
    init();
    text = msg;
    for( const std::string &opt : opts ) {
        entries.emplace_back( opt );
    }
    query();
}

uilist::uilist( const std::string &msg, std::initializer_list<const char *const> opts )
{
    init();
    text = msg;
    for( const char *const opt : opts ) {
        entries.emplace_back( opt );
    }
    query();
}

uilist::~uilist() = default;

void uilist::color_error( const bool report )
{
    if( report ) {
        _color_error = report_color_error::yes;
    } else {
        _color_error = report_color_error::no;
    }
}

/*
 * Enables oneshot construction -> running -> exit
 */
uilist::operator int() const
{
    return ret;
}

/**
 * Sane defaults on initialization
 */
void uilist::init()
{
    cata_assert( !test_mode ); // uilist should not be used in tests where there's no place for it
    desired_bounds = std::nullopt;
    ret = UILIST_WAIT_INPUT;
    text.clear();          // header text, after (maybe) folding, populates:
    title.clear();         // Makes use of the top border, no folding, sets min width if w_width is auto
    ret_evt = input_event(); // last input event
    keymap.clear();        // keymap[input_event] == index, for entries[index]
    selected = 0;          // current highlight, for entries[index]
    previewing = 0;           // current mouse highlight, for entries[index]
    entries.clear();       // uilist_entry(int returnval, bool enabled, int keycode, std::string text, ... TODO: submenu stuff)
    started = false;       // set to true when width and key calculations are done, and window is generated.
    desc_enabled = false;  // don't show option description by default
    footer_text.clear();   // takes precedence over per-entry descriptions.
    border_color = c_magenta; // border color
    text_color = c_light_gray;  // text color
    title_color = c_green;  // title color
    hilight_color = h_white; // highlight for up/down selection bar
    hotkey_color = c_light_green; // hotkey text to the right of menu entry's text
    disabled_color = c_dark_gray; // disabled menu entry
    allow_disabled = false;  // disallow selecting disabled options
    allow_anykey = false;    // do not return on unbound keys
    allow_cancel = true;     // allow canceling with "UILIST.QUIT" action
    allow_confirm = true;     // allow confirming with confirm action
    allow_additional = false; // do not return on unhandled additional actions
    hilight_disabled =
        false; // if false, hitting 'down' onto a disabled entry will advance downward to the first enabled entry
    vshift = 0;              // scrolling menu offset
    vmax = 0;                // max entries area rows
    callback = nullptr;         // * uilist_callback
    filter.clear();          // filter string. If "", show everything
    fentries.clear();        // fentries is the actual display after filtering, and maps displayed entry number to actual entry number
    fselected = 0;           // selected = fentries[fselected]
    filtering = true;        // enable list display filtering via '/' or '.'
    filtering_nocase = true; // ignore case when filtering
    max_entry_len = 0;
    max_column_len = 0;      // for calculating space for second column

    categories.clear();
    switch_to_category = current_category = 0;

    input_category = "UILIST";
    additional_actions.clear();
}

input_context uilist::create_main_input_context() const
{
    input_context ctxt( input_category, keyboard_mode::keycode );
    register_uilist_inputs( ctxt );
    return ctxt;
}

void uilist::register_uilist_inputs( input_context &ctxt ) const
{
    ctxt.register_action( "UILIST.UP" );
    ctxt.register_action( "UILIST.DOWN" );
    ctxt.register_action( "PAGE_UP", to_translation( "Fast scroll up" ) );
    ctxt.register_action( "PAGE_DOWN", to_translation( "Fast scroll down" ) );
    ctxt.register_action( "HOME", to_translation( "Go to first entry" ) );
    ctxt.register_action( "END", to_translation( "Go to last entry" ) );
    if( allow_cancel ) {
        ctxt.register_action( "UILIST.QUIT" );
    }
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );
    if( allow_confirm ) {
        ctxt.register_action( "CONFIRM" );
        ctxt.register_action( "SELECT" );
    }
    ctxt.register_action( "UILIST.FILTER" );
    if( !categories.empty() ) {
        ctxt.register_action( "UILIST.LEFT" );
        ctxt.register_action( "UILIST.RIGHT" );
    }
    ctxt.register_action( "ANY_INPUT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    //TODO: redundant, remove
    for( const auto &additional_action : additional_actions ) {
        ctxt.register_action( additional_action.first, additional_action.second );
    }
}

/**
 * repopulate filtered entries list (fentries) and set fselected accordingly
 */
void uilist::filterlist()
{
    // TODO: && is_all_lc( filter )
    fentries.clear();
    fselected = -1;
    int f = 0;
    for( size_t i = 0; i < entries.size(); i++ ) {
        bool visible = true;
        if( !categories.empty() && !category_filter( entries[i], categories[current_category].first ) ) {
            continue;
        }
        if( filtering && !filter.empty() ) {
            if( filtering_nocase ) {
                // case-insensitive match
                visible = lcmatch( entries[i].txt, filter );
            } else {
                // case-sensitive match
                visible = entries[i].txt.find( filter ) != std::string::npos;
            }
        }
        if( visible ) {
            fentries.push_back( static_cast<int>( i ) );
            if( hilight_disabled || entries[i].enabled ) {
                if( static_cast<int>( i ) == selected || ( static_cast<int>( i ) > selected && fselected == -1 ) ) {
                    // Either this is selected, or we are past the previously selected entry,
                    // which has been filtered out, so choose another nearby entry instead.
                    fselected = f;
                }
            }
            f++;
        }
    }
    if( fselected == -1 ) {
        fselected = 0;
        vshift = 0;
        if( fentries.empty() ) {
            selected = -1;
        } else {
            previewing = selected = fentries [ 0 ];
        }
    } else if( fselected < static_cast<int>( fentries.size() ) ) {
        previewing = selected = fentries[fselected];
    } else {
        previewing = fselected = selected = -1;
    }
    // scroll to top of screen if all remaining entries fit the screen.
    if( static_cast<int>( fentries.size() ) <= vmax ) {
        vshift = 0;
    }
    if( callback != nullptr ) {
        callback->select( this );
    }
}

void uilist::inputfilter()
{
    filter_popup = std::make_unique<text_input_popup>( 0, filter );
    filter_popup->set_max_input_length( 256 );
    filter = filter_popup->query();
    if( filter_popup->cancelled() ) {
        filter.clear();
    }
    filterlist();
    filter_popup.reset();
}

void uilist::calc_data()
{
    std::vector<int> autoassign;
    for( size_t i = 0; i < entries.size(); i++ ) {
        if( entries[ i ].enabled ) {
            if( !entries[i].hotkey.has_value() ) {
                autoassign.emplace_back( static_cast<int>( i ) );
            } else if( entries[i].hotkey.value() != input_event() ) {
                keymap[entries[i].hotkey.value()] = i;
            }
            if( entries[ i ].retval == -1 ) {
                entries[ i ].retval = i;
            }
        }
        if( entries[ i ].text_color == c_red_red ) {
            entries[ i ].text_color = text_color;
        }
    }
    input_context ctxt = create_main_input_context();
    const hotkey_queue &hotkeys = hotkey_queue::alpha_digits();
    input_event hotkey = ctxt.first_unassigned_hotkey( hotkeys );
    for( auto it = autoassign.begin(); it != autoassign.end() &&
         hotkey != input_event(); ++it ) {
        bool assigned = false;
        do {
            if( keymap.count( hotkey ) == 0 ) {
                entries[*it].hotkey = hotkey;
                keymap[hotkey] = *it;
                assigned = true;
            }
            hotkey = ctxt.next_unassigned_hotkey( hotkeys, hotkey );
        } while( !assigned && hotkey != input_event() );
    }

    if( desc_enabled ) {
        desc_enabled = !footer_text.empty() ||
        std::any_of( entries.begin(), entries.end(), []( const uilist_entry & ent ) {
            return !ent.desc.empty();
        } );
    }

    // Narrowed to the rows that fit by uilist_impl, once it has a screen to fit
    vmax = entries.size();
    shared_ptr_fast<uilist_impl> current_ui = ui.lock();
    if( current_ui ) {
        current_ui->mark_resized();
    }
}

void uilist::setup()
{
    if( !started ) {
        filterlist();
    }

    calc_data();

    started = true;
}

void uilist::reposition()
{
    setup();
}

uilist::scroll_amount uilist::scroll_amount_from_action( const std::string &action )
{
    const int scroll_rate = vmax > 20 ? 10 : 3;
    if( action == "UILIST.UP" ) {
        return uilist::scroll_amount::wrapped( -1 );
    } else if( action == "PAGE_UP" ) {
        return uilist::scroll_amount::clamped( -scroll_rate );
    } else if( action == "HOME" ) {
        return uilist::scroll_amount::abs( 0 );
    } else if( action == "END" ) {
        return uilist::scroll_amount::abs( static_cast<int>( fentries.size() - 1 ) );
    } else if( action == "UILIST.DOWN" ) {
        return uilist::scroll_amount::wrapped( 1 );
    } else if( action == "PAGE_DOWN" ) {
        return uilist::scroll_amount::clamped( scroll_rate );
    } else if( action == "SCROLL_UP" ) {
        return uilist::scroll_amount::clamped( -1 );
    } else if( action == "SCROLL_DOWN" ) {
        return uilist::scroll_amount::clamped( 1 );
    } else {
        return uilist::scroll_amount::clamped( 0 );
    }
}

/**
 * check for valid scrolling keypress and handle. return false if invalid keypress
 */
bool uilist::scrollby( const uilist::scroll_amount scrollby )
{
    bool is_relative = scrollby.type == uilist::scroll_amount::relative_wrapped ||
                       scrollby.type == uilist::scroll_amount::relative_clamped;
    if( is_relative && scrollby.qty == 0 ) {
        return false;
    }

    bool may_wrap = scrollby.type == uilist::scroll_amount::relative_wrapped;
    bool backwards = false;
    if( may_wrap ) {
        backwards = scrollby.qty < 0;
    } else {
        backwards = scrollby.qty > 0;
    }
    int recmax = static_cast<int>( fentries.size() );

    if( scrollby.type == uilist::scroll_amount::absolute ) {
        fselected = scrollby.qty;
    } else {
        fselected += scrollby.qty;
    }

    if( !may_wrap ) {
        // if wrapping is not allowed then clamp to the bounds of the list
        if( fselected < 0 ) {
            fselected = 0;
        } else if( fselected >= recmax ) {
            fselected = fentries.size() - 1;
        }
    }

    // if the current selected entry is disabled, find the next
    // enabled entry in the direction of movement
    if( backwards ) {
        if( fselected < 0 ) {
            fselected = fentries.size() - 1;
        }
        for( size_t i = 0; i < fentries.size(); ++i ) {
            if( hilight_disabled || entries[ fentries [ fselected ] ].enabled ) {
                break;
            }
            --fselected;
            if( fselected < 0 ) {
                fselected = fentries.size() - 1;
            }
        }
    } else {
        if( fselected >= recmax ) {
            fselected = 0;
        }
        for( size_t i = 0; i < fentries.size(); ++i ) {
            if( hilight_disabled || entries[ fentries [ fselected ] ].enabled ) {
                break;
            }
            ++fselected;
            if( fselected >= recmax ) {
                fselected = 0;
            }
        }
    }
    if( static_cast<size_t>( fselected ) < fentries.size() ) {
        selected = previewing = fentries [ fselected ];
        if( callback != nullptr ) {
            callback->select( this );
        }
    }

    need_to_scroll = true;
    return true;
}

shared_ptr_fast<uilist_impl> uilist::create_or_get_ui()
{
    shared_ptr_fast<uilist_impl> current_ui = ui.lock();
    if( !current_ui ) {
        ui = current_ui = make_shared_fast<uilist_impl>( *this );
    }
    return current_ui;
}

shared_ptr_fast<uilist_impl> uilist::query( bool loop, int timeout, bool allow_unfiltered_hotkeys )
{
    input_context ctxt = create_main_input_context();

#if defined(__ANDROID__)
    for( const auto &entry : entries ) {
        if( entry.enabled && entry.hotkey.has_value()
            && entry.hotkey.value() != input_event() ) {
            ctxt.register_manual_key( entry.hotkey.value().get_first_input(), entry.txt );
        }
    }
#endif
    if( !query_setup() ) {
        return nullptr;
    }
    shared_ptr_fast<uilist_impl> ui = create_or_get_ui();
    do {
        ui_manager::redraw();
        query_once( ctxt, timeout, allow_unfiltered_hotkeys );
    } while( loop && ret == UILIST_WAIT_INPUT );
    return ui;
}

/**
 * Handle input and update display
 *
 */
bool uilist::query_setup()
{
#if defined(__ANDROID__)
    if( get_option<bool>( "ANDROID_NATIVE_UI" ) && !entries.empty() && !desired_bounds ) {
        if( !started ) {
            calc_data();
            started = true;
        }
        JNIEnv *env = ( JNIEnv * )GetAndroidJNIEnv();
        jobject activity = ( jobject )GetAndroidActivity();
        jclass clazz( env->GetObjectClass( activity ) );
        jmethodID get_nativeui_method_id = env->GetMethodID( clazz, "getNativeUI",
                                           "()Lcom/cleverraven/cataclysmdda/NativeUI;" );
        jobject native_ui_obj = env->CallObjectMethod( activity, get_nativeui_method_id );
        jclass native_ui_cls( env->GetObjectClass( native_ui_obj ) );
        jmethodID list_menu_method_id = env->GetMethodID( native_ui_cls, "singleChoiceList",
                                        "(Ljava/lang/String;[Ljava/lang/String;[Z)I" );
        jstring jstr_message = env->NewStringUTF( text.c_str() );
        jobjectArray j_options = env->NewObjectArray( entries.size(), env->FindClass( "java/lang/String" ),
                                 env->NewStringUTF( "" ) );
        jbooleanArray j_enabled = env->NewBooleanArray( entries.size() );
        jboolean *n_enabled = new jboolean[entries.size()];
        for( std::size_t i = 0; i < entries.size(); i++ ) {
            std::string entry = remove_color_tags( entries[i].txt );
            if( !entries[i].ctxt.empty() ) {
                std::string ctxt = remove_color_tags( entries[i].ctxt );
                while( !ctxt.empty() && ctxt.back() == '\n' ) {
                    ctxt.pop_back();
                }
                if( !ctxt.empty() ) {
                    str_append( entry, "\n", ctxt );
                }
            }
            if( desc_enabled ) {
                std::string desc = remove_color_tags( entries[i].desc );
                while( !desc.empty() && desc.back() == '\n' ) {
                    desc.pop_back();
                }
                if( !desc.empty() ) {
                    str_append( entry, "\n", desc );
                }
            }
            env->SetObjectArrayElement( j_options, i, env->NewStringUTF( entry.c_str() ) );
            n_enabled[i] = entries[i].enabled;
        }
        env->SetBooleanArrayRegion( j_enabled, 0, entries.size(), n_enabled );
        int j_ret = env->CallIntMethod( native_ui_obj, list_menu_method_id, jstr_message, j_options,
                                        j_enabled );
        env->DeleteLocalRef( j_enabled );
        env->DeleteLocalRef( j_options );
        env->DeleteLocalRef( jstr_message );
        env->DeleteLocalRef( native_ui_cls );
        env->DeleteLocalRef( native_ui_obj );
        env->DeleteLocalRef( clazz );
        env->DeleteLocalRef( activity );
        delete[] n_enabled;
        if( j_ret == -1 ) {
            ret = UILIST_CANCEL;
        } else if( 0 <= j_ret && j_ret < static_cast<int>( entries.size() ) ) {
            ret = entries[j_ret].retval;
        } else {
            ret = UILIST_ERROR;
        }
        return false;
    }
#endif
    ret_evt = input_event();
    if( entries.empty() ) {
        ret = UILIST_ERROR;
        return false;
    }
    ret = UILIST_WAIT_INPUT;

    return true;
}

void uilist::query_once( input_context &ctxt, int timeout,
                         bool allow_unfiltered_hotkeys )
{
    ret_act = ctxt.handle_input( timeout );
    const input_event event = ctxt.get_raw_input();
    ret_evt = event;
    const auto iter = keymap.find( ret_evt );

    // Hovering a row previews it, clicking one picks it, and clicking a tab
    // switches to it. The wheel over a side panel scrolls that, which RmlUi
    // does itself, rather than the selection.
    bool mouse_handled = false;
    if( ret_act == "SCROLL_UP" || ret_act == "SCROLL_DOWN" ) {
        if( shared_ptr_fast<uilist_impl> current_ui = ui.lock() ) {
            mouse_handled = current_ui->hovering_panel();
        }
    }
    if( ret_act == "MOUSE_MOVE" || ret_act == "SELECT" ) {
        if( shared_ptr_fast<uilist_impl> current_ui = ui.lock() ) {
            const int hovered = current_ui->hovered_entry();
            const int tab = current_ui->hovered_category();
            if( hovered >= 0 ) {
                previewing = fentries[hovered];
                if( ret_act == "SELECT" ) {
                    fselected = hovered;
                    selected = previewing;
                    clicked = true;
                }
                mouse_handled = true;
            } else if( tab >= 0 && ret_act == "SELECT" ) {
                switch_to_category = tab;
                mouse_handled = true;
            }
        }
        mouse_handled = mouse_handled || ret_act == "MOUSE_MOVE";
    }

    if( !mouse_handled ) {
        scrollby( scroll_amount_from_action( ret_act ) );
    }

    if( filtering && ret_act == "UILIST.FILTER" ) {
        inputfilter();
    } else if( !categories.empty() && ( ret_act == "UILIST.LEFT" || ret_act == "UILIST.RIGHT" ) ) {
        switch_to_category = inc_clamp_wrap( current_category, ret_act == "UILIST.RIGHT",
                                             categories.size() );
    } else if( iter != keymap.end() ) {
        if( allow_unfiltered_hotkeys ) {
            const bool enabled = entries[iter->second].enabled;
            if( enabled || allow_disabled ) {
                ret = entries[iter->second].retval;
            }
        } else {
            const auto it = std::find( fentries.begin(), fentries.end(), iter->second );
            if( it != fentries.end() ) {
                const bool enabled = entries[*it].enabled;
                if( enabled || allow_disabled || hilight_disabled ) {
                    // Change the selection to display correctly when this function
                    // is called again.
                    fselected = std::distance( fentries.begin(), it );
                    selected = *it;
                    if( enabled || allow_disabled ) {
                        ret = entries[selected].retval;
                    }
                    if( callback != nullptr ) {
                        callback->select( this );
                    }
                }
            }
        }
    } else if( allow_confirm && !fentries.empty() && ( clicked || ret_act == "CONFIRM" ) ) {
        clicked = false;
        if( entries[ selected ].enabled || allow_disabled ) {
            ret = entries[selected].retval;
            if( callback != nullptr ) {
                callback->confirm( this );
            }
        }
    } else if( allow_cancel && ret_act == "UILIST.QUIT" ) {
        ret = UILIST_CANCEL;
    } else if( ret_act == "TIMEOUT" ) {
        ret = UILIST_WAIT_INPUT;
    } else if( mouse_handled ) {
        // Done above
    } else {
        // including HELP_KEYBINDINGS, in case the caller wants to refresh their contents
        bool unhandled = callback == nullptr || !callback->key( ctxt, event, selected, this );
        if( unhandled && allow_anykey ) {
            ret = UILIST_UNBOUND;
        } else if( unhandled && allow_additional ) {
            for( const auto &it : additional_actions ) {
                if( it.first == ret_act ) {
                    ret = UILIST_ADDITIONAL;
                    break;
                }
            }
        }
    }
}

///@}
void uilist::set_hide( bool val )
{
    create_or_get_ui()->hidden = val;
}

void uilist::scroll_right_panel( rml_ui::scroll how )
{
    create_or_get_ui()->pending_scroll = how;
}
/**
 * cleanup
 */
void uilist::reset()
{
    init();
}

void uilist::addentry( const std::string &txt )
{
    entries.emplace_back( txt );
}

void uilist::addentry( const uilist_entry &entry )
{
    entries.emplace_back( entry );
}

void uilist::addentry( int retval, bool enabled, int key, const std::string &txt )
{
    entries.emplace_back( retval, enabled, key, txt );
}

void uilist::addentry( const int retval, const bool enabled,
                       const std::optional<input_event> &key,
                       const std::string &txt )
{
    entries.emplace_back( retval, enabled, key, txt );
}

void uilist::addentry_desc( const std::string &txt, const std::string &desc )
{
    entries.emplace_back( txt, desc );
}

void uilist::addentry_desc( int retval, bool enabled, int key, const std::string &txt,
                            const std::string &desc )
{
    entries.emplace_back( retval, enabled, key, txt, desc );
}

void uilist::addentry_desc( int retval, bool enabled, const std::optional<input_event> &key,
                            const std::string &txt, const std::string &desc )
{
    entries.emplace_back( retval, enabled, key, txt, desc );
}

void uilist::addentry_col( int retval, bool enabled, int key, const std::string &txt,
                           const std::string &column,
                           const std::string &desc )
{
    entries.emplace_back( retval, enabled, key, txt, desc, column );
}

void uilist::addentry_col( const int retval, const bool enabled,
                           const std::optional<input_event> &key,
                           const std::string &txt, const std::string &column,
                           const std::string &desc )
{
    entries.emplace_back( retval, enabled, key, txt, desc, column );
}

void uilist::settext( const std::string &str )
{
    text = str;
}

void uilist::set_selected( int index )
{
    selected = previewing = std::clamp( index, 0, static_cast<int>( entries.size() - 1 ) );
}

void uilist::add_category( const std::string &key, const std::string &name )
{
    categories.emplace_back( key, name );
}

void uilist::set_category( const std::string &key )
{
    const auto it = std::find_if( categories.begin(),
    categories.end(), [key]( std::pair<std::string, std::string> &pair ) {
        return pair.first == key;
    } );
    switch_to_category = current_category = std::distance( categories.begin(), it );
}

void uilist::set_category_filter( const
                                  std::function<bool( const uilist_entry &, const std::string & )> &fun )
{
    category_filter = fun;
}

void uimenu::addentry( int retval, bool enabled, const std::vector<std::string> &col_content )
{
    cata_assert( static_cast<int>( col_content.size() ) == col_count );
    cols.emplace_back( retval, enabled, col_content );
}

void uimenu::finalize_addentries()
{
    menu.entries.clear();
    std::vector<int> maxes( col_count, 0 );
    // get max width of each column
    for( col &c : cols ) {
        int i = 0;
        for( const std::string &entry : c.col_content ) {
            maxes[i] = std::max( maxes[i], utf8_width( entry, true ) );
            ++i;
        }
    }
    // adding spacing between columns
    int free_width = suggest_width - std::reduce( maxes.begin(), maxes.end() );
    int spacing = std::min( 3, col_count > 1 ? free_width / ( col_count - 1 ) : 0 );
    if( spacing > 0 ) {
        for( int i = 0; i < col_count - 1; ++i ) {
            maxes[i] += spacing;
        }
    }

    for( col &c : cols ) {
        std::string row;
        int i = 0;
        for( const std::string &entry : c.col_content ) {
            // Pad with spaces
            // Add length of tags to number of spaces to pad with
            // That is length(entry_with_tags) - length(entry_without_tags)
            // Otherwise the entry padding will be shorter by number of chars in tags
            int entry_len_plus_tags = maxes[i] + utf8_width( entry, false ) - utf8_width( entry, true );
            row += string_format( "%-*s", entry_len_plus_tags, entry );
            ++i;
        }
        menu.addentry( c.retval, c.enabled, -1, row );
    }
}

void uimenu::set_selected( int index )
{
    menu.selected = menu.previewing = index;
}

void uimenu::set_title( const std::string &title )
{
    menu.title = title;
}

int uimenu::query()
{
    finalize_addentries();
    menu.query();
    return menu.ret;
}

struct pointmenu_cb::impl_t {
    const std::vector< tripoint_bub_ms > &points;
    int last; // to suppress redrawing
    tripoint_rel_ms last_view; // to reposition the view after selecting
    shared_ptr_fast<game::draw_callback_t> terrain_draw_cb;

    explicit impl_t( const std::vector<tripoint_bub_ms> &pts );
    ~impl_t();

    void select( uilist *menu );
};

pointmenu_cb::impl_t::impl_t( const std::vector<tripoint_bub_ms> &pts ) : points( pts )
{
    last = INT_MIN;
    avatar &player_character = get_avatar();
    last_view = player_character.view_offset;
    terrain_draw_cb = make_shared_fast<game::draw_callback_t>( [this, &player_character]() {
        if( last >= 0 && static_cast<size_t>( last ) < points.size() ) {
            g->draw_trail_to_square( player_character.view_offset, true );
        }
    } );
    g->add_draw_callback( terrain_draw_cb );
}

pointmenu_cb::impl_t::~impl_t()
{
    get_avatar().view_offset = last_view;
}

void pointmenu_cb::impl_t::select( uilist *const menu )
{
    if( last == menu->selected ) {
        return;
    }
    last = menu->selected;
    avatar &player_character = get_avatar();
    if( menu->selected < 0 || menu->selected >= static_cast<int>( points.size() ) ) {
        player_character.view_offset = tripoint_rel_ms::zero;
    } else {
        const tripoint_bub_ms &center = points[menu->selected];
        player_character.view_offset = center - player_character.pos_bub();
        // TODO: Remove this line when it's safe
        player_character.view_offset.z() = 0;
    }
    g->invalidate_main_ui_adaptor();
}


pointmenu_cb::pointmenu_cb( const std::vector<tripoint_bub_ms> &pts ) : impl( pts )
{
}

pointmenu_cb::~pointmenu_cb() = default;

void pointmenu_cb::select( uilist *const menu )
{
    impl->select( menu );
}

template bool navigate_ui_list<int, int>( const std::string &action, int &val, int page_delta,
        int size, bool wrap );
template bool navigate_ui_list<int, size_t>( const std::string &action, int &val, int page_delta,
        size_t size, bool wrap );
template bool navigate_ui_list<size_t, size_t>( const std::string &action, size_t &val,
        int page_delta, size_t size, bool wrap );
template bool navigate_ui_list<unsigned int, unsigned int>( const std::string &action,
        unsigned int &val,
        int page_delta, unsigned int size, bool wrap );

// Templating of existing `unsigned int` triggers linter rules against `unsigned long`
// NOLINTs below are to address
template<typename V, typename S>
bool navigate_ui_list( const std::string &action, V &val, int page_delta, S size, bool wrap )
{
    if( action == "UP" || action == "SCROLL_UP" || action == "DOWN" || action == "SCROLL_DOWN" ) {
        if( wrap ) {
            // NOLINTNEXTLINE(cata-no-long)
            val = inc_clamp_wrap( val, action == "DOWN" || action == "SCROLL_DOWN", static_cast<V>( size ) );
        } else {
            val = inc_clamp( val, action == "DOWN" || action == "SCROLL_DOWN",
                             // NOLINTNEXTLINE(cata-no-long)
                             static_cast<V>( size ? size - 1 : 0 ) );
        }
    } else if( ( action == "PAGE_UP" || action == "PAGE_DOWN" ) && page_delta ) {
        // page navigation never wraps
        val = inc_clamp( val, action == "PAGE_UP" ? -page_delta : page_delta,
                         // NOLINTNEXTLINE(cata-no-long)
                         static_cast<V>( size ? size - 1 : 0 ) );
    } else if( action == "HOME" ) {
        // NOLINTNEXTLINE(cata-no-long)
        val = static_cast<V>( 0 );
    } else if( action == "END" ) {
        // NOLINTNEXTLINE(cata-no-long)
        val = static_cast<V>( size ? size - 1 : 0 );
    } else {
        return false;
    }
    return true;
}

std::pair<int, int> subindex_around_cursor( int num_entries, int available_space, int cursor_pos,
        bool focused )
{
    if( !focused || num_entries <= available_space ) {
        return { 0, std::min( available_space, num_entries ) };
    }
    int slice_start = std::min( std::max( 0, cursor_pos - available_space / 2 ),
                                num_entries - available_space );
    return {slice_start, slice_start + available_space };
}

#include "rml_im.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core.h>

#include "catacharset.h"
#include "rml_markup.h"
#include "sdl_wrappers.h"

namespace rml_ui::im
{

namespace
{

// The size of the text the measurements below assume, in dp: window.rcss's
// font, and about a digit's width of it.
constexpr float font_dp = 16.f;
constexpr float char_dp = 0.56f * font_dp;
constexpr float line_dp_estimate = 1.3f * font_dp;

uint64_t hash_mix( uint64_t h, std::string_view s )
{
    // FNV-1a
    for( const char c : s ) {
        h ^= static_cast<unsigned char>( c );
        h *= 1099511628211ULL;
    }
    return h;
}

// The part of an ImGui-style label that shows: before any "##"
std::string shown_label( const std::string &label )
{
    const size_t hashes = label.find( "##" );
    return hashes == std::string::npos ? label : label.substr( 0, hashes );
}

// The part of an ImGui-style label that identifies it: all of it, or after "###"
std::string_view id_label( const std::string &label )
{
    const size_t hashes = label.find( "###" );
    return hashes == std::string::npos ? std::string_view( label ) :
           std::string_view( label ).substr( hashes );
}

std::string escape_attribute( const std::string &text )
{
    std::string out;
    out.reserve( text.size() + text.size() / 4 );
    for( const char c : text ) {
        switch( c ) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\n':
                out += ' ';
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string css_color( const vec4 &col )
{
    const auto byte = []( float v ) {
        return static_cast<int>( std::clamp( v, 0.f, 1.f ) * 255.f + 0.5f );
    };
    return string_format( "rgba(%d, %d, %d, %d)", byte( col.x ), byte( col.y ), byte( col.z ),
                          byte( col.w ) );
}

std::string id_text( uint64_t id )
{
    return std::to_string( id );
}

// One place the frame's RML goes: the window's body, a table cell, a popup...
// It lays items out in lines, as ImGui's cursor did.
struct buffer {
    std::string text;
    bool line_open = false;
    bool same = false;
    // Space before an item put on the same line: -1 for the default, or dp
    float spacing = -1.f;
    // Whether the next item goes to the right of the line
    bool push_right = false;
    float indent = 0.f;
};

struct table_frame {
    uint64_t id = 0;
    int columns = 1;
    flags tflags = 0;
    vec2 outer_size;
    std::vector<std::string> labels;
    std::vector<flags> column_flags;
    std::vector<float> widths;
    int col = -1;
    bool in_row = false;
    // A selectable that spans the row makes the whole row it
    uint64_t row_id = 0;
    bool row_selected = false;
    bool clipped = false;
    TableColumnSortSpecs spec;
    TableSortSpecs specs;
};

struct tab_bar_frame {
    uint64_t id = 0;
    std::string headers;
    bool any_tab = false;
};

struct popup_info {
    point pos;
    bool modal = false;
    float width = 0.f;
};

struct popup_frame {
    uint64_t id = 0;
    bool modal = false;
    bool combo = false;
};

struct child_frame {
    uint64_t id = 0;
    vec2 size;
    bool border = false;
};

} // namespace

class window::state
{
    public:
        // --- Kept between redraws ---
        // What the events since the last redraw did, by widget id
        std::unordered_set<uint64_t> clicks;
        std::unordered_set<uint64_t> right_clicks;
        std::unordered_map<uint64_t, std::string> edits;
        std::unordered_set<uint64_t> enters;
        bool outside_click = false;
        point right_click_pos;
        // Open trees and headers, the selected tab of each bar, open popups
        // and each sortable table's sort
        std::unordered_map<uint64_t, bool> open_nodes;
        std::unordered_map<uint64_t, uint64_t> selected_tabs;
        std::map<uint64_t, popup_info> open_popups;
        std::unordered_map<uint64_t, TableColumnSortSpecs> sorts;
        std::unordered_set<uint64_t> sorted_tables;
        // Popups an outside click closed at the start of this redraw, so a
        // click on a combo's own box closes it rather than opening it again
        std::unordered_set<uint64_t> just_closed;
        int frame_count = 0;

        // --- Read at the start of a redraw ---
        std::unordered_set<uint64_t> hovered_ids;
        uint64_t focused_id = 0;
        bool focused_input = false;
        std::vector<key_press> keys;
        io_state io;
        vec2 body_size;
        Rml::ElementDocument *doc = nullptr;

        // --- Built during a redraw ---
        std::vector<buffer> out;
        std::vector<uint64_t> id_stack;
        int auto_ids = 0;
        uint64_t last_item_id = 0;
        std::vector<table_frame> tables;
        std::vector<tab_bar_frame> tab_bars;
        std::vector<popup_frame> popups;
        std::vector<child_frame> children;
        std::vector<int> disabled_stack;
        std::vector<uint64_t> scroll_containers;
        std::string popups_rml;
        std::string tooltip_rml;
        int disabled = 0;
        float next_item_width = 0.f;
        float next_cursor_x = -1.f;
        float next_popup_width = 0.f;
        std::optional<bool> next_item_open;
        bool next_item_open_once = false;
        std::vector<std::pair<color_slot, vec4>> colors;
        bool focus_next_item = false;
        uint64_t pending_focus_id = 0;
        uint64_t pending_scroll_id = 0;
        std::unordered_set<uint64_t> closing_popups;

        // --- Ids ---
        uint64_t seed() const {
            return id_stack.empty() ? 1469598103934665603ULL : id_stack.back();
        }
        uint64_t make_id( const std::string &label ) const {
            return hash_mix( seed(), id_label( label ) );
        }
        uint64_t auto_id() {
            return hash_mix( seed() ^ 0x9e3779b97f4a7c15ULL, std::to_string( auto_ids++ ) );
        }

        // --- Output ---
        buffer &cur() {
            return out.back();
        }
        void close_line( buffer &b ) {
            if( b.line_open ) {
                b.text += "</div>";
                b.line_open = false;
            }
            b.same = false;
            b.push_right = false;
        }
        // An item in the line flow, which starts a new line unless SameLine() came before it
        void item( uint64_t id, const std::string &rml ) {
            buffer &b = cur();
            if( b.line_open && b.same ) {
                if( b.push_right ) {
                    b.text += "<div class=\"im-spring\"></div>";
                } else if( b.spacing < 0.f ) {
                    b.text += "<div class=\"im-gap\"></div>";
                } else if( b.spacing > 0.f ) {
                    b.text += string_format( "<div class=\"im-gap\" style=\"width: %.0fdp;\"></div>",
                                             b.spacing );
                }
            } else {
                close_line( b );
                b.text += b.indent > 0.f ?
                          string_format( "<div class=\"im-line\" style=\"margin-left: %.0fdp;\">", b.indent ) :
                          "<div class=\"im-line\">";
                b.line_open = true;
            }
            if( next_cursor_x >= 0.f ) {
                b.text += string_format( "<div class=\"im-gap\" style=\"width: %.0fdp;\"></div>",
                                         next_cursor_x );
                next_cursor_x = -1.f;
            }
            b.text += rml;
            b.same = false;
            b.push_right = false;
            last_item_id = id;
            if( focus_next_item ) {
                focus_next_item = false;
                pending_focus_id = id;
            }
        }
        // Something that takes the whole width, such as a table
        void block( const std::string &rml ) {
            buffer &b = cur();
            close_line( b );
            b.text += b.indent > 0.f ?
                      string_format( "<div style=\"margin-left: %.0fdp;\">", b.indent ) + rml + "</div>" : rml;
        }
        // Start building into a new buffer, indented as the current one is
        void push_buffer() {
            out.emplace_back();
        }
        std::string pop_buffer() {
            close_line( cur() );
            std::string text = std::move( cur().text );
            out.pop_back();
            return text;
        }

        // Attributes for an element the mouse can click, with this id
        std::string clickable( uint64_t id ) const {
            return " id=\"im" + id_text( id ) + "\" data-im=\"" + id_text( id ) + "\" data-imc=\"" +
                   id_text( id ) + "\"";
        }
        std::string hoverable( uint64_t id ) const {
            return " data-im=\"" + id_text( id ) + "\"";
        }
        bool take_click( uint64_t id ) {
            return disabled == 0 && clicks.erase( id ) > 0;
        }
        std::string width_style() {
            const float w = next_item_width;
            next_item_width = 0.f;
            if( w > 0.f ) {
                return string_format( " style=\"width: %.0fdp; flex: 0 0 auto;\"", w );
            }
            // Zero or less fills what is left of the line, as near enough to ImGui
            return " style=\"flex: 1 1 0; min-width: 40dp;\"";
        }

        // --- Tables ---
        table_frame *table() {
            return tables.empty() ? nullptr : &tables.back();
        }
        void end_row( table_frame &t ) {
            if( !t.in_row ) {
                return;
            }
            if( t.col >= 0 ) {
                // The open cell's content
                const std::string cell = pop_buffer();
                cur().text += cell + "</td>";
            }
            const std::string cells = pop_buffer();
            std::string attrs;
            std::string classes = t.clipped ? "line" : "";
            if( t.row_id != 0 ) {
                attrs += clickable( t.row_id );
                classes += std::string( classes.empty() ? "" : " " ) + "item" +
                           ( t.row_selected ? " selected" : "" );
            }
            if( !classes.empty() ) {
                attrs += " class=\"" + classes + "\"";
            }
            cur().text += "<tr" + attrs + ">" + cells + "</tr>";
            t.in_row = false;
            t.col = -1;
            t.row_id = 0;
            t.row_selected = false;
        }
        void begin_row( table_frame &t ) {
            end_row( t );
            push_buffer();
            t.in_row = true;
            t.col = -1;
        }
        void next_cell( table_frame &t ) {
            if( !t.in_row ) {
                begin_row( t );
            } else if( t.col + 1 >= t.columns ) {
                // Past the last column wraps to a new row, as ImGui's did;
                // ending the row closes the open cell
                begin_row( t );
            } else if( t.col >= 0 ) {
                const std::string cell = pop_buffer();
                cur().text += cell + "</td>";
            }
            t.col++;
            cur().text += "<td>";
            push_buffer();
        }

        // --- Popups ---
        bool begin_popup( uint64_t id, bool modal, bool combo ) {
            const auto iter = open_popups.find( id );
            if( iter == open_popups.end() ) {
                return false;
            }
            iter->second.modal = modal;
            if( next_popup_width > 0.f ) {
                iter->second.width = next_popup_width;
                next_popup_width = 0.f;
            }
            popups.push_back( { id, modal, combo } );
            push_buffer();
            return true;
        }
        void end_popup() {
            if( popups.empty() ) {
                return;
            }
            const popup_frame popup = popups.back();
            popups.pop_back();
            const std::string content = pop_buffer();
            const popup_info &info = open_popups[popup.id];
            // Nested ones end first, so stack each above those around it
            const std::string style = string_format( " z-index: %d;", 10 + static_cast<int>( popups.size() ) ) +
                                      ( info.width > 0.f ? string_format( " width: %.0fdp;", info.width ) : "" );
            if( popup.modal ) {
                popups_rml += "<div class=\"im-modal-back\" style=\"" + style +
                              "\"><div class=\"im-popup im-modal\">" + content + "</div></div>";
            } else {
                popups_rml += string_format( "<div class=\"im-popup\" style=\"left: %dpx; top: %dpx;%s\">",
                                             info.pos.x, info.pos.y, style ) + content + "</div>";
            }
        }
        void close_current_popup() {
            if( !popups.empty() ) {
                closing_popups.insert( popups.back().id );
            }
        }
        // A popup opened where the item with this id is, or at the mouse
        void open_popup_at_item( uint64_t id, uint64_t item_id ) {
            popup_info info;
            info.pos = mouse_px();
            if( doc != nullptr ) {
                if( Rml::Element *element = doc->GetElementById( "im" + id_text( item_id ) ) ) {
                    const Rml::Vector2f offset = element->GetAbsoluteOffset( Rml::BoxArea::Border );
                    const Rml::Vector2f size = element->GetBox().GetSize( Rml::BoxArea::Border );
                    info.pos = point( static_cast<int>( offset.x ), static_cast<int>( offset.y + size.y ) );
                }
            }
            open_popups[id] = info;
        }

        // --- Frame ---
        void begin_frame( Rml::ElementDocument *document ) {
            doc = document;
            frame_count++;
            out.clear();
            push_buffer();
            id_stack.clear();
            auto_ids = 0;
            last_item_id = 0;
            tables.clear();
            tab_bars.clear();
            popups.clear();
            children.clear();
            disabled_stack.clear();
            scroll_containers.clear();
            popups_rml.clear();
            tooltip_rml.clear();
            disabled = 0;
            next_item_width = 0.f;
            next_cursor_x = -1.f;
            next_popup_width = 0.f;
            next_item_open.reset();
            colors.clear();
            focus_next_item = false;
            pending_focus_id = 0;
            pending_scroll_id = 0;

            // A click outside every popup closes the ones that are not modal
            just_closed.clear();
            if( outside_click ) {
                for( auto iter = open_popups.begin(); iter != open_popups.end(); ) {
                    if( iter->second.modal ) {
                        ++iter;
                    } else {
                        just_closed.insert( iter->first );
                        iter = open_popups.erase( iter );
                    }
                }
            }
            outside_click = false;

            hovered_ids.clear();
            focused_id = 0;
            focused_input = false;
            if( doc != nullptr ) {
                Rml::Context *context = doc->GetContext();
                for( Rml::Element *element = context ? context->GetHoverElement() : nullptr; element;
                     element = element->GetParentNode() ) {
                    if( element->GetOwnerDocument() != doc ) {
                        break;
                    }
                    if( const Rml::Variant *value = element->GetAttribute( "data-im" ) ) {
                        hovered_ids.insert( std::strtoull( value->Get<Rml::String>().c_str(), nullptr, 10 ) );
                    }
                }
                Rml::Element *focus = context ? context->GetFocusElement() : nullptr;
                if( focus != nullptr && focus->GetOwnerDocument() == doc ) {
                    if( const Rml::Variant *value = focus->GetAttribute( "data-imc" ) ) {
                        focused_id = std::strtoull( value->Get<Rml::String>().c_str(), nullptr, 10 );
                    }
                    auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>( focus );
                    focused_input = control != nullptr;
                    // Whatever has been typed so far, in case the field has
                    // not sent a change event for it
                    if( control != nullptr && focused_id != 0 && !edits.count( focused_id ) ) {
                        edits[focused_id] = control->GetValue();
                    }
                }
                if( Rml::Element *body = doc->GetElementById( "body" ) ) {
                    const Rml::Vector2f size = body->GetBox().GetSize( Rml::BoxArea::Content );
                    body_size = vec2( size.x / dp_ratio(), size.y / dp_ratio() );
                }
            }
            keys = take_key_presses();
            io.KeyCtrl = ( SDL_GetModState() & SDL_KMOD_CTRL ) != 0;
#if defined(__APPLE__)
            io.KeyCtrl = io.KeyCtrl || ( SDL_GetModState() & SDL_KMOD_GUI ) != 0;
#endif
            io.WantTextInput = focused_input;
        }

        std::string end_frame() {
            while( !tables.empty() ) {
                EndTable();
            }
            while( !popups.empty() ) {
                end_popup();
            }
            while( out.size() > 1 ) {
                const std::string text = pop_buffer();
                cur().text += text;
            }
            for( const uint64_t id : closing_popups ) {
                open_popups.erase( id );
            }
            closing_popups.clear();
            // Events nothing asked about this redraw are stale by the next
            clicks.clear();
            right_clicks.clear();
            edits.clear();
            enters.clear();
            const std::string body = pop_buffer();
            out.clear();
            return body;
        }

        std::string overlay_rml() const {
            std::string rml = popups_rml;
            if( !tooltip_rml.empty() ) {
                const point mouse = mouse_px();
                rml += string_format( "<div class=\"im-tooltip\" style=\"left: %dpx; top: %dpx;\">",
                                      mouse.x + 16, mouse.y + 16 ) + tooltip_rml + "</div>";
            }
            return rml;
        }
};

namespace
{
window::state *current = nullptr;

uint64_t element_id( Rml::Element *element, const char *attribute )
{
    for( ; element != nullptr; element = element->GetParentNode() ) {
        if( const Rml::Variant *value = element->GetAttribute( attribute ) ) {
            return std::strtoull( value->Get<Rml::String>().c_str(), nullptr, 10 );
        }
    }
    return 0;
}

bool inside_popup( Rml::Element *element )
{
    for( ; element != nullptr; element = element->GetParentNode() ) {
        if( element->IsClassSet( "im-popup" ) || element->IsClassSet( "im-modal-back" ) ) {
            return true;
        }
    }
    return false;
}

// Records what the mouse and keyboard did in the window, for the next redraw
class event_listener : public Rml::EventListener
{
    public:
        explicit event_listener( window::state &st ) : st( st ) {}

        void ProcessEvent( Rml::Event &event ) override {
            Rml::Element *target = event.GetTargetElement();
            switch( event.GetId() ) {
                case Rml::EventId::Click: {
                    if( !inside_popup( target ) ) {
                        st.outside_click = true;
                    }
                    if( const uint64_t id = element_id( target, "data-imc" ) ) {
                        st.clicks.insert( id );
                    }
                    break;
                }
                case Rml::EventId::Mousedown: {
                    if( event.GetParameter<int>( "button", 0 ) == 1 ) {
                        // Every item under it, so a context menu finds whichever it belongs to
                        for( Rml::Element *element = target; element != nullptr;
                             element = element->GetParentNode() ) {
                            if( const Rml::Variant *value = element->GetAttribute( "data-im" ) ) {
                                st.right_clicks.insert( std::strtoull( value->Get<Rml::String>().c_str(),
                                                        nullptr, 10 ) );
                            }
                        }
                        st.right_click_pos = mouse_px();
                    }
                    break;
                }
                case Rml::EventId::Change: {
                    auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>( target );
                    const uint64_t id = element_id( target, "data-imc" );
                    if( control != nullptr && id != 0 ) {
                        st.edits[id] = control->GetValue();
                    }
                    break;
                }
                case Rml::EventId::Keydown: {
                    if( event.GetParameter<int>( "key_identifier", 0 ) == Rml::Input::KI_RETURN ||
                        event.GetParameter<int>( "key_identifier", 0 ) == Rml::Input::KI_NUMPADENTER ) {
                        if( const uint64_t id = element_id( target, "data-imc" ) ) {
                            st.enters.insert( id );
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

    private:
        window::state &st;
};

std::unordered_map<const window *, std::unique_ptr<event_listener>> listeners;

constexpr std::array<Rml::EventId, 4> listened_events = {{
        Rml::EventId::Click, Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Keydown
    }
};
} // namespace

window::window( const std::string &title, const std::string &file ) :
    rml_ui::window( title, file ),
    st( std::make_unique<state>() )
{
    set_body_scrolls( false );
    if( Rml::ElementDocument *doc = get() ) {
        std::unique_ptr<event_listener> &listener = listeners[this];
        listener = std::make_unique<event_listener>( *st );
        for( const Rml::EventId event : listened_events ) {
            doc->AddEventListener( event, listener.get() );
        }
    }
}

window::~window()
{
    const auto iter = listeners.find( this );
    if( iter != listeners.end() ) {
        if( Rml::ElementDocument *doc = get() ) {
            for( const Rml::EventId event : listened_events ) {
                doc->RemoveEventListener( event, iter->second.get() );
            }
        }
        listeners.erase( iter );
    }
}

void window::on_redraw()
{
    Rml::ElementDocument *doc = get();
    st->begin_frame( doc );
    state *previous = current;
    current = st.get();
    draw_im();
    const std::string body = st->end_frame();
    current = previous;

    // New content would lose the focused field and its caret, so carry them over
    Rml::Context *context = doc ? doc->GetContext() : nullptr;
    Rml::Element *focused = context ? context->GetFocusElement() : nullptr;
    std::string focus_id;
    int caret_start = -1;
    int caret_end = -1;
    if( focused != nullptr && focused->GetOwnerDocument() == doc ) {
        focus_id = focused->GetId();
        if( auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>( focused ) ) {
            input->GetSelection( &caret_start, &caret_end, nullptr );
        }
    }
    const bool changed = show( "body", body );
    show( "im-overlay", st->overlay_rml() );
    if( doc != nullptr && changed && !focus_id.empty() ) {
        if( Rml::Element *element = doc->GetElementById( focus_id ) ) {
            element->Focus();
            if( auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>( element ) ) {
                if( caret_start >= 0 ) {
                    input->SetSelectionRange( caret_start, caret_end );
                }
            }
        }
    }
    if( st->pending_focus_id != 0 ) {
        focus( "im" + id_text( st->pending_focus_id ) );
    }
    if( st->pending_scroll_id != 0 ) {
        scroll_into_view( "im" + id_text( st->pending_scroll_id ) );
    }
}

// --- List clipper ---

ListClipper::~ListClipper()
{
    End();
}

void ListClipper::Begin( int items_count )
{
    count = std::max( 0, items_count );
    steps = 0;
    DisplayStart = 0;
    DisplayEnd = std::min( count, 200 );
    if( !current ) {
        return;
    }
    const float row_px = line_dp * dp_ratio();
    window::state &st = *current;
    if( st.doc != nullptr && !st.scroll_containers.empty() ) {
        if( Rml::Element *scroller = st.doc->GetElementById( "im" + id_text(
                                         st.scroll_containers.back() ) ) ) {
            const int first = static_cast<int>( scroller->GetScrollTop() / row_px ) - 3;
            const int shown = static_cast<int>( scroller->GetClientHeight() / row_px ) + 8;
            DisplayStart = std::clamp( first, 0, count );
            DisplayEnd = std::clamp( DisplayStart + shown, DisplayStart, count );
        }
    }
    // The rows above the shown ones, as one spacer
    if( table_frame *t = st.table() ) {
        t->clipped = true;
        if( DisplayStart > 0 ) {
            st.end_row( *t );
            st.cur().text += string_format( "<tr style=\"height: %.0fpx;\"><td colspan=\"%d\"></td></tr>",
                                            DisplayStart * row_px, t->columns );
        }
    } else if( DisplayStart > 0 ) {
        st.block( string_format( "<div style=\"height: %.0fpx;\"></div>", DisplayStart * row_px ) );
    }
}

bool ListClipper::Step()
{
    if( steps++ == 0 ) {
        return count > 0;
    }
    End();
    return false;
}

void ListClipper::End()
{
    if( count <= 0 || !current ) {
        count = 0;
        return;
    }
    // The rows below the shown ones, as one spacer
    const float row_px = line_dp * dp_ratio();
    const int below = count - DisplayEnd;
    count = 0;
    if( below <= 0 ) {
        return;
    }
    window::state &st = *current;
    if( table_frame *t = st.table() ) {
        st.end_row( *t );
        st.cur().text += string_format( "<tr style=\"height: %.0fpx;\"><td colspan=\"%d\"></td></tr>",
                                        below * row_px, t->columns );
    } else {
        st.block( string_format( "<div style=\"height: %.0fpx;\"></div>", below * row_px ) );
    }
}

// --- Ids ---

void PushID( const std::string &str_id )
{
    if( current ) {
        current->id_stack.push_back( hash_mix( current->seed(), str_id ) );
    }
}

void PushID( int int_id )
{
    PushID( "#" + std::to_string( int_id ) );
}

void PopID()
{
    if( current && !current->id_stack.empty() ) {
        current->id_stack.pop_back();
    }
}

// --- Layout ---

void SameLine( float offset_from_start_x, float spacing )
{
    if( !current ) {
        return;
    }
    buffer &b = current->cur();
    b.same = true;
    b.spacing = spacing;
    // An offset from the start of the line is mostly used to push something
    // to the right
    b.push_right = offset_from_start_x > 0.f;
}

void NewLine()
{
    if( current ) {
        current->block( "<div class=\"gap\"></div>" );
    }
}

void Spacing()
{
    if( current ) {
        current->block( "<div style=\"height: 4dp;\"></div>" );
    }
}

void Separator()
{
    if( current ) {
        current->block( "<div class=\"sep\"></div>" );
    }
}

void SeparatorText( const std::string &label )
{
    if( current ) {
        current->block( "<div class=\"im-septext\">" + escape_rml( label ) + "</div>" );
    }
}

void Dummy( const vec2 &size )
{
    if( current ) {
        current->block( string_format( "<div style=\"height: %.0fdp; width: %.0fdp;\"></div>",
                                       std::max( 0.f, size.y ), std::max( 0.f, size.x ) ) );
    }
}

void Indent( float indent_w )
{
    if( current ) {
        current->close_line( current->cur() );
        current->cur().indent += indent_w > 0.f ? indent_w : 16.f;
    }
}

void Unindent( float indent_w )
{
    if( current ) {
        current->close_line( current->cur() );
        current->cur().indent = std::max( 0.f, current->cur().indent -
                                          ( indent_w > 0.f ? indent_w : 16.f ) );
    }
}

void SetNextItemWidth( float item_width )
{
    if( current ) {
        current->next_item_width = item_width;
    }
}

void SetCursorPosX( float local_x )
{
    if( current ) {
        current->next_cursor_x = std::max( 0.f, local_x );
    }
}

float GetCursorPosX()
{
    return current ? current->cur().indent : 0.f;
}

void AlignTextToFramePadding()
{
}

void PushTextWrapPos( float )
{
}

void PopTextWrapPos()
{
}

bool BeginChild( const std::string &str_id, const vec2 &size, bool border, flags )
{
    if( !current ) {
        return false;
    }
    const uint64_t id = current->make_id( str_id );
    current->children.push_back( { id, size, border } );
    current->scroll_containers.push_back( id );
    current->push_buffer();
    return true;
}

void EndChild()
{
    if( !current || current->children.empty() ) {
        return;
    }
    const child_frame child = current->children.back();
    current->children.pop_back();
    current->scroll_containers.pop_back();
    const std::string content = current->pop_buffer();
    std::string style;
    std::string classes = "im-child";
    if( child.size.y > 0.f ) {
        style += string_format( "height: %.0fdp;", child.size.y );
    } else if( child.size.y < 0.f ) {
        // Fills what is left of the window, less what follows it, as ImGui's
        // negative sizes did
        classes += " im-fill";
    }
    if( child.size.x > 0.f ) {
        style += string_format( "width: %.0fdp;", child.size.x );
    }
    if( child.border ) {
        classes += " im-border";
    }
    current->block( "<div id=\"im" + id_text( child.id ) + "\" class=\"" + classes + "\" style=\"" +
                    style + "\">" + content + "</div>" );
}

void BeginDisabled( bool disabled )
{
    if( current ) {
        current->disabled += disabled ? 1 : 0;
        // Kept even when not disabling, so the matching EndDisabled() balances
        current->disabled_stack.push_back( disabled ? 1 : 0 );
    }
}

void EndDisabled()
{
    if( current && !current->disabled_stack.empty() ) {
        current->disabled -= current->disabled_stack.back();
        current->disabled_stack.pop_back();
    }
}

void PushStyleColor( color_slot idx, const vec4 &col )
{
    if( current ) {
        current->colors.emplace_back( idx, col );
    }
}

void PopStyleColor( int count )
{
    if( current ) {
        for( ; count > 0 && !current->colors.empty(); --count ) {
            current->colors.pop_back();
        }
    }
}

// --- Text ---

static void text_item( const std::string &rml, const std::string &classes = "im-text" )
{
    if( current ) {
        const uint64_t id = current->auto_id();
        current->item( id, "<div class=\"" + classes + "\"" + current->hoverable( id ) + ">" + rml +
                       "</div>" );
    }
}

void TextUnformatted( const std::string &text )
{
    text_item( escape_rml( text ) );
}

void TextDisabledUnformatted( const std::string &text )
{
    text_item( escape_rml( text ), "im-text muted" );
}

void TextColoredUnformatted( const vec4 &col, const std::string &text )
{
    text_item( "<span style=\"color: " + css_color( col ) + ";\">" + escape_rml( text ) + "</span>" );
}

void TextMarkup( const std::string &markup, const nc_color &color )
{
    text_item( markup_to_rml( markup, color ) );
}

void BulletTextUnformatted( const std::string &text )
{
    text_item( "• " + escape_rml( text ) );
}

void Bullet()
{
    text_item( "•" );
    SameLine();
}

// --- Widgets ---

static bool button_widget( const std::string &label, const vec2 &size, bool small )
{
    if( !current ) {
        return false;
    }
    const uint64_t id = current->make_id( label );
    std::string style;
    for( const std::pair<color_slot, vec4> &col : current->colors ) {
        if( col.first == Col_Button ) {
            style += "background-color: " + css_color( col.second ) + ";";
        }
    }
    if( size.x > 0.f ) {
        style += string_format( "width: %.0fdp;", size.x );
    }
    const std::string classes = std::string( "button" ) + ( small ? " small" : "" ) +
                                ( current->disabled > 0 ? " disabled" : "" );
    current->item( id, "<div class=\"" + classes + "\"" +
                   ( current->disabled > 0 ? current->hoverable( id ) : current->clickable( id ) ) +
                   ( style.empty() ? "" : " style=\"" + style + "\"" ) + ">" + escape_rml( shown_label(
                               label ) ) + "</div>" );
    return current->take_click( id );
}

bool Button( const std::string &label, const vec2 &size )
{
    return button_widget( label, size, false );
}

bool SmallButton( const std::string &label )
{
    return button_widget( label, vec2(), true );
}

bool Checkbox( const std::string &label, bool *v )
{
    if( !current ) {
        return false;
    }
    const uint64_t id = current->make_id( label );
    const bool clicked = current->take_click( id );
    if( clicked && v != nullptr ) {
        *v = !*v;
    }
    const std::string text = shown_label( label );
    current->item( id, "<div class=\"im-check" + std::string( current->disabled > 0 ? " disabled" : "" ) +
                   "\"" + current->clickable( id ) + "><span class=\"check" + std::string( v != nullptr &&
                           *v ? " on" : "" ) + "\"></span>" + escape_rml( text ) + "</div>" );
    return clicked;
}

bool Selectable( const std::string &label, bool selected, flags selectable_flags, const vec2 & )
{
    if( !current ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    const bool clicked = st.take_click( id );
    const std::string text = shown_label( label );
    table_frame *t = st.table();
    if( t != nullptr && t->in_row && ( selectable_flags & SelectableFlags_SpanAllColumns ) ) {
        // The whole row is the selectable; its label is just text in the cell
        t->row_id = id;
        t->row_selected = selected;
        if( !text.empty() ) {
            st.item( id, "<div class=\"im-text\">" + escape_rml( text ) + "</div>" );
        }
        st.last_item_id = id;
    } else {
        st.item( id, "<div class=\"item im-grow" + std::string( selected ? " selected" : "" ) + "\"" +
                 st.clickable( id ) + ">" + escape_rml( text ) + "</div>" );
    }
    if( clicked && !st.popups.empty() ) {
        // Picking from a popup or combo closes it, as ImGui's did
        st.close_current_popup();
    }
    return clicked;
}

bool Selectable( const std::string &label, bool *p_selected, flags selectable_flags,
                 const vec2 &size )
{
    const bool clicked = Selectable( label, p_selected != nullptr && *p_selected, selectable_flags,
                                     size );
    if( clicked && p_selected != nullptr ) {
        *p_selected = !*p_selected;
    }
    return clicked;
}

bool MenuItem( const std::string &label, const char *shortcut, bool selected, bool enabled )
{
    if( !current ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    if( !enabled ) {
        st.item( id, "<div class=\"item disabled im-grow\"" + st.hoverable( id ) + ">" + escape_rml(
                     shown_label( label ) ) + "</div>" );
        return false;
    }
    const bool clicked = st.take_click( id );
    std::string rml = ( selected ? "✓ " : "" ) + escape_rml( shown_label( label ) );
    if( shortcut != nullptr && *shortcut != '\0' ) {
        rml += " <span class=\"muted\">" + escape_rml( shortcut ) + "</span>";
    }
    st.item( id, "<div class=\"item im-grow\"" + st.clickable( id ) + ">" + rml + "</div>" );
    if( clicked ) {
        st.close_current_popup();
    }
    return clicked;
}

static bool text_field( const std::string &label, const std::string &hint, std::string *str,
                        flags input_flags )
{
    if( !current || str == nullptr ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    bool changed = false;
    const auto edit = st.edits.find( id );
    if( edit != st.edits.end() && !( input_flags & InputTextFlags_ReadOnly ) && st.disabled == 0 ) {
        changed = *str != edit->second;
        *str = edit->second;
    }
    std::string attrs = st.clickable( id ) + st.width_style() + " value=\"" + escape_attribute(
                            *str ) + "\"";
    if( !hint.empty() ) {
        attrs += " placeholder=\"" + escape_attribute( hint ) + "\"";
    }
    if( ( input_flags & InputTextFlags_ReadOnly ) || st.disabled > 0 ) {
        attrs += " disabled=\"disabled\"";
    }
    std::string rml = "<input type=\"text\"" + attrs + "/>";
    const std::string text = shown_label( label );
    if( !text.empty() ) {
        rml += "<div class=\"im-label\">" + escape_rml( text ) + "</div>";
    }
    st.item( id, rml );
    if( input_flags & InputTextFlags_EnterReturnsTrue ) {
        return st.enters.erase( id ) > 0;
    }
    return changed;
}

bool InputText( const std::string &label, std::string *str, flags input_flags )
{
    return text_field( label, std::string(), str, input_flags );
}

bool InputTextWithHint( const std::string &label, const std::string &hint, std::string *str,
                        flags input_flags )
{
    return text_field( label, hint, str, input_flags );
}

bool InputInt( const std::string &label, int *v, int step, int, flags input_flags )
{
    if( !current || v == nullptr ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    const uint64_t minus_id = hash_mix( id, "-" );
    const uint64_t plus_id = hash_mix( id, "+" );
    bool changed = false;
    const auto edit = st.edits.find( id );
    if( edit != st.edits.end() && st.disabled == 0 ) {
        char *end = nullptr;
        const long parsed = std::strtol( edit->second.c_str(), &end, 10 );
        if( end != edit->second.c_str() && static_cast<int>( parsed ) != *v ) {
            *v = static_cast<int>( parsed );
            changed = true;
        }
    }
    if( step > 0 && st.take_click( minus_id ) ) {
        *v -= step;
        changed = true;
    }
    if( step > 0 && st.take_click( plus_id ) ) {
        *v += step;
        changed = true;
    }
    std::string rml = "<input type=\"text\"" + st.clickable( id ) + st.width_style() + " value=\"" +
                      std::to_string( *v ) + "\"" + ( ( input_flags & InputTextFlags_ReadOnly ) ?
                              " disabled=\"disabled\"" : "" ) + "/>";
    if( step > 0 ) {
        rml += "<div class=\"button small\"" + st.clickable( minus_id ) + ">-</div>"
               "<div class=\"button small\"" + st.clickable( plus_id ) + ">+</div>";
    }
    const std::string text = shown_label( label );
    if( !text.empty() ) {
        rml += "<div class=\"im-label\">" + escape_rml( text ) + "</div>";
    }
    st.item( id, rml );
    return changed;
}

bool SliderInt( const std::string &label, int *v, int v_min, int v_max, const char *format )
{
    if( !current || v == nullptr ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    bool changed = false;
    const auto edit = st.edits.find( id );
    if( edit != st.edits.end() && st.disabled == 0 ) {
        const int parsed = std::clamp( static_cast<int>( std::lround( std::strtod( edit->second.c_str(),
                                       nullptr ) ) ), v_min, v_max );
        if( parsed != *v ) {
            *v = parsed;
            changed = true;
        }
    }
    std::string rml = string_format( "<input type=\"range\" min=\"%d\" max=\"%d\" step=\"1\" value=\"%d\"",
                                     v_min, v_max, *v ) + st.clickable( id ) + st.width_style() + "/>";
    rml += "<div class=\"im-label\">" + escape_rml( string_format( format != nullptr ? format : "%d",
            *v ) ) + "</div>";
    const std::string text = shown_label( label );
    if( !text.empty() ) {
        rml += "<div class=\"im-label\">" + escape_rml( text ) + "</div>";
    }
    st.item( id, rml );
    return changed;
}

bool BeginCombo( const std::string &label, const std::string &preview_value, flags )
{
    if( !current ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    if( st.take_click( id ) ) {
        if( st.open_popups.count( id ) ) {
            st.open_popups.erase( id );
        } else if( !st.just_closed.count( id ) ) {
            st.open_popup_at_item( id, id );
        }
    }
    std::string rml = "<div class=\"im-combo\"" + st.clickable( id ) + st.width_style() + ">" +
                      escape_rml( preview_value ) + "<span class=\"im-combo-arrow\">v</span></div>";
    const std::string text = shown_label( label );
    if( !text.empty() ) {
        rml += "<div class=\"im-label\">" + escape_rml( text ) + "</div>";
    }
    st.item( id, rml );
    return st.begin_popup( id, false, true );
}

void EndCombo()
{
    if( current ) {
        current->end_popup();
    }
}

bool Combo( const std::string &label, int *current_item, const char *const items[],
            int items_count )
{
    if( current_item == nullptr || items_count <= 0 ) {
        return false;
    }
    const int shown = std::clamp( *current_item, 0, items_count - 1 );
    bool changed = false;
    if( BeginCombo( label, items[shown] ) ) {
        for( int i = 0; i < items_count; ++i ) {
            PushID( i );
            if( Selectable( items[i], i == *current_item ) ) {
                changed = *current_item != i;
                *current_item = i;
            }
            PopID();
        }
        EndCombo();
    }
    return changed;
}

static bool tree_header( const std::string &label, flags tree_flags, const std::string &classes )
{
    if( !current ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t id = st.make_id( label );
    auto iter = st.open_nodes.find( id );
    const bool is_new = iter == st.open_nodes.end();
    if( is_new ) {
        iter = st.open_nodes.emplace( id, ( tree_flags & TreeNodeFlags_DefaultOpen ) != 0 ).first;
    }
    if( st.next_item_open ) {
        if( is_new || !st.next_item_open_once ) {
            iter->second = *st.next_item_open;
        }
        st.next_item_open.reset();
    }
    if( st.take_click( id ) ) {
        iter->second = !iter->second;
    }
    const bool open = iter->second;
    st.close_line( st.cur() );
    st.block( "<div class=\"" + classes + "\"" + st.clickable( id ) + ">" + ( open ? "- " : "+ " ) +
              escape_rml( shown_label( label ) ) + "</div>" );
    st.last_item_id = id;
    return open;
}

bool CollapsingHeader( const std::string &label, flags tree_flags )
{
    return tree_header( label, tree_flags, "section-head" );
}

bool TreeNode( const std::string &label )
{
    return TreeNodeEx( label, 0 );
}

bool TreeNodeEx( const std::string &label, flags tree_flags )
{
    const bool open = tree_header( label, tree_flags, "item im-tree" );
    if( open && current ) {
        Indent();
        PushID( label );
    }
    return open;
}

void TreePop()
{
    PopID();
    Unindent();
}

void SetNextItemOpen( bool is_open, cond when )
{
    if( current ) {
        current->next_item_open = is_open;
        current->next_item_open_once = when == Cond_Once;
    }
}

// --- Tabs ---

bool BeginTabBar( const std::string &str_id )
{
    if( !current ) {
        return false;
    }
    current->tab_bars.push_back( { current->make_id( str_id ), std::string(), false } );
    current->push_buffer();
    return true;
}

void EndTabBar()
{
    if( !current || current->tab_bars.empty() ) {
        return;
    }
    const tab_bar_frame bar = current->tab_bars.back();
    current->tab_bars.pop_back();
    const std::string content = current->pop_buffer();
    current->block( "<div class=\"im-tabs\">" + bar.headers + "</div><div class=\"im-tab-body\">" +
                    content + "</div>" );
}

bool BeginTabItem( const std::string &label, bool select )
{
    if( !current || current->tab_bars.empty() ) {
        return false;
    }
    window::state &st = *current;
    tab_bar_frame &bar = st.tab_bars.back();
    const uint64_t id = hash_mix( bar.id, id_label( label ) );
    uint64_t &selected = st.selected_tabs[bar.id];
    if( select || st.take_click( id ) || ( selected == 0 && !bar.any_tab ) ) {
        selected = id;
    }
    bar.any_tab = true;
    const bool active = selected == id;
    bar.headers += "<div class=\"tab" + std::string( active ? " active" : "" ) + "\"" + st.clickable(
                       id ) + ">" + escape_rml( shown_label( label ) ) + "</div>";
    return active;
}

void EndTabItem()
{
}

// --- Tables ---

bool BeginTable( const std::string &str_id, int columns, flags table_flags, const vec2 &outer_size )
{
    if( !current || columns <= 0 ) {
        return false;
    }
    window::state &st = *current;
    table_frame t;
    t.id = st.make_id( str_id );
    t.columns = columns;
    t.tflags = table_flags;
    t.outer_size = outer_size;
    st.tables.push_back( t );
    if( table_flags & TableFlags_ScrollY ) {
        st.scroll_containers.push_back( t.id );
    }
    st.push_buffer();
    return true;
}

void EndTable()
{
    if( !current || current->tables.empty() ) {
        return;
    }
    window::state &st = *current;
    table_frame t = st.tables.back();
    st.end_row( st.tables.back() );
    st.tables.pop_back();
    const std::string rows = st.pop_buffer();

    std::string cols;
    for( size_t i = 0; i < t.widths.size(); ++i ) {
        cols += ( t.column_flags[i] & TableColumnFlags_WidthFixed ) && t.widths[i] > 0.f ?
                string_format( "<col style=\"width: %.0fdp;\"/>", t.widths[i] ) : "<col/>";
    }
    std::string classes = "grid im-table";
    if( t.tflags & TableFlags_Borders ) {
        classes += " bordered";
    }
    if( t.tflags & TableFlags_RowBg ) {
        classes += " im-rowbg";
    }
    std::string table = "<table class=\"" + classes + "\">" + cols + rows + "</table>";
    if( t.tflags & TableFlags_ScrollY ) {
        st.scroll_containers.pop_back();
        const std::string height = t.outer_size.y > 0.f ?
                                   string_format( "height: %.0fdp;", t.outer_size.y ) : std::string();
        table = "<div id=\"im" + id_text( t.id ) + "\" class=\"im-child" + ( t.outer_size.y > 0.f ? "" :
                " im-fill" ) + "\" style=\"" + height + "\">" + table + "</div>";
    }
    st.block( table );
}

void TableSetupColumn( const std::string &label, flags column_flags, float init_width )
{
    if( table_frame *t = current ? current->table() : nullptr ) {
        t->labels.push_back( shown_label( label ) );
        t->column_flags.push_back( column_flags );
        t->widths.push_back( init_width );
    }
}

void TableSetupScrollFreeze( int, int )
{
}

void TableHeadersRow()
{
    table_frame *t = current ? current->table() : nullptr;
    if( t == nullptr ) {
        return;
    }
    window::state &st = *current;
    st.end_row( *t );
    const bool sortable = t->tflags & TableFlags_Sortable;
    TableColumnSortSpecs &sort = st.sorts[t->id];
    if( sortable && st.sorted_tables.insert( t->id ).second ) {
        // First sight of this table: its default sort
        for( size_t i = 0; i < t->column_flags.size(); ++i ) {
            if( t->column_flags[i] & TableColumnFlags_DefaultSort ) {
                sort.ColumnIndex = static_cast<int>( i );
                break;
            }
        }
        t->specs.SpecsDirty = true;
    }
    std::string row = "<tr class=\"im-head\">";
    for( size_t i = 0; i < t->labels.size(); ++i ) {
        const bool sorts = sortable && !( t->column_flags[i] & TableColumnFlags_NoSort );
        const uint64_t header_id = hash_mix( t->id, "#sort" + std::to_string( i ) );
        if( sorts && st.take_click( header_id ) ) {
            if( sort.ColumnIndex == static_cast<int>( i ) ) {
                sort.SortDirection = sort.SortDirection == SortDirection_Ascending ?
                                     SortDirection_Descending : SortDirection_Ascending;
            } else {
                sort.ColumnIndex = static_cast<int>( i );
                sort.SortDirection = SortDirection_Ascending;
            }
            t->specs.SpecsDirty = true;
        }
        std::string label = escape_rml( t->labels[i] );
        if( sorts && sort.ColumnIndex == static_cast<int>( i ) ) {
            label += sort.SortDirection == SortDirection_Ascending ? " ^" : " v";
        }
        row += "<th" + ( sorts ? st.clickable( header_id ) : std::string() ) + ">" + label + "</th>";
    }
    st.cur().text += row + "</tr>";
}

void TableNextRow()
{
    if( table_frame *t = current ? current->table() : nullptr ) {
        current->begin_row( *t );
    }
}

bool TableNextColumn()
{
    if( table_frame *t = current ? current->table() : nullptr ) {
        current->next_cell( *t );
        return true;
    }
    return false;
}

bool TableSetColumnIndex( int column_n )
{
    table_frame *t = current ? current->table() : nullptr;
    if( t == nullptr || column_n < 0 || column_n >= t->columns ) {
        return false;
    }
    if( t->in_row && t->col > column_n ) {
        current->begin_row( *t );
    }
    while( !t->in_row || t->col < column_n ) {
        current->next_cell( *t );
    }
    return true;
}

TableSortSpecs *TableGetSortSpecs()
{
    table_frame *t = current ? current->table() : nullptr;
    if( t == nullptr || !( t->tflags & TableFlags_Sortable ) ) {
        return nullptr;
    }
    t->spec = current->sorts[t->id];
    t->specs.Specs = &t->spec;
    t->specs.SpecsCount = 1;
    return &t->specs;
}

// --- Popups and tooltips ---

void OpenPopup( const std::string &str_id )
{
    if( !current ) {
        return;
    }
    const uint64_t id = current->make_id( str_id );
    if( !current->open_popups.count( id ) ) {
        current->open_popups[id].pos = mouse_px();
    }
}

bool BeginPopup( const std::string &str_id )
{
    return current && current->begin_popup( current->make_id( str_id ), false, false );
}

bool BeginPopupModal( const std::string &name, bool *p_open, flags )
{
    if( !current ) {
        return false;
    }
    if( p_open != nullptr && !*p_open ) {
        current->open_popups.erase( current->make_id( name ) );
        return false;
    }
    return current->begin_popup( current->make_id( name ), true, false );
}

bool BeginPopupContextItem( const std::string &str_id )
{
    if( !current ) {
        return false;
    }
    window::state &st = *current;
    const uint64_t item = st.last_item_id;
    const uint64_t id = str_id.empty() ? hash_mix( item, "#ctx" ) : st.make_id( str_id );
    if( item != 0 && st.right_clicks.count( item ) ) {
        st.right_clicks.erase( item );
        st.open_popups[id].pos = st.right_click_pos;
    }
    return st.begin_popup( id, false, false );
}

void EndPopup()
{
    if( current ) {
        current->end_popup();
    }
}

void CloseCurrentPopup()
{
    if( current ) {
        current->close_current_popup();
    }
}

bool IsPopupOpen( const char *str_id, flags popup_flags )
{
    if( !current ) {
        return false;
    }
    if( str_id == nullptr || ( popup_flags & PopupFlags_AnyPopupId ) ) {
        return !current->open_popups.empty();
    }
    return current->open_popups.count( current->make_id( str_id ) ) > 0;
}

void SetNextWindowSize( const vec2 &size )
{
    if( current ) {
        current->next_popup_width = size.x;
    }
}

void BeginTooltip()
{
    if( current ) {
        current->push_buffer();
    }
}

void EndTooltip()
{
    if( current && current->out.size() > 1 ) {
        current->tooltip_rml = current->pop_buffer();
    }
}

void SetTooltipUnformatted( const std::string &text )
{
    if( current ) {
        current->tooltip_rml = "<div class=\"pre\">" + escape_rml( text ) + "</div>";
    }
}

// --- State ---

bool IsItemHovered()
{
    return current && current->last_item_id != 0 && current->hovered_ids.count( current->last_item_id );
}

bool IsItemFocused()
{
    return current && current->last_item_id != 0 && current->focused_id == current->last_item_id;
}

bool IsAnyItemActive()
{
    return current && current->focused_input;
}

void SetKeyboardFocusHere( int offset )
{
    if( !current ) {
        return;
    }
    if( offset < 0 ) {
        current->pending_focus_id = current->last_item_id;
    } else {
        current->focus_next_item = true;
    }
}

void SetScrollHereY( float )
{
    if( current ) {
        current->pending_scroll_id = current->last_item_id;
    }
}

bool IsKeyPressed( key k, bool )
{
    if( !current ) {
        return false;
    }
    int sdl_key = 0;
    if( k >= Key_0 && k <= Key_9 ) {
        sdl_key = SDLK_0 + ( k - Key_0 );
    } else if( k == Key_Slash ) {
        sdl_key = SDLK_SLASH;
    } else if( k == Key_Period ) {
        sdl_key = SDLK_PERIOD;
    }
    return std::any_of( current->keys.begin(), current->keys.end(), [sdl_key]( const key_press & p ) {
        return p.key == sdl_key;
    } );
}

int GetFrameCount()
{
    return current ? current->frame_count : 0;
}

io_state &GetIO()
{
    static io_state outside;
    return current ? current->io : outside;
}

const style &GetStyle()
{
    static const style the_style;
    return the_style;
}

vec2 GetContentRegionAvail()
{
    if( !current ) {
        return vec2();
    }
    return vec2( std::max( 0.f, current->body_size.x - current->cur().indent ), current->body_size.y );
}

vec2 CalcTextSize( const std::string &text, const char *, bool, float wrap_width )
{
    const float width = utf8_width( text ) * char_dp;
    if( wrap_width > 0.f && width > wrap_width ) {
        return vec2( wrap_width, std::ceil( width / wrap_width ) * line_dp_estimate );
    }
    return vec2( width, line_dp_estimate );
}

float GetFontSize()
{
    return font_dp;
}

float GetFrameHeightWithSpacing()
{
    return line_dp + 4.f;
}

float GetTextLineHeightWithSpacing()
{
    return line_dp;
}

void SetClipboardText( const std::string &text )
{
    ::SetClipboardText( text );
}

void ColorConvertHSVtoRGB( float h, float s, float v, float &out_r, float &out_g, float &out_b )
{
    if( s <= 0.f ) {
        out_r = out_g = out_b = v;
        return;
    }
    h = std::fmod( h, 1.f ) / ( 60.f / 360.f );
    const int i = static_cast<int>( h );
    const float f = h - static_cast<float>( i );
    const float p = v * ( 1.f - s );
    const float q = v * ( 1.f - s * f );
    const float t = v * ( 1.f - s * ( 1.f - f ) );
    switch( i ) {
        case 0:
            out_r = v;
            out_g = t;
            out_b = p;
            break;
        case 1:
            out_r = q;
            out_g = v;
            out_b = p;
            break;
        case 2:
            out_r = p;
            out_g = v;
            out_b = t;
            break;
        case 3:
            out_r = p;
            out_g = q;
            out_b = v;
            break;
        case 4:
            out_r = t;
            out_g = p;
            out_b = v;
            break;
        default:
            out_r = v;
            out_g = p;
            out_b = q;
            break;
    }
}

} // namespace rml_ui::im

#pragma once
#ifndef CATA_SRC_RML_IM_H
#define CATA_SRC_RML_IM_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "color.h"
#include "rml_window.h"
#include "string_formatter.h"

/**
 * An immediate-mode layer over RmlUi, shaped like the part of ImGui's API the
 * debug console was written against, so code drawn that way can run on RmlUi.
 *
 * A screen derives from im::window and draws in draw_im() on every redraw,
 * calling the functions below as it would ImGui's. They build the frame's RML,
 * and report what was clicked, toggled or typed since the last redraw: a
 * button returns true on the redraw after its click, as ImGui's would on the
 * frame after. The RML only replaces the document when it changes, and focus,
 * the caret and scrolling carry over when it does.
 *
 * Sizes and positions are in RCSS `dp`, about an ImGui pixel at the default
 * font size; text measurements are estimates.
 *
 * Widgets are identified the way ImGui identifies them: by their label, with
 * "label##id" showing "label", "###id" ignoring the label, and PushID() for
 * repeated labels.
 */
namespace rml_ui::im
{

struct vec2 {
    float x = 0.f;
    float y = 0.f;
    constexpr vec2() = default;
    constexpr vec2( float x, float y ) : x( x ), y( y ) {}
};

/// A color as red, green, blue and alpha, each 0 to 1.
struct vec4 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 0.f;
    constexpr vec4() = default;
    constexpr vec4( float x, float y, float z, float w ) : x( x ), y( y ), z( z ), w( w ) {}
};

using flags = int;

enum table_flags : flags {
    TableFlags_None = 0,
    TableFlags_RowBg = 1 << 0,
    TableFlags_Borders = 1 << 1,
    TableFlags_ScrollY = 1 << 2,
    TableFlags_SizingFixedFit = 1 << 3,
    TableFlags_SizingStretchProp = 1 << 4,
    TableFlags_NoSavedSettings = 1 << 5,
    TableFlags_Sortable = 1 << 6,
    TableFlags_SortMulti = 1 << 7,
    TableFlags_Resizable = 1 << 8,
};

enum table_column_flags : flags {
    TableColumnFlags_None = 0,
    TableColumnFlags_NoSort = 1 << 0,
    TableColumnFlags_WidthFixed = 1 << 1,
    TableColumnFlags_WidthStretch = 1 << 2,
    TableColumnFlags_DefaultSort = 1 << 3,
    TableColumnFlags_NoResize = 1 << 4,
};

enum selectable_flags : flags {
    SelectableFlags_None = 0,
    SelectableFlags_SpanAllColumns = 1 << 0,
    SelectableFlags_AllowOverlap = 1 << 1,
};

enum tree_node_flags : flags {
    TreeNodeFlags_None = 0,
    TreeNodeFlags_DefaultOpen = 1 << 0,
};

enum input_text_flags : flags {
    InputTextFlags_None = 0,
    InputTextFlags_ReadOnly = 1 << 0,
    InputTextFlags_EnterReturnsTrue = 1 << 1,
};

enum window_flags : flags {
    WindowFlags_None = 0,
    WindowFlags_AlwaysAutoResize = 1 << 0,
    WindowFlags_HorizontalScrollbar = 1 << 1,
};

enum popup_flags : flags {
    PopupFlags_None = 0,
    PopupFlags_AnyPopupId = 1 << 0,
    PopupFlags_AnyPopupLevel = 1 << 1,
};

enum color_slot : int {
    Col_Button,
    Col_ButtonHovered,
};

enum key : int {
    Key_0, Key_1, Key_2, Key_3, Key_4, Key_5, Key_6, Key_7, Key_8, Key_9,
    Key_Slash,
    Key_Period,
};

enum sort_direction : int {
    SortDirection_None,
    SortDirection_Ascending,
    SortDirection_Descending,
};

struct TableColumnSortSpecs {
    int ColumnIndex = 0;
    sort_direction SortDirection = SortDirection_Ascending;
};

struct TableSortSpecs {
    const TableColumnSortSpecs *Specs = nullptr;
    int SpecsCount = 0;
    bool SpecsDirty = false;
};

/// The spacing ImGui's style held, in dp, for code that measures with it.
struct style {
    vec2 ItemSpacing = { 8.f, 4.f };
    vec2 FramePadding = { 4.f, 3.f };
    vec2 CellPadding = { 4.f, 2.f };
    vec2 WindowPadding = { 8.f, 8.f };
    float WindowBorderSize = 1.f;
};

struct io_state {
    bool KeyCtrl = false;
    bool WantTextInput = false;
};

/**
 * Builds only the rows of a long list that can be seen, as ImGui's list
 * clipper did, inside a table or child that scrolls: rows have the fixed
 * height of window.rcss's `.line`, and spacers stand in for the rest.
 */
class ListClipper
{
    public:
        int DisplayStart = 0;
        int DisplayEnd = 0;
        ~ListClipper();
        void Begin( int items_count );
        bool Step();
        void End();
    private:
        int count = 0;
        int steps = 0;
};

/// A window drawn the immediate-mode way; see the namespace comment.
class window : public rml_ui::window
{
    public:
        explicit window( const std::string &title, const std::string &file = "im.rml" );
        ~window() override;

        /// Internal to rml_im.cpp: what the window keeps between redraws and
        /// builds during one.
        class state;

    protected:
        /// Draw the window's contents, calling the functions above.
        virtual void draw_im() = 0;
        void on_redraw() override;

    private:
        std::unique_ptr<state> st;
};

// --- Ids ---
void PushID( const std::string &str_id );
void PushID( int int_id );
void PopID();

// --- Layout ---
void SameLine( float offset_from_start_x = 0.f, float spacing = -1.f );
void NewLine();
void Spacing();
void Separator();
void SeparatorText( const std::string &label );
void Dummy( const vec2 &size );
void Indent( float indent_w = 0.f );
void Unindent( float indent_w = 0.f );
void SetNextItemWidth( float item_width );
void SetCursorPosX( float local_x );
float GetCursorPosX();
void AlignTextToFramePadding();
void PushTextWrapPos( float wrap_local_pos_x = 0.f );
void PopTextWrapPos();
bool BeginChild( const std::string &str_id, const vec2 &size = vec2(), bool border = false,
                 flags window_flags = 0 );
void EndChild();
void BeginDisabled( bool disabled = true );
void EndDisabled();
void PushStyleColor( color_slot idx, const vec4 &col );
void PopStyleColor( int count = 1 );

// --- Text ---
void TextUnformatted( const std::string &text );
void TextDisabledUnformatted( const std::string &text );
void TextColoredUnformatted( const vec4 &col, const std::string &text );
/// Game markup, color tags and all, in this color where untagged.
void TextMarkup( const std::string &markup, const nc_color &color = c_light_gray );
void BulletTextUnformatted( const std::string &text );
void Bullet();

template<typename ...Args>
void Text( const char *fmt, Args &&...args )
{
    TextUnformatted( string_format( fmt, std::forward<Args>( args )... ) );
}
template<typename ...Args>
void TextDisabled( const char *fmt, Args &&...args )
{
    TextDisabledUnformatted( string_format( fmt, std::forward<Args>( args )... ) );
}
template<typename ...Args>
void TextColored( const vec4 &col, const char *fmt, Args &&...args )
{
    TextColoredUnformatted( col, string_format( fmt, std::forward<Args>( args )... ) );
}
template<typename ...Args>
void TextWrapped( const char *fmt, Args &&...args )
{
    TextUnformatted( string_format( fmt, std::forward<Args>( args )... ) );
}
template<typename ...Args>
void BulletText( const char *fmt, Args &&...args )
{
    BulletTextUnformatted( string_format( fmt, std::forward<Args>( args )... ) );
}

// --- Widgets ---
bool Button( const std::string &label, const vec2 &size = vec2() );
bool SmallButton( const std::string &label );
bool Checkbox( const std::string &label, bool *v );
bool Selectable( const std::string &label, bool selected = false, flags selectable_flags = 0,
                 const vec2 &size = vec2() );
bool Selectable( const std::string &label, bool *p_selected, flags selectable_flags = 0,
                 const vec2 &size = vec2() );
bool MenuItem( const std::string &label, const char *shortcut = nullptr, bool selected = false,
               bool enabled = true );
bool InputText( const std::string &label, std::string *str, flags input_flags = 0 );
bool InputTextWithHint( const std::string &label, const std::string &hint, std::string *str,
                        flags input_flags = 0 );
bool InputInt( const std::string &label, int *v, int step = 1, int step_fast = 100,
               flags input_flags = 0 );
bool SliderInt( const std::string &label, int *v, int v_min, int v_max,
                const char *format = "%d" );
bool BeginCombo( const std::string &label, const std::string &preview_value, flags combo_flags = 0 );
void EndCombo();
bool Combo( const std::string &label, int *current_item, const char *const items[], int items_count );
bool CollapsingHeader( const std::string &label, flags tree_flags = 0 );
bool TreeNode( const std::string &label );
bool TreeNodeEx( const std::string &label, flags tree_flags = 0 );
void TreePop();
enum cond : int {
    Cond_Always = 0,
    // Only when the tree or header has no open state yet
    Cond_Once = 1,
};
void SetNextItemOpen( bool is_open, cond when = Cond_Always );

// --- Tabs ---
bool BeginTabBar( const std::string &str_id );
void EndTabBar();
/// A tab, its body drawn while it returns true; `select` picks it.
bool BeginTabItem( const std::string &label, bool select = false );
void EndTabItem();

// --- Tables ---
bool BeginTable( const std::string &str_id, int columns, flags table_flags = 0,
                 const vec2 &outer_size = vec2() );
void EndTable();
void TableSetupColumn( const std::string &label, flags column_flags = 0, float init_width = 0.f );
void TableSetupScrollFreeze( int cols, int rows );
void TableHeadersRow();
void TableNextRow();
bool TableNextColumn();
bool TableSetColumnIndex( int column_n );
TableSortSpecs *TableGetSortSpecs();

// --- Popups and tooltips ---
void OpenPopup( const std::string &str_id );
bool BeginPopup( const std::string &str_id );
bool BeginPopupModal( const std::string &name, bool *p_open = nullptr, flags window_flags = 0 );
bool BeginPopupContextItem( const std::string &str_id = std::string() );
void EndPopup();
void CloseCurrentPopup();
bool IsPopupOpen( const char *str_id, flags popup_flags = 0 );
void SetNextWindowSize( const vec2 &size );
void BeginTooltip();
void EndTooltip();
void SetTooltipUnformatted( const std::string &text );
template<typename ...Args>
void SetTooltip( const char *fmt, Args &&...args )
{
    SetTooltipUnformatted( string_format( fmt, std::forward<Args>( args )... ) );
}

// --- State ---
bool IsItemHovered();
bool IsItemFocused();
bool IsAnyItemActive();
void SetKeyboardFocusHere( int offset = 0 );
void SetScrollHereY( float center_y_ratio = 0.5f );
bool IsKeyPressed( key k, bool repeat = true );
int GetFrameCount();
io_state &GetIO();
const style &GetStyle();
vec2 GetContentRegionAvail();
vec2 CalcTextSize( const std::string &text, const char *text_end = nullptr,
                   bool hide_text_after_double_hash = false, float wrap_width = -1.f );
float GetFontSize();
float GetFrameHeightWithSpacing();
float GetTextLineHeightWithSpacing();
void SetClipboardText( const std::string &text );
void ColorConvertHSVtoRGB( float h, float s, float v, float &out_r, float &out_g, float &out_b );

} // namespace rml_ui::im

#endif // CATA_SRC_RML_IM_H

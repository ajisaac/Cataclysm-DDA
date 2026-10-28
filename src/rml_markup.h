#pragma once
#ifndef CATA_SRC_RML_MARKUP_H
#define CATA_SRC_RML_MARKUP_H

#include <string>
#include <string_view>

#include "color.h"

/**
 * Conversion from the game's text markup to RML, for the RmlUi layer.
 *
 * Kept apart from rml_ui and free of RmlUi itself, so it can be tested
 * without an RmlUi context.
 */
namespace rml_ui
{

/**
 * Convert game text to RML that renders the way print_colored_text() would.
 *
 * - `<color_NAME>` ... `</color>` become nested spans carrying the palette
 *   classes from color_to_rml_classes(), so the innermost color wins, as with
 *   the catacurses color stack. A stray `</color>` is dropped, and any left
 *   open are closed at the end.
 * - The shorthand tags replace_colors() knows -- `<good>`, `<bad>`, `<info>`
 *   and the rest -- are expanded first, as cataimgui::draw_colored_text does.
 * - Anything else that looks like a tag is printed literally, as catacurses
 *   prints it. That includes hotkey markup such as "<M|m>OTD", which callers
 *   must resolve themselves (see shortcut_text()).
 * - `&`, `<` and `>` are escaped, and newlines become `<br/>`.
 *
 * The result is RML content for SetInnerRML(), not a whole document.
 */
std::string markup_to_rml( std::string_view text,
                           report_color_error color_error = report_color_error::yes );

/**
 * The RML classes for one color, mapped the way catacurses::wattron() does:
 * "fg-NAME", plus " bg-NAME" unless the background is the default black.
 * NAME is the lowercased palette entry from colors.json, e.g. "lred" or
 * "dgray". Bold brightens the foreground and blink the background.
 */
std::string color_to_rml_classes( const nc_color &color );

/**
 * RCSS defining every class color_to_rml_classes() can return, from the
 * palette currently loaded into windowsPalette. rml_ui attaches it to every
 * document.
 */
std::string palette_rcss();

} // namespace rml_ui

#endif // CATA_SRC_RML_MARKUP_H

#pragma once
#ifndef CATA_SRC_RML_UI_H
#define CATA_SRC_RML_UI_H

#include <string>
#include <vector>

#include "sdl_wrappers.h"

/**
 * Experimental RmlUi layer, built behind the opt-in `RMLUI` CMake flag.
 *
 * Right now this exists to run side by side with the real catacurses UI so the
 * two can be compared directly: show_main_menu() draws an RmlUi replica of the
 * main menu, mirroring the live labels and selection, while the original keeps
 * drawing underneath. It is a comparison harness, not a replacement yet.
 *
 * Everything here is a no-op unless CATA_RMLUI is defined, so the normal build
 * is unaffected and call sites need no guards of their own.
 */
namespace rml_ui
{

/// Bring up RmlUi against an existing SDL_Renderer. Safe to call twice.
void init( const SDL_Renderer_Ptr &renderer, int width, int height );

/// Tear down. Safe to call without a prior init().
void shutdown();

/// Tell RmlUi the output size changed.
void resize( int width, int height );

/// Update and draw. Call inside the frame, before presenting.
void render();

/// Whether the spike is compiled in and initialised.
bool active();

/**
 * Draw the RmlUi replica of the main menu.
 *
 * @param items Menu labels in the game's own markup, e.g. "<M|m>OTD" -- the
 *              bracketed part marks the hotkey letter and is rendered as an
 *              accented character rather than printed literally.
 * @param selected Index of the highlighted entry, mirroring main_menu's sel1.
 *
 * Cheap to call every redraw: the element list is only rebuilt when `items`
 * actually changes, and a selection change just moves a CSS class.
 */
void show_main_menu( const std::vector<std::string> &items, int selected );

/// Hide the main menu replica.
void hide_main_menu();

} // namespace rml_ui

#endif // CATA_SRC_RML_UI_H

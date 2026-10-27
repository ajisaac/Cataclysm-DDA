#pragma once
#ifndef CATA_SRC_RML_UI_H
#define CATA_SRC_RML_UI_H

#include "sdl_wrappers.h"

/**
 * Experimental RmlUi layer, built behind the opt-in `RMLUI` CMake flag.
 *
 * This is a spike to see whether RmlUi can draw over the existing SDL_Renderer
 * presentation path, not a UI framework yet. It draws one styled box so that
 * layout, CSS styling and font rasterisation can all be eyeballed at once.
 *
 * Everything here is a no-op unless CATA_RMLUI is defined, so the normal build
 * is unaffected.
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

} // namespace rml_ui

#endif // CATA_SRC_RML_UI_H

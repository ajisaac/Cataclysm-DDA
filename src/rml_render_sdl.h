#pragma once
#ifndef CATA_SRC_RML_RENDER_SDL_H
#define CATA_SRC_RML_RENDER_SDL_H

#if defined(CATA_RMLUI)

#include <vector>

#include <RmlUi/Core/RenderInterface.h>

#include "sdl_wrappers.h"

/**
 * RmlUi render interface implemented on top of SDL_Renderer.
 *
 * Upstream RmlUi dropped its SDL_Renderer backend (Backends/ now ships only
 * GL2/GL3/VK/GPU/DX12), but the game draws everything through SDL_Renderer --
 * see cata_imgui.cpp, which uses ImGui_ImplSDLRenderer3. So this is the
 * missing adapter rather than a reimplementation of something upstream has.
 *
 * Geometry is handed to SDL_RenderGeometryRaw. RmlUi vertex colours are
 * premultiplied alpha (Rml::Vertex::colour is ColourbPremultiplied), so draws
 * use SDL_BLENDMODE_BLEND_PREMULTIPLIED; using plain SDL_BLENDMODE_BLEND here
 * would double-darken every edge.
 */
class rml_sdl_render_interface : public Rml::RenderInterface
{
    public:
        explicit rml_sdl_render_interface( SDL_Renderer *renderer );

        Rml::CompiledGeometryHandle CompileGeometry( Rml::Span<const Rml::Vertex> vertices,
                Rml::Span<const int> indices ) override;
        void RenderGeometry( Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                             Rml::TextureHandle texture ) override;
        void ReleaseGeometry( Rml::CompiledGeometryHandle geometry ) override;

        Rml::TextureHandle LoadTexture( Rml::Vector2i &texture_dimensions,
                                        const Rml::String &source ) override;
        Rml::TextureHandle GenerateTexture( Rml::Span<const Rml::byte> source,
                                            Rml::Vector2i source_dimensions ) override;
        void ReleaseTexture( Rml::TextureHandle texture ) override;

        void EnableScissorRegion( bool enable ) override;
        void SetScissorRegion( Rml::Rectanglei region ) override;

    private:
        SDL_Renderer *renderer = nullptr;
        // Reused across draws so RenderGeometry does not allocate per call.
        std::vector<float> translated_xy;
};

#endif // CATA_RMLUI

#endif // CATA_SRC_RML_RENDER_SDL_H

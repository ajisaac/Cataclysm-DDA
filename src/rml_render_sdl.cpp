#include "rml_render_sdl.h"

#if defined(CATA_RMLUI)

#include <cstdint>
#include <memory>
#include <vector>

#include <RmlUi/Core/Vertex.h>

#include "debug.h"

namespace
{

// One compiled batch, kept in the layout SDL_RenderGeometryRaw wants so that
// RenderGeometry only has to apply the translation.
struct compiled_geometry {
    std::vector<float> xy;            // interleaved x, y -- untranslated
    std::vector<SDL_FColor> colour;
    std::vector<float> uv;            // interleaved u, v
    std::vector<int> indices;
};

compiled_geometry *as_geometry( Rml::CompiledGeometryHandle handle )
{
    return reinterpret_cast<compiled_geometry *>( handle );
}

SDL_Texture *as_texture( Rml::TextureHandle handle )
{
    return reinterpret_cast<SDL_Texture *>( handle );
}

} // namespace

rml_sdl_render_interface::rml_sdl_render_interface( SDL_Renderer *renderer )
    : renderer( renderer )
{
}

Rml::CompiledGeometryHandle rml_sdl_render_interface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices )
{
    auto geometry = std::make_unique<compiled_geometry>();
    geometry->xy.reserve( vertices.size() * 2 );
    geometry->uv.reserve( vertices.size() * 2 );
    geometry->colour.reserve( vertices.size() );

    for( const Rml::Vertex &vertex : vertices ) {
        geometry->xy.push_back( vertex.position.x );
        geometry->xy.push_back( vertex.position.y );
        geometry->uv.push_back( vertex.tex_coord.x );
        geometry->uv.push_back( vertex.tex_coord.y );
        // Already premultiplied; just widen to float. Do not divide out alpha.
        geometry->colour.push_back( SDL_FColor{
            vertex.colour.red / 255.0f,
            vertex.colour.green / 255.0f,
            vertex.colour.blue / 255.0f,
            vertex.colour.alpha / 255.0f } );
    }
    geometry->indices.assign( indices.begin(), indices.end() );

    return reinterpret_cast<Rml::CompiledGeometryHandle>( geometry.release() );
}

void rml_sdl_render_interface::RenderGeometry( Rml::CompiledGeometryHandle handle,
        Rml::Vector2f translation, Rml::TextureHandle texture )
{
    compiled_geometry *geometry = as_geometry( handle );
    if( !geometry || geometry->indices.empty() ) {
        return;
    }

    translated_xy.resize( geometry->xy.size() );
    for( size_t i = 0; i + 1 < geometry->xy.size(); i += 2 ) {
        translated_xy[i] = geometry->xy[i] + translation.x;
        translated_xy[i + 1] = geometry->xy[i + 1] + translation.y;
    }

    SDL_Texture *sdl_texture = as_texture( texture );
    if( sdl_texture ) {
        SDL_SetTextureBlendMode( sdl_texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED );
    }
    SDL_SetRenderDrawBlendMode( renderer, SDL_BLENDMODE_BLEND_PREMULTIPLIED );

    const int num_vertices = static_cast<int>( geometry->colour.size() );
    if( !SDL_RenderGeometryRaw( renderer, sdl_texture,
                                translated_xy.data(), 2 * sizeof( float ),
                                geometry->colour.data(), sizeof( SDL_FColor ),
                                geometry->uv.data(), 2 * sizeof( float ),
                                num_vertices,
                                geometry->indices.data(),
                                static_cast<int>( geometry->indices.size() ),
                                sizeof( int ) ) ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: SDL_RenderGeometryRaw failed: " << SDL_GetError();
    }
}

void rml_sdl_render_interface::ReleaseGeometry( Rml::CompiledGeometryHandle handle )
{
    delete as_geometry( handle );
}

Rml::TextureHandle rml_sdl_render_interface::LoadTexture( Rml::Vector2i &texture_dimensions,
        const Rml::String &source )
{
    // Only needed for <img> and background-image. The start-menu spike uses
    // neither, so a failure here is not fatal -- RmlUi treats 0 as "no texture".
    SDL_Surface *surface = IMG_Load( source.c_str() );
    if( !surface ) {
        DebugLog( D_WARNING, DC_ALL ) << "RmlUi: could not load texture " << source << ": " << SDL_GetError();
        return 0;
    }
    texture_dimensions = Rml::Vector2i( surface->w, surface->h );
    SDL_Texture *texture = SDL_CreateTextureFromSurface( renderer, surface );
    SDL_DestroySurface( surface );
    if( !texture ) {
        DebugLog( D_WARNING, DC_ALL ) << "RmlUi: could not create texture from " << source << ": " <<
                         SDL_GetError();
        return 0;
    }
    SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED );
    return reinterpret_cast<Rml::TextureHandle>( texture );
}

Rml::TextureHandle rml_sdl_render_interface::GenerateTexture(
    Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions )
{
    // This is the path that matters: RmlUi feeds font glyph atlases through it.
    // RGBA32 is the byte-order-independent alias for R,G,B,A in memory, which
    // is the layout RmlUi hands over.
    SDL_Texture *texture = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_RGBA32,
                           SDL_TEXTUREACCESS_STATIC,
                           source_dimensions.x, source_dimensions.y );
    if( !texture ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: SDL_CreateTexture failed: " << SDL_GetError();
        return 0;
    }
    const int pitch = source_dimensions.x * 4;
    if( !SDL_UpdateTexture( texture, nullptr, source.data(), pitch ) ) {
        DebugLog( D_ERROR, DC_ALL ) << "RmlUi: SDL_UpdateTexture failed: " << SDL_GetError();
        SDL_DestroyTexture( texture );
        return 0;
    }
    SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED );
    return reinterpret_cast<Rml::TextureHandle>( texture );
}

void rml_sdl_render_interface::ReleaseTexture( Rml::TextureHandle texture )
{
    if( SDL_Texture *sdl_texture = as_texture( texture ) ) {
        SDL_DestroyTexture( sdl_texture );
    }
}

void rml_sdl_render_interface::EnableScissorRegion( bool enable )
{
    if( !enable ) {
        SDL_SetRenderClipRect( renderer, nullptr );
    }
    // When enabling, SetScissorRegion follows immediately with the rect.
}

void rml_sdl_render_interface::SetScissorRegion( Rml::Rectanglei region )
{
    const SDL_Rect rect{ region.Left(), region.Top(), region.Width(), region.Height() };
    SDL_SetRenderClipRect( renderer, &rect );
}

#endif // CATA_RMLUI

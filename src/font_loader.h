#pragma once
#ifndef CATA_SRC_FONT_LOADER_H
#define CATA_SRC_FONT_LOADER_H

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cata_utility.h"
#include "debug.h"
#include "filesystem.h"
#include "json.h"
#include "path_info.h"

// How a font's glyphs are hinted, as the font settings file names them.
enum class font_hinting : int {
    automatic,
    no_auto,
    light,
    none,
    bitmap,
};

// The font-configuration values modifiable by the user.
struct font_config {
    // Path to the font file.
    std::string path;
    // The type of hinting to apply. Read so settings files keep working, but
    // nothing renders with it since the ImGui fonts it was for went away.
    std::optional<font_hinting> hinting = std::nullopt;
    // In practice, antialiasing will be ignored when hinting is set to bitmap.
    bool antialiasing = true;

    font_config() = default;

    explicit font_config( std::string path ) : path( std::move( path ) ) {}
    font_config( std::string path,
                 const std::optional<font_hinting> hinting ) : path( std::move( path ) ),
        hinting( hinting ) {}
    font_config( std::string path, const std::optional<font_hinting> hinting,
                 const bool antialiasing ) : path( std::move( path ) ), hinting( hinting ),
        antialiasing( antialiasing ) {}

    void deserialize( const JsonObject &jo );
};


extern void ensure_unifont_loaded( std::vector<font_config> &font_list );
extern void ensure_unifont_loaded( std::vector<std::string> &font_list );


class font_loader
{
    public:
        bool fontblending = false;
        std::vector<font_config> typeface;
        std::vector<font_config> map_typeface;
        std::vector<font_config> gui_typeface;
        std::vector<font_config> overmap_typeface;
        int fontwidth = 8;
        int fontheight = 16;
        int fontsize = 16;
        int map_fontwidth = 8;
        int map_fontheight = 16;
        int map_fontsize = 16;
        int overmap_fontwidth = 8;
        int overmap_fontheight = 16;
        int overmap_fontsize = 16;

    private:
        void load_throws( const cata_path &path );
        void save( const cata_path &path ) const;

    public:
        /// @throws std::exception upon any kind of error.
        void load();
};

#endif // CATA_SRC_FONT_LOADER_H

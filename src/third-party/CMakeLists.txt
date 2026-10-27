cmake_minimum_required(VERSION 3.20)

# Defaults to CMAKE_SOURCE_DIR for in-tree builds; Android sets it explicitly.
if(NOT DEFINED CATA_REPO_ROOT)
    set(CATA_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# Don't clang-tidy third party code
set(CMAKE_CXX_CLANG_TIDY "")

# Root third-party library which will export all individual ones.
add_library(
        third-party
        INTERFACE)

# Flatbuffers
file(GLOB FLATBUFFERS_HEADERS flatbuffers/**/*.h)
file(GLOB FLATBUFFERS_SOURCES flatbuffers/*.cpp)

add_library(
        flatbuffers
        STATIC
        ${FLATBUFFERS_HEADERS}
        ${FLATBUFFERS_SOURCES})

target_include_directories(
        flatbuffers
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

# For cata_flatbuffers_assert.h
target_include_directories(
        flatbuffers
        PUBLIC
        ${CATA_REPO_ROOT}/src)

target_link_libraries(
        third-party
        INTERFACE
        flatbuffers
        )

# fmt
file(GLOB FMT_HEADERS fmt/*.h)
file(GLOB FMT_SOURCES fmt/*.cc)

add_library(
        fmt
        STATIC
        ${FMT_HEADERS}
        ${FMT_SOURCES})

target_include_directories(
        fmt
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
        third-party
        INTERFACE
        fmt
        )


# GHC filesystem
file(GLOB GHC_HEADERS ghc/*.hpp)

add_library(
        ghc-filesystem
        INTERFACE)

target_sources(
        ghc-filesystem
        INTERFACE
        ${GHC_HEADERS})

target_include_directories(
        ghc-filesystem
        SYSTEM
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
        third-party
        INTERFACE
        ghc-filesystem
        )

# ImGUI
if (USE_SDL3)
    set(_imgui_backend_sources
        imgui/imgui_impl_sdl3.cpp
        imgui/imgui_impl_sdl3.h
        imgui/imgui_impl_sdlrenderer3.cpp
        imgui/imgui_impl_sdlrenderer3.h
        )
else ()
    set(_imgui_backend_sources
        imgui/imgui_impl_sdl2.cpp
        imgui/imgui_impl_sdl2.h
        imgui/imgui_impl_sdlrenderer2.cpp
        imgui/imgui_impl_sdlrenderer2.h
        )
endif ()
set(_imgui_freetype_sources "")
set(_imgui_freetype_libraries "")
if (NOT ANDROID)
    find_package(Freetype REQUIRED)
    set(_imgui_freetype_sources
        imgui/imgui_freetype.cpp
        imgui/imgui_freetype.h
        )
    set(_imgui_freetype_libraries Freetype::Freetype)
endif ()
add_library(
        imgui
        STATIC
        imgui/imconfig.h
        imgui/imgui.cpp
        imgui/imgui.h
        imgui/imgui_demo.cpp
        imgui/imgui_draw.cpp
        ${_imgui_freetype_sources}
        ${_imgui_backend_sources}
        imgui/imgui_internal.h
        imgui/imgui_stdlib.cpp
        imgui/imgui_stdlib.h
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imstb_rectpack.h
        imgui/imstb_textedit.h
        imgui/imstb_truetype.h
        )

target_include_directories(
        imgui
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        )

if (NOT USE_SDL3)
    target_include_directories(
            imgui
            SYSTEM
            PRIVATE
            ${SDL2_INCLUDE_DIR}/..
            )
endif ()

target_compile_options(
        imgui
        PRIVATE
        -w
        )

if (USE_SDL3)
    if (ANDROID)
        target_link_libraries(imgui PUBLIC SDL3::SDL3)
    elseif (NOT DYNAMIC_LINKING)
        target_link_libraries(imgui PUBLIC
            $<IF:$<TARGET_EXISTS:SDL3::SDL3-static>,SDL3::SDL3-static,SDL3::SDL3>
            ${_imgui_freetype_libraries}
        )
    else()
        target_link_libraries(imgui PUBLIC
            SDL3::SDL3
            ${_imgui_freetype_libraries}
        )
    endif ()
else ()
    if (NOT DYNAMIC_LINKING)
        target_link_libraries(imgui PUBLIC
            SDL2::SDL2-static
            ${_imgui_freetype_libraries}
        )
    else()
        target_link_libraries(imgui PUBLIC
            SDL2::SDL2
            ${_imgui_freetype_libraries}
        )
    endif ()
endif ()


target_link_libraries(
        third-party
        INTERFACE
        imgui
        )

# We use snmalloc in header only mode
file(GLOB SNMALLOC_HEADERS snmalloc/**/*)

add_library(
        snmalloc
        INTERFACE)

target_include_directories(
        snmalloc
        SYSTEM
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
        third-party
        INTERFACE
        snmalloc
        )

if (WIN32)
    target_link_libraries(
            third-party
            INTERFACE
            Synchronization.lib
            WindowsApp.lib
            )
endif ()

# Zstandard
file(GLOB ZSTD_HEADERS zstd/**/*.h)
file(GLOB ZSTD_SOURCES zstd/common/*.c zstd/compress/*.c zstd/decompress/*.c)

add_library(
        zstd
        STATIC
        ${ZSTD_HEADERS}
        ${ZSTD_SOURCES})

target_include_directories(
        zstd
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(
        zstd
        PUBLIC
        ZSTD_STATIC_LINKING_ONLY
        ZSTD_DISABLE_ASM
        )

target_link_libraries(
        third-party
        INTERFACE
        zstd
        )

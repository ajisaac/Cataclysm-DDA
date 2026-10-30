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

# RmlUi -- the HTML/CSS UI library the game's UI is drawn with. Required.
# Fetched at configure time rather than vendored, which would drop thousands
# of files into the tree.
include(FetchContent)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG 6.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rmlui)
# CATA_WARNINGS (-Werror -Wold-style-cast -Wsuggest-override ...) lands in
# CMAKE_CXX_FLAGS, so it is inherited by anything FetchContent adds as a
# subdirectory. RmlUi does not build clean under it, so its warnings are off.
foreach (_rmlui_target rmlui_core rmlui_debugger rmlui_lua)
    if (TARGET ${_rmlui_target})
        target_compile_options(${_rmlui_target} PRIVATE -w)
        # RmlUi's own headers are included by our code, which is still built
        # with CATA_WARNINGS, so they need -isystem the way the vendored
        # fmt/flatbuffers headers above do.
        get_target_property(_rmlui_inc ${_rmlui_target} INTERFACE_INCLUDE_DIRECTORIES)
        if (_rmlui_inc)
            set_property(TARGET ${_rmlui_target} APPEND PROPERTY
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_rmlui_inc})
        endif ()
    endif ()
endforeach ()
target_link_libraries(
        third-party
        INTERFACE
        RmlUi::Core
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

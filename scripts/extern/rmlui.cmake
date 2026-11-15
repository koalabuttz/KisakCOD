# RmlUi (https://github.com/mikke89/RmlUi, MIT licence) for the opt-in RmlUi
# front-end (KISAK_RMLUI, src/ui_rml, docs/rmlui-frontend.md).
#
# Only the core library is built, as a static library: no FreeType (the game's
# own bitmap fonts are drawn through src/ui_rml/rml_font.cpp), no Lua, SVG,
# Lottie, samples or tests. Nothing here is committed to the repository; the
# source is fetched at configure time like OpenAL Soft and Tracy. Point
# FETCHCONTENT_SOURCE_DIR_RMLUI at a checkout of the same tag to build offline.
#
# Defines kisak_rmlui_frontend(<target>): adds the front-end's sources, the
# KISAK_RMLUI definition and the RmlUi include directory and library to a game
# target. The caller adds KISAK_RMLUI to any other object library whose units
# include the front-end's hooks.

include_guard(GLOBAL)
include(FetchContent)

set(KISAK_RMLUI_TAG 6.3)

# RmlUi's own options, set before it reads them (its option() calls keep a
# normal variable's value under CMP0077).
set(RMLUI_FONT_ENGINE "none" CACHE STRING "KisakCOD draws RmlUi text with the game's fonts" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "" FORCE)
set(RMLUI_TRACY_PROFILING OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
set(RMLUI_INSTALL_LICENSES_AND_BUILD_INFO OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

FetchContent_Declare(rmlui
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG ${KISAK_RMLUI_TAG}
    GIT_SHALLOW TRUE
)

# Static, whatever the rest of the configure asks for, and without RmlUi's
# tests (BUILD_TESTING) even when the including project has them on.
set(kisak_rmlui_saved_shared "${BUILD_SHARED_LIBS}")
set(kisak_rmlui_saved_testing "${BUILD_TESTING}")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
if(MSVC)
    # The game links the static CRT (/MT, /MTd); RmlUi must too.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
FetchContent_MakeAvailable(rmlui)
set(BUILD_SHARED_LIBS "${kisak_rmlui_saved_shared}")
set(BUILD_TESTING "${kisak_rmlui_saved_testing}")

if(NOT TARGET RmlUi::Core)
    message(FATAL_ERROR "RmlUi ${KISAK_RMLUI_TAG} did not define RmlUi::Core")
endif()
# The engine builds with -fms-extensions and other flags of its own; RmlUi
# keeps its defaults. Position-independent code for the Android .so.
set_target_properties(rmlui_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KISAK_RMLUI_SOURCES
    "${SRC_DIR}/ui_rml/rml_frontend.cpp"
    "${SRC_DIR}/ui_rml/rml_frontend.h"
    "${SRC_DIR}/ui_rml/rml_internal.h"
    "${SRC_DIR}/ui_rml/rml_render.cpp"
    "${SRC_DIR}/ui_rml/rml_font.cpp"
    "${SRC_DIR}/ui_rml/rml_system.cpp"
    "${SRC_DIR}/ui_rml/rml_classic.cpp"
)

function(kisak_rmlui_frontend target)
    target_sources(${target} PRIVATE ${KISAK_RMLUI_SOURCES})
    target_compile_definitions(${target} PRIVATE KISAK_RMLUI RMLUI_STATIC_LIB)
    target_link_libraries(${target} PRIVATE RmlUi::Core)
endfunction()

message(STATUS "KisakCOD: RmlUi front-end on (RmlUi ${KISAK_RMLUI_TAG}, core only, game fonts)")

# ---------------------------------------------------------------------------
# Opt-in offline world front-end probe (KISAK_BUILD_WORLD_FRONTEND_PROBE)
#
# Off by default, and only available on top of the native D3D9 boundary
# (KISAK_BUILD_LINUX_NATIVE_D3D9), because two of the units it links call the
# D3D9 API. When enabled it builds:
#
#   KisakCOD-world-frontend-probe            the offline asset exporter (the
#        KisakCOD-dedi sources plus the export tool files, exactly as
#        KisakCOD-ui-asset-export builds them) with the production renderer
#        front end linked in. KISAK_WORLD_FRONTEND_MAP=<map> loads that map's
#        zone through the engine's database and runs the front end over its
#        GfxWorld (src/linux/tools/world_frontend_probe.cpp). No device is
#        created and no DXVK library is linked.
#   KisakCOD-world-frontend-invariants-test  the asset-free checks for the rules
#        and the spawn-point scan the probe applies (header only).
#
# How the renderer units are compiled:
#
#   * They use the client define set the compile census and the synthetic
#     front-end test use (KISAK_MP CINEMA USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL
#     RELEASE_ASSERTS), so production assertions stay live in every
#     configuration. The dedicated units keep their own definitions. The probe
#     compares the struct sizes both sides see before it reads the world
#     (world_frontend_layout.h).
#   * r_scene.cpp and r_buffers.cpp go through kisak_linux_native_d3d9; every
#     other unit keeps the type-only stand-in.
#   * src/linux/server_renderer_counterparts.cpp is left out: it holds the
#     dedicated server's counterparts of r_dvars.cpp, r_buffers.cpp,
#     r_rendercmds.cpp, r_xsurface.cpp and r_bsp_load_obj.cpp, which are linked
#     here instead.
#   * Material_Sort and its comparison helpers are the tail of
#     r_material_load_obj.cpp, a file that does not compile on Linux as a whole
#     (it includes d3dx9 and Win32 file APIs). The span from
#     Material_GetVertexShaderName to the end of the file is copied verbatim,
#     with a #line directive back to the original, into a generated unit at
#     configure time. The copy fails closed if the marker is missing, repeated,
#     or the span stops looking like the sort code.
#
# Everything else the linked units reach is either a dedicated-server owner
# that is already in KisakCOD-dedi, or one of the definitions in
# src/linux/tools/world_frontend_probe_boundary.cpp (storage, or a function
# that aborts with its name). See docs/linux-world-frontend.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_WORLD_FRONTEND_PROBE
    "Build the offline world front-end probe (needs KISAK_BUILD_LINUX_NATIVE_D3D9)" OFF)
if(NOT KISAK_BUILD_WORLD_FRONTEND_PROBE)
    return()
endif()

message(STATUS "----- KisakCOD world front-end probe (opt-in, offline) -----")
if(NOT TARGET kisak_linux_native_d3d9)
    message(FATAL_ERROR
        "KISAK_BUILD_WORLD_FRONTEND_PROBE needs KISAK_BUILD_LINUX_NATIVE_D3D9=ON and "
        "KISAK_DXVK_NATIVE_INCLUDE_DIR (r_scene.cpp and r_buffers.cpp compile through the boundary)")
endif()

# ---- the Material_Sort span ---------------------------------------------------
set(world_frontend_material_source "${SRC_DIR}/gfx_d3d/r_material_load_obj.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${world_frontend_material_source}")
file(READ "${world_frontend_material_source}" world_frontend_material_text)
# Engine sources are dropping the redundant __cdecl (upstream issue #35), so the
# marker and the required definitions match with or without it. The generated
# unit still copies the span verbatim.
set(world_frontend_material_marker "void Material_GetVertexShaderName(")
string(FIND "${world_frontend_material_text}" "\nvoid __cdecl Material_GetVertexShaderName(" world_frontend_cdecl_marker_at)
if(NOT world_frontend_cdecl_marker_at EQUAL -1)
    set(world_frontend_material_marker "void __cdecl Material_GetVertexShaderName(")
endif()
string(FIND "${world_frontend_material_text}" "\n${world_frontend_material_marker}" world_frontend_marker_at)
string(FIND "${world_frontend_material_text}" "\n${world_frontend_material_marker}" world_frontend_marker_last REVERSE)
if(world_frontend_marker_at EQUAL -1 OR NOT world_frontend_marker_at EQUAL world_frontend_marker_last)
    message(FATAL_ERROR "r_material_load_obj.cpp must define '${world_frontend_material_marker}' exactly once at the start of a line")
endif()
math(EXPR world_frontend_span_at "${world_frontend_marker_at} + 1")
string(SUBSTRING "${world_frontend_material_text}" 0 ${world_frontend_span_at} world_frontend_material_head)
string(SUBSTRING "${world_frontend_material_text}" ${world_frontend_span_at} -1 world_frontend_material_span)
string(REGEX MATCHALL "\n" world_frontend_head_lines "${world_frontend_material_head}")
list(LENGTH world_frontend_head_lines world_frontend_span_line)
math(EXPR world_frontend_span_line "${world_frontend_span_line} + 1")
string(REPLACE "__cdecl " "" world_frontend_material_span_plain "${world_frontend_material_span}")
foreach(required_definition
        "void Material_SortInternal(Material **sortedMaterials, uint32_t materialCount)"
        "void Material_Sort()"
        "bool Material_Compare(const Material *mtl0, const Material *mtl1)")
    string(FIND "${world_frontend_material_span_plain}" "${required_definition}" required_at)
    if(required_at EQUAL -1)
        message(FATAL_ERROR "the Material_Sort span of r_material_load_obj.cpp no longer defines '${required_definition}'")
    endif()
endforeach()
string(FIND "${world_frontend_material_span}" "#include" world_frontend_span_include)
if(NOT world_frontend_span_include EQUAL -1)
    message(FATAL_ERROR "the Material_Sort span of r_material_load_obj.cpp gained an #include; review the generated unit")
endif()
set(world_frontend_material_unit "${CMAKE_CURRENT_BINARY_DIR}/world_frontend/material_sort_span.cpp")
set(world_frontend_material_unit_text
"// Generated by scripts/platform/linux/world_frontend_probe.cmake. Do not edit.
// Lines ${world_frontend_span_line}-end of src/gfx_d3d/r_material_load_obj.cpp, copied
// verbatim: Material_Sort and the comparison helpers it uses. The rest of that
// file does not compile on Linux (d3dx9, Win32 file APIs).
#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_utils.h>
#include <gfx_d3d/r_init.h>
#include <database/database.h>
#include <algorithm>
#include <cstring>
#line ${world_frontend_span_line} \"src/gfx_d3d/r_material_load_obj.cpp\"
${world_frontend_material_span}")
set(world_frontend_material_unit_old "")
if(EXISTS "${world_frontend_material_unit}")
    file(READ "${world_frontend_material_unit}" world_frontend_material_unit_old)
endif()
if(NOT world_frontend_material_unit_old STREQUAL world_frontend_material_unit_text)
    file(WRITE "${world_frontend_material_unit}" "${world_frontend_material_unit_text}")
endif()
message(STATUS "World front-end probe: Material_Sort span from r_material_load_obj.cpp line ${world_frontend_span_line}")

# ---- renderer units, client definitions ---------------------------------------
set(KISAK_WORLD_FRONTEND_CLIENT_DEFINITIONS
    KISAK_MP CINEMA USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL RELEASE_ASSERTS
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    $<$<CONFIG:Debug>:_DEBUG>
)
set(KISAK_WORLD_FRONTEND_CLIENT_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -msse2 -mfpmath=sse -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Wno-format-security
)
# The synthetic test's renderer closure (scripts/tests/run_world_frontend_checks.py),
# r_bsp.cpp for R_SetWorldPtr_FastFile, rb_logfile.cpp, the generated Material_Sort
# span, and the probe with its link boundary.
set(KISAK_WORLD_FRONTEND_RENDERER_SOURCES
    "${SRC_DIR}/gfx_d3d/r_dpvs.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_static.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_dynmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_sceneent.cpp"
    "${SRC_DIR}/gfx_d3d/r_dpvs_entity.cpp"
    "${SRC_DIR}/gfx_d3d/r_add_bsp.cpp"
    "${SRC_DIR}/gfx_d3d/r_add_cmdbuf.cpp"
    "${SRC_DIR}/gfx_d3d/r_pretess.cpp"
    "${SRC_DIR}/gfx_d3d/r_workercmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_drawsurf.cpp"
    "${SRC_DIR}/gfx_d3d/r_rendercmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_dvars.cpp"
    "${SRC_DIR}/gfx_d3d/r_utils.cpp"
    "${SRC_DIR}/gfx_d3d/r_warn.cpp"
    "${SRC_DIR}/gfx_d3d/r_debug.cpp"
    "${SRC_DIR}/gfx_d3d/r_debug_alloc.cpp"
    "${SRC_DIR}/gfx_d3d/r_sky.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_method.cpp"
    "${SRC_DIR}/gfx_d3d/r_xsurface.cpp"
    "${SRC_DIR}/gfx_d3d/r_bsp.cpp"
    # R_SortWorldSurfaces ends in R_SetPrimaryLightShadowSurfaces, which files
    # every shadow-casting surface under the primary lights that touch it:
    # R_ForEachPrimaryLightAffectingSurface and R_AddShadowSurfaceToPrimaryLight
    # live in r_bsp_load_obj.cpp, the light culling in r_primarylights.cpp.
    "${SRC_DIR}/gfx_d3d/r_bsp_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_primarylights.cpp"
    # RB_LogPrint, which the real buffer release path calls when r_logFile is set.
    "${SRC_DIR}/gfx_d3d/rb_logfile.cpp"
    "${world_frontend_material_unit}"
)
# The probe and its link boundary are this tool's own code: every warning is an
# error, and engine headers are system headers for them so only that code is
# judged (the same split run_world_frontend_checks.py makes).
set(KISAK_WORLD_FRONTEND_PROBE_OWN_SOURCES
    "${SRC_DIR}/linux/tools/world_frontend_probe.cpp"
    "${SRC_DIR}/linux/tools/world_frontend_probe_boundary.cpp"
)
set(KISAK_WORLD_FRONTEND_NATIVE_SOURCES
    "${SRC_DIR}/gfx_d3d/r_scene.cpp"
    "${SRC_DIR}/gfx_d3d/r_buffers.cpp"
)
add_library(KisakCOD-world-frontend-renderer OBJECT ${KISAK_WORLD_FRONTEND_RENDERER_SOURCES})
add_library(KisakCOD-world-frontend-renderer-native OBJECT ${KISAK_WORLD_FRONTEND_NATIVE_SOURCES})
add_library(KisakCOD-world-frontend-probe-own OBJECT ${KISAK_WORLD_FRONTEND_PROBE_OWN_SOURCES})
foreach(renderer_target
        KisakCOD-world-frontend-renderer KisakCOD-world-frontend-renderer-native KisakCOD-world-frontend-probe-own)
    target_compile_features(${renderer_target} PRIVATE cxx_std_20)
    target_compile_definitions(${renderer_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_DEFINITIONS})
    target_compile_options(${renderer_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_OPTIONS})
    add_dependencies(${renderer_target} update_build_number)
endforeach()
foreach(renderer_target KisakCOD-world-frontend-renderer KisakCOD-world-frontend-renderer-native)
    target_include_directories(${renderer_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
endforeach()
target_include_directories(KisakCOD-world-frontend-probe-own SYSTEM PRIVATE
    "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
target_compile_options(KisakCOD-world-frontend-probe-own PRIVATE -Wall -Wextra -Werror)
target_link_libraries(KisakCOD-world-frontend-renderer-native PRIVATE kisak_linux_native_d3d9)

# ---- the executable: the asset exporter plus the front end ---------------------
get_target_property(world_frontend_dedi_sources KisakCOD-dedi SOURCES)
set(world_frontend_counterparts "${SRC_DIR}/linux/server_renderer_counterparts.cpp")
list(FIND world_frontend_dedi_sources "${world_frontend_counterparts}" world_frontend_counterparts_at)
if(world_frontend_counterparts_at EQUAL -1)
    message(FATAL_ERROR "KisakCOD-dedi no longer lists ${world_frontend_counterparts}; review what the probe leaves out")
endif()
list(REMOVE_ITEM world_frontend_dedi_sources "${world_frontend_counterparts}")
add_executable(KisakCOD-world-frontend-probe ${world_frontend_dedi_sources}
    "${SRC_DIR}/linux/tools/font_asset_export.cpp"
    "${SRC_DIR}/linux/tools/world_surface_export.cpp"
    "${SRC_DIR}/linux/tools/world_image_capture.cpp"
    $<TARGET_OBJECTS:KisakCOD-world-frontend-renderer>
    $<TARGET_OBJECTS:KisakCOD-world-frontend-renderer-native>
    $<TARGET_OBJECTS:KisakCOD-world-frontend-probe-own>)
foreach(property COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_OPTIONS)
    get_target_property(probe_property KisakCOD-dedi ${property})
    if(probe_property)
        set_property(TARGET KisakCOD-world-frontend-probe PROPERTY ${property} "${probe_property}")
    endif()
endforeach()
get_target_property(probe_definitions KisakCOD-dedi COMPILE_DEFINITIONS)
list(FILTER probe_definitions EXCLUDE REGEX "^TRACY_")
target_compile_definitions(KisakCOD-world-frontend-probe PRIVATE
    ${probe_definitions} KISAK_UI_ASSET_EXPORT_ONLY KISAK_WORLD_FRONTEND_PROBE)
target_include_directories(KisakCOD-world-frontend-probe PRIVATE "${tracy_SOURCE_DIR}/public")
# Profiling stays disabled: the offline tool must not start Tracy sockets.
target_link_libraries(KisakCOD-world-frontend-probe PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-world-frontend-probe update_build_number)

# Asset-free checks for the rules and the spawn-point scan the probe applies. It
# compiles the header alone: no engine source, no asset tree, no zone.
add_executable(KisakCOD-world-frontend-invariants-test
    "${TESTS_DIR}/linux_asset_export/world_frontend_invariants_test.cpp")
target_compile_features(KisakCOD-world-frontend-invariants-test PRIVATE cxx_std_20)
target_include_directories(KisakCOD-world-frontend-invariants-test PRIVATE "${SRC_DIR}")
target_compile_options(KisakCOD-world-frontend-invariants-test PRIVATE -Wall -Wextra -Werror)

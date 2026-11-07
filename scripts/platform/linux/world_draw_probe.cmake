# ---------------------------------------------------------------------------
# Opt-in world draw probe (KISAK_BUILD_WORLD_DRAW_PROBE)
#
# Off by default. It starts from the world device probe's renderer lists, so it
# needs KISAK_BUILD_WORLD_DEVICE_PROBE (and through it the world front-end and
# device probes). When enabled it builds:
#
#   KisakCOD-world-draw-probe   the offline asset exporter (the KisakCOD-dedi
#        sources plus the export tool files) with the whole renderer. Where a
#        client's CL_InitRenderer runs, it starts the renderer with r_init.cpp's
#        own R_Init: the window, the device, the graphics zones, render
#        targets, initial state, the scene, the code images and materials.
#        After $init it loads one allowlisted map zone, registers it with
#        R_LoadWorld, and draws one fixed view of it the way the engine's own
#        cgame-free scene render does (R_GenerateReflectionRawData in
#        r_reflection_probe.cpp): R_BeginFrame, R_ClearScene, R_RenderScene,
#        R_EndFrame, R_IssueRenderCommands. The frame is read back into memory
#        and checked; nothing but a JSON report is written
#        (src/linux/tools/world_draw_probe.cpp).
#        KISAK_WORLD_DRAW_MAP=<map> selects the map.
#        KISAK_FREE_CAMERA_MAP=<map> selects the free-camera replay instead: the
#        free camera's FrameOwner (src/linux/client/view) over the engine
#        renderer (src/linux/tools/engine_scene_renderer.cpp) replays the free
#        camera's reference traversals on the map, with a resize, a minimize,
#        a device reset and a reload (src/linux/tools/free_camera_replay.cpp).
#        With KISAK_FREE_CAMERA_INPUT=window the camera is flown through the
#        renderer's own window instead, and with =script that window is driven
#        by SDL events the run pushes and checks
#        (src/linux/tools/free_camera_window_driver.cpp).
#
# How it links:
#
#   * Every renderer unit except the four the client compile census still
#     blocks (scripts/tests/client_compile_census_blocked.txt):
#     r_material_load_obj.cpp (D3DX; fast files only here), r_screenshot.cpp
#     (Win32 monitor API; the probe takes no screenshot), r_cinematic.cpp (Bink)
#     and r_texturemem.cpp (DirectDraw). Units on the native D3D9 proof list
#     (scripts/platform/linux/native_d3d9_renderer_sources.txt) go through
#     kisak_linux_native_d3d9; the rest keep the type-only stand-in, which the
#     census proves they compile with.
#   * src/linux/server_device_counterparts.cpp, server_renderer_counterparts.cpp
#     and server_draw_counterparts.cpp are left out: their owners are linked.
#     server_assets.cpp stays (FX_Register and the sound archiving no-ops).
#   * The renderer units use the client definitions (RELEASE_ASSERTS keeps
#     assertions live), as in the earlier probes.
#   * What no linked unit defines is in
#     src/linux/tools/world_draw_probe_boundary.cpp: storage whose owner is not
#     linked, the calls into client systems this process does not run (dynamic
#     entities, sound, the cgame's scene additions), the two Windows-only renderer
#     services (cinematics, video memory) as Linux has them, and functions the
#     probe must never reach, each aborting with its name.
#
# See docs/linux-world-draw-probe.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_WORLD_DRAW_PROBE
    "Build the world draw probe (needs KISAK_BUILD_WORLD_DEVICE_PROBE)" OFF)
if(NOT KISAK_BUILD_WORLD_DRAW_PROBE)
    return()
endif()

message(STATUS "----- KisakCOD world draw probe (opt-in, DXVK Native + SDL3) -----")
if(NOT TARGET KisakCOD-world-device-native)
    message(FATAL_ERROR
        "KISAK_BUILD_WORLD_DRAW_PROBE needs KISAK_BUILD_WORLD_DEVICE_PROBE=ON "
        "(it builds on the renderer lists that probe defines)")
endif()

# Renderer units on the native D3D9 proof list that the world device probe does
# not link yet: the draw, state and backend units.
set(KISAK_WORLD_DRAW_NATIVE_SOURCES
    ${KISAK_WORLD_DEVICE_NATIVE_SOURCES}
    "${SRC_DIR}/gfx_d3d/r_draw_lit.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_lighting.cpp"
    "${SRC_DIR}/gfx_d3d/r_setstate_d3d.cpp"
    "${SRC_DIR}/gfx_d3d/r_shade.cpp"
    "${SRC_DIR}/gfx_d3d/rb_backend.cpp"
    "${SRC_DIR}/gfx_d3d/rb_depthprepass.cpp"
    "${SRC_DIR}/gfx_d3d/rb_draw3d.cpp"
    "${SRC_DIR}/gfx_d3d/rb_imagetouch.cpp"
    "${SRC_DIR}/gfx_d3d/rb_pixelcost.cpp"
    "${SRC_DIR}/gfx_d3d/rb_sky.cpp"
    "${SRC_DIR}/gfx_d3d/rhi/r_rhi_d3d9_state.cpp"
)
# The rest of the renderer, with the stand-in D3D9 types.
set(KISAK_WORLD_DRAW_STANDIN_SOURCES
    ${KISAK_WORLD_DEVICE_STANDIN_SOURCES}
    "${SRC_DIR}/gfx_d3d/r_add_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_cmdbuf.cpp"
    "${SRC_DIR}/gfx_d3d/r_cmds.cpp"
    "${SRC_DIR}/gfx_d3d/r_color.cpp"
    "${SRC_DIR}/gfx_d3d/r_command_buffer.cpp"
    "${SRC_DIR}/gfx_d3d/r_devgui.cpp"
    "${SRC_DIR}/gfx_d3d/r_dobj_skin.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_bsp.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_material.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_pixelshader.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_shadowable_light.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_draw_xmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_fog.cpp"
    "${SRC_DIR}/gfx_d3d/r_font.cpp"
    "${SRC_DIR}/gfx_d3d/r_font_metrics.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_utils.cpp"
    "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp"
    "${SRC_DIR}/gfx_d3d/r_light.cpp"
    "${SRC_DIR}/gfx_d3d/r_light_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_marks.cpp"
    "${SRC_DIR}/gfx_d3d/r_material_clone.cpp"
    "${SRC_DIR}/gfx_d3d/r_material_layered.cpp"
    "${SRC_DIR}/gfx_d3d/r_meshdata.cpp"
    "${SRC_DIR}/gfx_d3d/r_model.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_pose.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_skin.cpp"
    "${SRC_DIR}/gfx_d3d/r_model_skin_sse.cpp"
    "${SRC_DIR}/gfx_d3d/r_outdoor.cpp"
    "${SRC_DIR}/gfx_d3d/r_pixelcost_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_reflection_probe.cpp"
    "${SRC_DIR}/gfx_d3d/r_reflection_probe_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_shadowcookie.cpp"
    "${SRC_DIR}/gfx_d3d/r_spotshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_state.cpp"
    "${SRC_DIR}/gfx_d3d/r_state_utils.cpp"
    "${SRC_DIR}/gfx_d3d/r_staticmodel.cpp"
    "${SRC_DIR}/gfx_d3d/r_staticmodelcache.cpp"
    "${SRC_DIR}/gfx_d3d/r_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/r_viewport.cpp"
    "${SRC_DIR}/gfx_d3d/r_water.cpp"
    "${SRC_DIR}/gfx_d3d/r_water_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_workercmds_common.cpp"
    "${SRC_DIR}/gfx_d3d/rb_debug.cpp"
    "${SRC_DIR}/gfx_d3d/rb_drawprofile.cpp"
    "${SRC_DIR}/gfx_d3d/rb_fog.cpp"
    "${SRC_DIR}/gfx_d3d/rb_imagefilter.cpp"
    "${SRC_DIR}/gfx_d3d/rb_light.cpp"
    "${SRC_DIR}/gfx_d3d/rb_postfx.cpp"
    "${SRC_DIR}/gfx_d3d/rb_shade.cpp"
    "${SRC_DIR}/gfx_d3d/rb_shadowcookie.cpp"
    "${SRC_DIR}/gfx_d3d/rb_showcollision.cpp"
    "${SRC_DIR}/gfx_d3d/rb_spotshadow.cpp"
    "${SRC_DIR}/gfx_d3d/rb_state.cpp"
    "${SRC_DIR}/gfx_d3d/rb_stats.cpp"
    "${SRC_DIR}/gfx_d3d/rb_sunshadow.cpp"
    "${SRC_DIR}/gfx_d3d/rb_tess.cpp"
)
# Client units the renderer needs whole, with the stand-in D3D9 types: the
# effects system and the cgame's vision sets. fx_dvars.cpp owns the effects
# dvars the scene reads every frame (fx_marks, fx_marks_smodels, fx_marks_ents),
# which the probe registers with the owner's FX_RegisterDvars; the rest is the
# effects system the free-camera replay and window driver run for the map's own
# effects (src/linux/tools/engine_effects.cpp). cg_visionsets.cpp loads and
# applies the map's vision set, its glow and film, for the same two modes
# (src/linux/tools/engine_vision.cpp). The draw mode starts neither.
set(KISAK_WORLD_DRAW_CLIENT_SOURCES
    "${SRC_DIR}/cgame/cg_visionsets.cpp"
    "${SRC_DIR}/EffectsCore/fx_archive.cpp"
    "${SRC_DIR}/EffectsCore/fx_beam.cpp"
    "${SRC_DIR}/EffectsCore/fx_draw.cpp"
    "${SRC_DIR}/EffectsCore/fx_dvars.cpp"
    "${SRC_DIR}/EffectsCore/fx_graph.cpp"
    "${SRC_DIR}/EffectsCore/fx_marks.cpp"
    "${SRC_DIR}/EffectsCore/fx_postlight.cpp"
    "${SRC_DIR}/EffectsCore/fx_profile.cpp"
    "${SRC_DIR}/EffectsCore/fx_random.cpp"
    "${SRC_DIR}/EffectsCore/fx_sort.cpp"
    "${SRC_DIR}/EffectsCore/fx_sprite.cpp"
    "${SRC_DIR}/EffectsCore/fx_system.cpp"
    "${SRC_DIR}/EffectsCore/fx_update.cpp"
    "${SRC_DIR}/EffectsCore/fx_update_util.cpp"
    "${SRC_DIR}/EffectsCore/fxcurve.cpp"
)
# Every renderer unit is in exactly one list, and the census-blocked ones are in
# none: a unit added to src/gfx_d3d, or one the census unblocks, fails the
# configure until this file says where it goes.
file(GLOB world_draw_all_renderer_units "${SRC_DIR}/gfx_d3d/*.cpp" "${SRC_DIR}/gfx_d3d/rhi/*.cpp")
set(world_draw_census_blocked
    "${SRC_DIR}/gfx_d3d/r_material_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_screenshot.cpp"
    "${SRC_DIR}/gfx_d3d/r_cinematic.cpp"
    "${SRC_DIR}/gfx_d3d/r_texturemem.cpp")
foreach(unit ${world_draw_all_renderer_units})
    list(FIND KISAK_WORLD_DRAW_NATIVE_SOURCES "${unit}" in_native)
    list(FIND KISAK_WORLD_DRAW_STANDIN_SOURCES "${unit}" in_standin)
    list(FIND world_draw_census_blocked "${unit}" in_blocked)
    if(NOT ((in_native GREATER -1) OR (in_standin GREATER -1) OR (in_blocked GREATER -1)))
        message(FATAL_ERROR "The world draw probe does not place ${unit}; add it to a list in ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    if((in_native GREATER -1) AND (in_standin GREATER -1))
        message(FATAL_ERROR "The world draw probe lists ${unit} twice")
    endif()
    if((in_blocked GREATER -1) AND ((in_native GREATER -1) OR (in_standin GREATER -1)))
        message(FATAL_ERROR "The world draw probe links ${unit}, which the client compile census still blocks")
    endif()
endforeach()
# The probe's own code: every warning is an error, and engine headers are
# system headers for it.
# The boundary goes through the native D3D9 boundary too: its video-memory
# counterpart asks the device.
# The free-camera replay (KISAK_FREE_CAMERA_MAP) adds the free camera's own
# sources (src/linux/client/view), its engine adapter with the effects host that
# plays the map's own effects and the vision host that applies its vision set,
# and the replay driver.
# Its window input (KISAK_FREE_CAMERA_INPUT) adds the window host that adopts
# the renderer's window (src/linux/client/window, as KISAK_BUILD_LINUX_WINDOW_HOST
# builds it for SDL3) and the window driver.
# src/linux/client/msvc_rand.cpp gives the probe's own objects the Microsoft runtime's
# 15-bit rand() and srand(), which the effects, renderer and cgame callers assume
# (see that file). Its definitions are hidden, so shared libraries keep glibc's.
set(KISAK_WORLD_DRAW_OWN_NATIVE_SOURCES
    "${SRC_DIR}/linux/client/msvc_rand.cpp"
    "${SRC_DIR}/linux/tools/world_draw_engine.cpp"
    "${SRC_DIR}/linux/tools/world_draw_probe.cpp"
    "${SRC_DIR}/linux/tools/world_draw_probe_boundary.cpp"
    "${SRC_DIR}/linux/client/texture_memory.cpp"
    "${SRC_DIR}/linux/tools/engine_scene_renderer.cpp"
    "${SRC_DIR}/linux/tools/engine_effects.cpp"
    "${SRC_DIR}/linux/tools/engine_vision.cpp"
    "${SRC_DIR}/linux/tools/free_camera_replay.cpp"
    "${SRC_DIR}/linux/tools/free_camera_window_driver.cpp"
    "${SRC_DIR}/linux/client/view/free_camera.cpp"
    "${SRC_DIR}/linux/client/view/free_camera_controls.cpp"
    "${SRC_DIR}/linux/client/view/frame_owner.cpp"
    "${SRC_DIR}/linux/client/view/free_camera_window.cpp"
    "${SRC_DIR}/linux/client/window/window_host.cpp"
    "${SRC_DIR}/linux/client/window/pixel_buffer.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp"
)

add_library(KisakCOD-world-draw-native OBJECT ${KISAK_WORLD_DRAW_NATIVE_SOURCES})
add_library(KisakCOD-world-draw-standin OBJECT
    ${KISAK_WORLD_DRAW_STANDIN_SOURCES} ${KISAK_WORLD_DRAW_CLIENT_SOURCES})
add_library(KisakCOD-world-draw-own-native OBJECT ${KISAK_WORLD_DRAW_OWN_NATIVE_SOURCES})
foreach(probe_target
        KisakCOD-world-draw-native KisakCOD-world-draw-standin KisakCOD-world-draw-own-native)
    target_compile_features(${probe_target} PRIVATE cxx_std_20)
    target_compile_definitions(${probe_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_DEFINITIONS})
    target_compile_options(${probe_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_OPTIONS})
    add_dependencies(${probe_target} update_build_number)
endforeach()
foreach(probe_target KisakCOD-world-draw-native KisakCOD-world-draw-standin)
    target_include_directories(${probe_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
endforeach()
target_include_directories(KisakCOD-world-draw-own-native SYSTEM PRIVATE
    "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
target_compile_options(KisakCOD-world-draw-own-native PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(KisakCOD-world-draw-own-native PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_link_libraries(KisakCOD-world-draw-native PRIVATE kisak_linux_native_d3d9)
target_link_libraries(KisakCOD-world-draw-own-native PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)

# ---- the executable: the asset exporter and the renderer ----------------------
get_target_property(world_draw_dedi_sources KisakCOD-dedi SOURCES)
foreach(left_out
        "${SRC_DIR}/linux/server_renderer_counterparts.cpp"
        "${SRC_DIR}/linux/server_device_counterparts.cpp"
        "${SRC_DIR}/linux/server_draw_counterparts.cpp"
        "${SRC_DIR}/linux/sys_splash.cpp")
    list(FIND world_draw_dedi_sources "${left_out}" left_out_at)
    if(left_out_at EQUAL -1)
        message(FATAL_ERROR "KisakCOD-dedi no longer lists ${left_out}; review what the world draw probe leaves out")
    endif()
    list(REMOVE_ITEM world_draw_dedi_sources "${left_out}")
endforeach()
add_executable(KisakCOD-world-draw-probe ${world_draw_dedi_sources}
    "${SRC_DIR}/linux/tools/font_asset_export.cpp"
    "${SRC_DIR}/linux/tools/world_surface_export.cpp"
    "${SRC_DIR}/linux/tools/world_image_capture.cpp"
    $<TARGET_OBJECTS:KisakCOD-world-draw-native>
    $<TARGET_OBJECTS:KisakCOD-world-draw-standin>
    $<TARGET_OBJECTS:KisakCOD-world-draw-own-native>)
foreach(property COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_OPTIONS)
    get_target_property(probe_property KisakCOD-dedi ${property})
    if(probe_property)
        set_property(TARGET KisakCOD-world-draw-probe PROPERTY ${property} "${probe_property}")
    endif()
endforeach()
get_target_property(probe_definitions KisakCOD-dedi COMPILE_DEFINITIONS)
list(FILTER probe_definitions EXCLUDE REGEX "^TRACY_")
target_compile_definitions(KisakCOD-world-draw-probe PRIVATE
    ${probe_definitions} KISAK_UI_ASSET_EXPORT_ONLY KISAK_WORLD_DRAW_PROBE)
target_include_directories(KisakCOD-world-draw-probe PRIVATE "${tracy_SOURCE_DIR}/public")
# Profiling stays disabled: the offline tool must not start Tracy sockets. The
# D3D9 library and SDL3 are linked for their symbols only: the dedicated sources
# compiled here must not see the boundary's definitions or SDL's include path.
target_link_libraries(KisakCOD-world-draw-probe PRIVATE
    "${KISAK_DXVK_NATIVE_LIBRARY}" $<LINK_ONLY:SDL3::SDL3> Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-world-draw-probe update_build_number)

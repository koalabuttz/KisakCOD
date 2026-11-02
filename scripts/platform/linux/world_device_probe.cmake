# ---------------------------------------------------------------------------
# Opt-in world device probe (KISAK_BUILD_WORLD_DEVICE_PROBE)
#
# Off by default. It needs both earlier probes, because it starts from their
# renderer source lists and compiles them again for itself:
# KISAK_BUILD_WORLD_FRONTEND_PROBE (the front end, compiled with the client
# definitions) and KISAK_BUILD_DEVICE_PROBE (r_init.cpp, r_rendertarget.cpp and
# r_material.cpp through the native D3D9 boundary, DXVK Native and SDL3).
# When enabled it builds:
#
#   KisakCOD-world-device-probe   the offline asset exporter (the KisakCOD-dedi
#        sources plus the export tool files) with a device. It creates the
#        window and the Direct3D 9 device through r_init.cpp, then loads the
#        graphics zones and one allowlisted map zone with r_loadForRenderer on,
#        so the engine's own loaders create every texture, shader, vertex
#        declaration and vertex/index buffer on that device, and checks that
#        they all exist (src/linux/tools/world_device_probe.cpp).
#        KISAK_WORLD_DEVICE_MAP=<map> selects the map. It reads game data only
#        from the folder it is given and writes one report into a private
#        output directory.
#
# How it links:
#
#   * src/linux/server_device_counterparts.cpp is left out. It holds the
#     dedicated server's renderer start (CL_InitDedicated latching
#     r_loadForRenderer off) and its r_loadForRenderer-off counterparts of
#     r_init.cpp, r_image.cpp, r_image_load_obj.cpp, r_material.cpp,
#     r_material_override.cpp and rb_uploadshaders.cpp, which are linked here
#     instead. The probe brings its own CL_InitDedicated, which configures the
#     renderer and registers its dvars but creates nothing and loads no zone.
#   * src/linux/server_renderer_counterparts.cpp is left out too, as in the
#     world front-end probe: its owners are in the front-end objects.
#   * The renderer units use the client definitions (RELEASE_ASSERTS keeps
#     assertions live). Units that call D3D9 go through kisak_linux_native_d3d9;
#     the rest keep the type-only stand-in.
#   * Whatever no linked unit defines is in
#     src/linux/tools/world_device_probe_boundary.cpp: storage whose owner is
#     not linked, and functions this probe must never reach, each aborting with
#     its name.
#
# See docs/linux-world-device-probe.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_WORLD_DEVICE_PROBE
    "Build the world device probe (needs KISAK_BUILD_WORLD_FRONTEND_PROBE and KISAK_BUILD_DEVICE_PROBE)" OFF)
if(NOT KISAK_BUILD_WORLD_DEVICE_PROBE)
    return()
endif()

message(STATUS "----- KisakCOD world device probe (opt-in, DXVK Native + SDL3) -----")
if(NOT TARGET KisakCOD-world-frontend-renderer OR NOT TARGET KisakCOD-device-probe-native)
    message(FATAL_ERROR
        "KISAK_BUILD_WORLD_DEVICE_PROBE needs KISAK_BUILD_WORLD_FRONTEND_PROBE=ON and KISAK_BUILD_DEVICE_PROBE=ON "
        "(it builds on the front end and the device bring-up those probes define)")
endif()

# Production renderer units, compiled for this probe with the client definitions.
# The base is the two earlier closures: the world front end
# (KISAK_WORLD_FRONTEND_RENDERER_SOURCES and _NATIVE_SOURCES, with the generated
# Material_Sort span) and the device bring-up (KISAK_DEVICE_PROBE_NATIVE_SOURCES).
# To those it adds the owners of the device objects a zone load creates and
# releases. Units that call D3D9 go through the boundary; r_utils.cpp moves
# there too, because its R_ReleaseD3DSurface (the release every static buffer and
# shader goes through) only exists when it is compiled with the D3D9 API.
set(KISAK_WORLD_DEVICE_STANDIN_SOURCES ${KISAK_WORLD_FRONTEND_RENDERER_SOURCES})
list(REMOVE_ITEM KISAK_WORLD_DEVICE_STANDIN_SOURCES "${SRC_DIR}/gfx_d3d/r_utils.cpp")
list(APPEND KISAK_WORLD_DEVICE_STANDIN_SOURCES
    # Image_LoadFromFileWithReader: an image the zone does not carry is read from
    # images/<name>.iwi through the file system; the .iwi decoders.
    "${SRC_DIR}/gfx_d3d/r_image_load_obj.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_wavelet.cpp"
    # The technique-set remap and override table the loader and DB_PostLoadXZone call.
    "${SRC_DIR}/gfx_d3d/r_material_override.cpp"
)
set(KISAK_WORLD_DEVICE_NATIVE_SOURCES
    ${KISAK_WORLD_FRONTEND_NATIVE_SOURCES}
    ${KISAK_DEVICE_PROBE_NATIVE_SOURCES}
    "${SRC_DIR}/gfx_d3d/r_utils.cpp"
    # Texture creation and upload: Load_Texture, R_DelayLoadImage, Image_Free.
    "${SRC_DIR}/gfx_d3d/r_image.cpp"
    "${SRC_DIR}/gfx_d3d/r_image_load_common.cpp"
    # Material_UploadShaders, the loader's shader warm-up queue.
    "${SRC_DIR}/gfx_d3d/rb_uploadshaders.cpp"
)
list(REMOVE_DUPLICATES KISAK_WORLD_DEVICE_NATIVE_SOURCES)
# The probe's own code: every warning is an error, and engine headers are
# system headers for it.
set(KISAK_WORLD_DEVICE_OWN_NATIVE_SOURCES
    "${SRC_DIR}/linux/tools/world_device_probe.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp"
)
set(KISAK_WORLD_DEVICE_OWN_STANDIN_SOURCES
    "${SRC_DIR}/linux/tools/world_device_probe_boundary.cpp"
)

add_library(KisakCOD-world-device-native OBJECT ${KISAK_WORLD_DEVICE_NATIVE_SOURCES})
add_library(KisakCOD-world-device-standin OBJECT ${KISAK_WORLD_DEVICE_STANDIN_SOURCES})
add_library(KisakCOD-world-device-own-native OBJECT ${KISAK_WORLD_DEVICE_OWN_NATIVE_SOURCES})
add_library(KisakCOD-world-device-own-standin OBJECT ${KISAK_WORLD_DEVICE_OWN_STANDIN_SOURCES})
foreach(probe_target
        KisakCOD-world-device-native KisakCOD-world-device-standin
        KisakCOD-world-device-own-native KisakCOD-world-device-own-standin)
    target_compile_features(${probe_target} PRIVATE cxx_std_20)
    target_compile_definitions(${probe_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_DEFINITIONS})
    target_compile_options(${probe_target} PRIVATE ${KISAK_WORLD_FRONTEND_CLIENT_OPTIONS})
    add_dependencies(${probe_target} update_build_number)
endforeach()
foreach(probe_target KisakCOD-world-device-native KisakCOD-world-device-standin)
    target_include_directories(${probe_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
endforeach()
foreach(probe_target KisakCOD-world-device-own-native KisakCOD-world-device-own-standin)
    target_include_directories(${probe_target} SYSTEM PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
    target_compile_options(${probe_target} PRIVATE -Wall -Wextra -Werror)
endforeach()
target_compile_definitions(KisakCOD-world-device-own-native PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_link_libraries(KisakCOD-world-device-native PRIVATE kisak_linux_native_d3d9)
target_link_libraries(KisakCOD-world-device-own-native PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)

# ---- the executable: the asset exporter, the front end and the device ---------
get_target_property(world_device_dedi_sources KisakCOD-dedi SOURCES)
foreach(left_out
        "${SRC_DIR}/linux/server_renderer_counterparts.cpp"
        "${SRC_DIR}/linux/server_device_counterparts.cpp"
        "${SRC_DIR}/linux/sys_splash.cpp")
    list(FIND world_device_dedi_sources "${left_out}" left_out_at)
    if(left_out_at EQUAL -1)
        message(FATAL_ERROR "KisakCOD-dedi no longer lists ${left_out}; review what the world device probe leaves out")
    endif()
    list(REMOVE_ITEM world_device_dedi_sources "${left_out}")
endforeach()
add_executable(KisakCOD-world-device-probe ${world_device_dedi_sources}
    "${SRC_DIR}/linux/tools/font_asset_export.cpp"
    "${SRC_DIR}/linux/tools/world_surface_export.cpp"
    "${SRC_DIR}/linux/tools/world_image_capture.cpp"
    $<TARGET_OBJECTS:KisakCOD-world-device-native>
    $<TARGET_OBJECTS:KisakCOD-world-device-standin>
    $<TARGET_OBJECTS:KisakCOD-world-device-own-native>
    $<TARGET_OBJECTS:KisakCOD-world-device-own-standin>)
foreach(property COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_OPTIONS)
    get_target_property(probe_property KisakCOD-dedi ${property})
    if(probe_property)
        set_property(TARGET KisakCOD-world-device-probe PROPERTY ${property} "${probe_property}")
    endif()
endforeach()
get_target_property(probe_definitions KisakCOD-dedi COMPILE_DEFINITIONS)
list(FILTER probe_definitions EXCLUDE REGEX "^TRACY_")
target_compile_definitions(KisakCOD-world-device-probe PRIVATE
    ${probe_definitions} KISAK_UI_ASSET_EXPORT_ONLY KISAK_WORLD_DEVICE_PROBE)
target_include_directories(KisakCOD-world-device-probe PRIVATE "${tracy_SOURCE_DIR}/public")
# Profiling stays disabled: the offline tool must not start Tracy sockets. The
# D3D9 library and SDL3 are linked for their symbols only: the dedicated sources
# compiled here must not see the boundary's definitions or SDL's include path.
target_link_libraries(KisakCOD-world-device-probe PRIVATE
    "${KISAK_DXVK_NATIVE_LIBRARY}" $<LINK_ONLY:SDL3::SDL3> Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-world-device-probe update_build_number)

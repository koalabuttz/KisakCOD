# ---------------------------------------------------------------------------
# Opt-in Vulkan Legacy adapter (KISAK_BUILD_VULKAN_LEGACY)
#
# Off by default. The part of Direct3D 9 the renderer uses, on the shared
# Vulkan core (vulkan_probe.cmake) and the Legacy shader translator
# (vulkan_shaders.cmake), so it needs both of their options:
#
#   kisak_gfx_vk_legacy            src/gfx_vk/legacy: devices, textures,
#                                  surfaces, buffers with D3D9's lock rules,
#                                  shaders, vertex declarations, render and
#                                  sampler state, pipelines, passes, draws,
#                                  clears, copies, present and queries
#   KisakCOD-vulkan-legacy-check   src/gfx_vk/tools/vk_legacy_check.cpp: draws
#                                  small scenes through the adapter (with
#                                  shaders assembled from D3D9 assembly) and
#                                  checks their pixels; no game data
#
# and, where the renderer's own D3D9 declarations are available (the Windows
# SDK, or the native D3D9 boundary, KISAK_BUILD_LINUX_NATIVE_D3D9, on Linux and
# macOS):
#
#   kisak_gfx_vk_d3d9              src/gfx_vk/d3d9: IDirect3D9, IDirect3DDevice9
#                                  and the resource interfaces on the adapter,
#                                  so the renderer's D3D9 calls land on Vulkan
#   KisakCOD-vulkan-d3d9-check     src/gfx_vk/tools/vk_d3d9_check.cpp: drives
#                                  the front through the D3D9 interfaces and
#                                  checks pixels, counts and errors; no game data
#
# Nothing of the engine is compiled in here: the game's programs link the front
# with KISAK_BUILD_VULKAN_BACKEND (vulkan_backend.cmake), and the renderer
# reaches it through R_HW_Direct3DCreate9 (src/gfx_d3d/rhi) with r_backend
# vulkan. See docs/vulkan-legacy-adapter.md and docs/vulkan-d3d9-front.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VULKAN_LEGACY
    "Build the Vulkan Legacy adapter (D3D9 on the Vulkan core) and its check tool; needs KISAK_BUILD_VULKAN_PROBE and KISAK_BUILD_VULKAN_SHADERS" OFF)
if(NOT KISAK_BUILD_VULKAN_LEGACY)
    return()
endif()
if(NOT KISAK_BUILD_VULKAN_PROBE OR NOT KISAK_BUILD_VULKAN_SHADERS)
    message(FATAL_ERROR "KISAK_BUILD_VULKAN_LEGACY needs KISAK_BUILD_VULKAN_PROBE=ON and KISAK_BUILD_VULKAN_SHADERS=ON")
endif()

message(STATUS "----- KisakCOD Vulkan Legacy adapter (opt-in) -----")

add_library(kisak_gfx_vk_legacy STATIC
    "${SRC_DIR}/gfx_vk/legacy/vk_legacy_device.cpp"
    "${SRC_DIR}/gfx_vk/legacy/vk_legacy_draw.cpp"
    "${SRC_DIR}/gfx_vk/legacy/vk_legacy_resources.cpp"
    "${SRC_DIR}/gfx_vk/legacy/vk_legacy_shader_cache.cpp")
target_compile_features(kisak_gfx_vk_legacy PUBLIC cxx_std_20)
target_link_libraries(kisak_gfx_vk_legacy PUBLIC kisak_gfx_vk kisak_vk_legacy_shader)
target_compile_options(kisak_gfx_vk_legacy PRIVATE ${KISAK_VULKAN_WARNINGS})
if(MSVC)
    target_compile_definitions(kisak_gfx_vk_legacy PRIVATE NOMINMAX)
endif()

# The check tools need SDL3; the Windows game's libraries-only build
# (KISAK_VULKAN_LIBRARIES_ONLY, vulkan_probe.cmake) leaves them out.
if(NOT KISAK_VULKAN_LIBRARIES_ONLY)
    add_executable(KisakCOD-vulkan-legacy-check "${SRC_DIR}/gfx_vk/tools/vk_legacy_check.cpp")
    target_compile_options(KisakCOD-vulkan-legacy-check PRIVATE ${KISAK_VULKAN_WARNINGS})
    target_link_libraries(KisakCOD-vulkan-legacy-check PRIVATE kisak_gfx_vk_legacy SDL3::SDL3)
    if(MSVC)
        target_compile_definitions(KisakCOD-vulkan-legacy-check PRIVATE NOMINMAX)
    endif()
    if(WIN32)
        add_custom_command(TARGET KisakCOD-vulkan-legacy-check POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:KisakCOD-vulkan-legacy-check> $<TARGET_FILE_DIR:KisakCOD-vulkan-legacy-check>
            COMMAND_EXPAND_LISTS)
    endif()
endif()

# The D3D9 front, compiled against the same D3D9 declarations as the renderer.
if(WIN32 OR TARGET kisak_linux_native_d3d9)
    set(vk_d3d9_sources
        "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_backend_vulkan.cpp"
        "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_device.cpp"
        "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_direct3d.cpp"
        "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_resources.cpp")
    add_library(kisak_gfx_vk_d3d9 STATIC ${vk_d3d9_sources})
    target_link_libraries(kisak_gfx_vk_d3d9 PUBLIC kisak_gfx_vk_legacy)
    target_include_directories(kisak_gfx_vk_d3d9 PUBLIC "${SRC_DIR}")
    target_compile_options(kisak_gfx_vk_d3d9 PRIVATE ${KISAK_VULKAN_WARNINGS})
    if(MSVC)
        target_compile_definitions(kisak_gfx_vk_d3d9 PRIVATE NOMINMAX)
    endif()
    if(NOT WIN32)
        # DXVK Native's COM headers need Microsoft extensions (anonymous
        # structs and unions), as every renderer unit that includes them does;
        # the boundary's __stdcall is ignored off i386, as in the engine.
        target_link_libraries(kisak_gfx_vk_d3d9 PUBLIC kisak_linux_native_d3d9)
        target_compile_options(kisak_gfx_vk_d3d9 PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
            -Wno-ignored-attributes)
    endif()

    if(KISAK_VULKAN_LIBRARIES_ONLY)
        return()
    endif()
    add_executable(KisakCOD-vulkan-d3d9-check "${SRC_DIR}/gfx_vk/tools/vk_d3d9_check.cpp")
    target_compile_options(KisakCOD-vulkan-d3d9-check PRIVATE ${KISAK_VULKAN_WARNINGS})
    target_link_libraries(KisakCOD-vulkan-d3d9-check PRIVATE kisak_gfx_vk_d3d9 SDL3::SDL3)
    if(MSVC)
        target_compile_definitions(KisakCOD-vulkan-d3d9-check PRIVATE NOMINMAX)
    endif()
    if(WIN32)
        add_custom_command(TARGET KisakCOD-vulkan-d3d9-check POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:KisakCOD-vulkan-d3d9-check> $<TARGET_FILE_DIR:KisakCOD-vulkan-d3d9-check>
            COMMAND_EXPAND_LISTS)
    endif()
else()
    message(STATUS "Vulkan D3D9 front: skipped (needs the Windows SDK or KISAK_BUILD_LINUX_NATIVE_D3D9)")
endif()

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
# Nothing of the engine is compiled in yet: the renderer reaches it through
# src/gfx_d3d/rhi/r_rhi.h in a later step. See docs/vulkan-legacy-adapter.md.
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
    "${SRC_DIR}/gfx_vk/legacy/vk_legacy_resources.cpp")
target_compile_features(kisak_gfx_vk_legacy PUBLIC cxx_std_20)
target_link_libraries(kisak_gfx_vk_legacy PUBLIC kisak_gfx_vk kisak_vk_legacy_shader)
target_compile_options(kisak_gfx_vk_legacy PRIVATE ${KISAK_VULKAN_WARNINGS})
if(MSVC)
    target_compile_definitions(kisak_gfx_vk_legacy PRIVATE NOMINMAX)
endif()

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

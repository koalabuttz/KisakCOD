# ---------------------------------------------------------------------------
# KisakCOD-gxm-probe: the PS Vita client's graphics layer on its own
# (src/gfx_gxm/tools/gxm_probe.cpp, docs/vita-client.md "GXM probe").
#
# Built with the client (KISAK_BUILD_VITA_MP_CLIENT). It brings up sceGxm and
# the GPU memory pools, loads the user's shader compiler, compiles and draws a
# few small shaders and logs the times and memory to
# ux0:data/KisakCOD/gxm-probe.log. No engine code and no game data.
# ---------------------------------------------------------------------------
add_executable(KisakCOD-gxm-probe
    "${SRC_DIR}/gfx_gxm/gxm_display.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_heap.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_memory.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_shader_compiler.cpp"
    "${SRC_DIR}/gfx_gxm/tools/gxm_probe.cpp"
)
target_compile_features(KisakCOD-gxm-probe PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-gxm-probe PRIVATE ENABLE_VITASDK_BUILD_ASSERT)
target_include_directories(KisakCOD-gxm-probe PRIVATE "${SRC_DIR}")
target_compile_options(KisakCOD-gxm-probe PRIVATE -Wall -Werror=format-security)
# psp2/display.h's enum size checks assume -fshort-enums (gxm_display.cpp).
set_source_files_properties("${SRC_DIR}/gfx_gxm/gxm_display.cpp" TARGET_DIRECTORY KisakCOD-gxm-probe
    PROPERTIES COMPILE_OPTIONS "-UENABLE_VITASDK_BUILD_ASSERT")
target_link_options(KisakCOD-gxm-probe PRIVATE ${KISAK_VITA_LINK_OPTIONS})
# The shader compiler's imports are weak: the probe starts without
# libshacccg.suprx and reports that it is missing.
target_link_libraries(KisakCOD-gxm-probe PRIVATE
    SceGxm_stub SceDisplay_stub SceShaccCg_stub_weak ${KISAK_VITA_LINK_LIBRARIES})

set(KISAK_VITA_GXM_PROBE_TITLE_ID "KSKD00003" CACHE STRING "PS Vita title ID of the GXM probe .vpk (9 characters)")
# UNSAFE: the shader compiler lives in ur0:data, which safe homebrew cannot read.
kisak_vita_pack_vpk(KisakCOD-gxm-probe "${KISAK_VITA_GXM_PROBE_TITLE_ID}" "KisakCOD GXM Probe" UNSAFE)

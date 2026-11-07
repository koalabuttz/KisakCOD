# ---------------------------------------------------------------------------
# Opt-in native D3D9 boundary (KISAK_BUILD_LINUX_NATIVE_D3D9)
#
# Off by default. When enabled, this provides the one build boundary through
# which engine renderer code reaches DXVK Native D3D9 (i386):
#
#   kisak_linux_native_d3d9   INTERFACE target. A translation unit that links it
#                             gets KISAK_LINUX_NATIVE_D3D9 and the DXVK include
#                             directory, so <linux/d3d9_types.h> resolves to the
#                             real D3D9 interfaces through src/linux/d3d9_native.h.
#                             Every other unit keeps the type-only stand-in.
#
# and four compile proofs, none of which is a renderer or links DXVK:
#
#   KisakCOD-native-d3d9-boundary-check   the boundary header on its own
#   KisakCOD-native-d3d9-layout-standin   D3D9-bearing engine layouts, stand-in view
#   KisakCOD-native-d3d9-layout-native    the same layouts, DXVK view
#   KisakCOD-native-d3d9-renderer-proof   OBJECT library of the production
#                                         renderer units listed in
#                                         native_d3d9_renderer_sources.txt
#
# The first three describe the i386 layouts and calling conventions; under
# KISAK_LINUX_ARCH=x86_64 or aarch64, and on macOS, only the renderer proof is
# built.
#
# scripts/tests/run_native_d3d9_boundary_checks.py runs the same proofs without
# CMake and compares the two layout views. The dedicated server never sees any
# of this. See docs/linux-native-d3d9.md.
# ---------------------------------------------------------------------------
if(NOT KISAK_BUILD_LINUX_NATIVE_D3D9)
    return()
endif()

message(STATUS "----- KisakCOD native D3D9 boundary (opt-in, DXVK Native) -----")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT KISAK_NATIVE_ARCH MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "KISAK_BUILD_LINUX_NATIVE_D3D9 needs a 32-bit (-m32) build, or KISAK_LINUX_ARCH=x86_64 or aarch64 (or KISAK_PLATFORM=macos) for the 64-bit tracks")
endif()
if(NOT KISAK_DXVK_NATIVE_INCLUDE_DIR)
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_NATIVE_D3D9 needs KISAK_DXVK_NATIVE_INCLUDE_DIR: the DXVK Native "
        "include directory that holds d3d9.h and windows.h (<prefix>/include/dxvk)")
endif()
get_filename_component(KISAK_DXVK_NATIVE_INCLUDE_DIR "${KISAK_DXVK_NATIVE_INCLUDE_DIR}" ABSOLUTE)
foreach(dxvk_header d3d9.h d3d9types.h d3d9caps.h windows.h windows_base.h unknwn.h)
    if(NOT EXISTS "${KISAK_DXVK_NATIVE_INCLUDE_DIR}/${dxvk_header}")
        message(FATAL_ERROR "KISAK_DXVK_NATIVE_INCLUDE_DIR has no ${dxvk_header}: ${KISAK_DXVK_NATIVE_INCLUDE_DIR}")
    endif()
endforeach()
message(STATUS "Native D3D9: DXVK Native headers from ${KISAK_DXVK_NATIVE_INCLUDE_DIR}")

add_library(kisak_linux_native_d3d9 INTERFACE)
target_compile_definitions(kisak_linux_native_d3d9 INTERFACE KISAK_LINUX_NATIVE_D3D9)
# SYSTEM: DXVK's headers are third-party; the engine's warning settings are not
# applied to them.
target_include_directories(kisak_linux_native_d3d9 SYSTEM INTERFACE "${KISAK_DXVK_NATIVE_INCLUDE_DIR}")
if(KISAK_DXVK_NATIVE_LIBRARY)
    if(NOT EXISTS "${KISAK_DXVK_NATIVE_LIBRARY}")
        message(FATAL_ERROR "KISAK_DXVK_NATIVE_LIBRARY does not exist: ${KISAK_DXVK_NATIVE_LIBRARY}")
    endif()
    # Only targets that create a device need the library; the proofs below do not.
    target_link_libraries(kisak_linux_native_d3d9 INTERFACE "${KISAK_DXVK_NATIVE_LIBRARY}")
    message(STATUS "Native D3D9: linking ${KISAK_DXVK_NATIVE_LIBRARY}")
endif()

set(KISAK_NATIVE_D3D9_ENGINE_DEFINITIONS
    KISAK_MP
    # Selected for the portable type-shim branch of src/sound/snd_public.h, exactly
    # as KisakCOD-client-core does; no sound source is compiled here.
    KISAK_OPENAL
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST
    $<$<CONFIG:Debug>:_DEBUG>
)
set(KISAK_NATIVE_D3D9_ENGINE_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    ${KISAK_LINUX_SIMD_OPTIONS} -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Werror=format-security
)

# The boundary header alone, and the D3D9-bearing engine layouts once per view.
# Both check the documented Windows x86-32 sizes and the i386 stdcall/cdecl
# split, so the x86_64 track builds the renderer proof only; there every unit
# that includes the boundary checks that __stdcall is the plain C convention.
set(native_d3d9_i386_checks "")
if(KISAK_NATIVE_ARCH STREQUAL "i386")
    add_executable(KisakCOD-native-d3d9-boundary-check
        "${TESTS_DIR}/linux_d3d9_boundary/boundary_header_check.cpp")
    target_compile_features(KisakCOD-native-d3d9-boundary-check PRIVATE cxx_std_20)
    target_include_directories(KisakCOD-native-d3d9-boundary-check PRIVATE "${SRC_DIR}")
    target_compile_options(KisakCOD-native-d3d9-boundary-check PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions> -Wall -Wextra -Werror)
    target_link_libraries(KisakCOD-native-d3d9-boundary-check PRIVATE kisak_linux_native_d3d9)

    # The D3D9-bearing engine layouts, once per view.
    foreach(layout_view standin native)
        set(layout_target KisakCOD-native-d3d9-layout-${layout_view})
        add_executable(${layout_target} "${TESTS_DIR}/linux_d3d9_boundary/engine_layout_check.cpp")
        target_compile_features(${layout_target} PRIVATE cxx_std_20)
        target_compile_definitions(${layout_target} PRIVATE ${KISAK_NATIVE_D3D9_ENGINE_DEFINITIONS})
        target_include_directories(${layout_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
        target_compile_options(${layout_target} PRIVATE ${KISAK_NATIVE_D3D9_ENGINE_OPTIONS})
        if(layout_view STREQUAL "native")
            target_link_libraries(${layout_target} PRIVATE kisak_linux_native_d3d9)
        endif()
    endforeach()
    list(APPEND native_d3d9_i386_checks KisakCOD-native-d3d9-boundary-check
        KisakCOD-native-d3d9-layout-standin KisakCOD-native-d3d9-layout-native)
endif()

# Production renderer units that call D3D9, compiled unmodified through the
# boundary. An OBJECT library: nothing links these objects.
file(STRINGS "${SCRIPTS_DIR}/platform/linux/native_d3d9_renderer_sources.txt" native_d3d9_proof_lines)
set(KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES "")
foreach(proof_line IN LISTS native_d3d9_proof_lines)
    string(STRIP "${proof_line}" proof_line)
    if(proof_line STREQUAL "" OR proof_line MATCHES "^#")
        continue()
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${proof_line}")
        message(FATAL_ERROR "native_d3d9_renderer_sources.txt names a missing file: ${proof_line}")
    endif()
    list(APPEND KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES "${CMAKE_SOURCE_DIR}/${proof_line}")
endforeach()
list(LENGTH KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES native_d3d9_proof_count)
if(native_d3d9_proof_count EQUAL 0)
    message(FATAL_ERROR "native_d3d9_renderer_sources.txt lists no source")
endif()
add_library(KisakCOD-native-d3d9-renderer-proof OBJECT ${KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES})
target_compile_features(KisakCOD-native-d3d9-renderer-proof PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-native-d3d9-renderer-proof PRIVATE ${KISAK_NATIVE_D3D9_ENGINE_DEFINITIONS})
target_include_directories(KisakCOD-native-d3d9-renderer-proof PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
target_compile_options(KisakCOD-native-d3d9-renderer-proof PRIVATE ${KISAK_NATIVE_D3D9_ENGINE_OPTIONS})
target_link_libraries(KisakCOD-native-d3d9-renderer-proof PRIVATE kisak_linux_native_d3d9)
message(STATUS "Native D3D9: ${native_d3d9_proof_count} renderer unit(s) in the compile proof")

add_custom_target(KisakCOD-native-d3d9-proofs
    DEPENDS
        ${native_d3d9_i386_checks}
        KisakCOD-native-d3d9-renderer-proof
)

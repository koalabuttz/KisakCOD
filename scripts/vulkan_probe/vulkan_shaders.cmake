# ---------------------------------------------------------------------------
# Opt-in Legacy shader translator (KISAK_BUILD_VULKAN_SHADERS)
#
# Off by default. Turns the game's Direct3D 9 shader bytecode (vs/ps 1.1 to
# 3.0) into SPIR-V for the Vulkan back end's Legacy path, through dxbc-spirv
# (MIT, github.com/doitsujin/dxbc-spirv), the converter DXVK 3.1 uses for D3D9
# and the one the renderer plan's shader spike chose (docs/shader-translator-spike.md).
#
#   kisak_dxbc_spirv          dxbc-spirv's SM1-3 front end, IR passes and SPIR-V
#                             back end, at the commit DXVK 3.1.1 ships
#   kisak_vk_legacy_shader    src/gfx_vk/shader: our lowering of D3D9's legacy
#                             inputs to the Legacy descriptor layout, I/O
#                             linking between stages, and reflection
#   KisakCOD-vulkan-shaders   src/gfx_vk/tools/vk_shader_tool.cpp: translates
#                             bytecode files and prints what each shader uses
#
# dxbc-spirv and its SPIR-V headers are fetched at configure time at pinned
# commits; FETCHCONTENT_SOURCE_DIR_KISAK_DXBC_SPIRV can point at a local
# checkout of the same commit (with its submodule) on a machine without network.
# Nothing here needs the Vulkan headers, SDL3, the engine or game data, and the
# translator runs on every KISAK_LINUX_ARCH, Windows (MSVC) and macOS.
# See docs/vulkan-legacy-shaders.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VULKAN_SHADERS
    "Build the Legacy shader translator (D3D9 bytecode to SPIR-V through dxbc-spirv) and its tool" OFF)
if(NOT KISAK_BUILD_VULKAN_SHADERS)
    return()
endif()

message(STATUS "----- KisakCOD Legacy shader translator (opt-in, dxbc-spirv) -----")

include(FetchContent)
# The commit DXVK 3.1.1 pins as its subprojects/dxbc-spirv, which drew CoD4
# correctly on the laptop in the shader spike (S0). Its submodule pins the
# SPIR-V headers (c8ad050fcb29e42a2f57d9f59e97488f465c436d).
set(KISAK_DXBC_SPIRV_COMMIT bf14419e5fa7eacb817b7b632f03cb61d61bbad7)
set(KISAK_SPIRV_HEADERS_COMMIT c8ad050fcb29e42a2f57d9f59e97488f465c436d)
FetchContent_Declare(kisak_dxbc_spirv
    GIT_REPOSITORY https://github.com/doitsujin/dxbc-spirv.git
    GIT_TAG ${KISAK_DXBC_SPIRV_COMMIT}
    GIT_SUBMODULES "submodules/spirv_headers"
    GIT_PROGRESS FALSE)
# dxbc-spirv builds with meson; only its sources are used, so populate without
# adding a subdirectory.
FetchContent_GetProperties(kisak_dxbc_spirv)
if(NOT kisak_dxbc_spirv_POPULATED)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(kisak_dxbc_spirv)
endif()
set(KISAK_DXBC_SPIRV_DIR "${kisak_dxbc_spirv_SOURCE_DIR}")

# Refuse a checkout that is not the pinned commits (a local source directory
# without git metadata is not checked, as with Tracy).
find_package(Git QUIET)
foreach(_kisak_pin IN ITEMS "${KISAK_DXBC_SPIRV_DIR}|${KISAK_DXBC_SPIRV_COMMIT}"
                            "${KISAK_DXBC_SPIRV_DIR}/submodules/spirv_headers|${KISAK_SPIRV_HEADERS_COMMIT}")
    string(REPLACE "|" ";" _kisak_pin "${_kisak_pin}")
    list(GET _kisak_pin 0 _kisak_pin_dir)
    list(GET _kisak_pin 1 _kisak_pin_commit)
    if(GIT_EXECUTABLE AND EXISTS "${_kisak_pin_dir}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_kisak_pin_dir}" rev-parse HEAD
            OUTPUT_VARIABLE _kisak_pin_head OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _kisak_pin_result)
        if(NOT _kisak_pin_result EQUAL 0 OR NOT _kisak_pin_head STREQUAL _kisak_pin_commit)
            message(FATAL_ERROR "${_kisak_pin_dir} is at ${_kisak_pin_head}, expected ${_kisak_pin_commit}")
        endif()
    endif()
endforeach()
if(NOT EXISTS "${KISAK_DXBC_SPIRV_DIR}/submodules/spirv_headers/include/spirv/unified1/spirv.hpp")
    message(FATAL_ERROR "dxbc-spirv's SPIR-V headers submodule is missing in ${KISAK_DXBC_SPIRV_DIR}")
endif()
message(STATUS "Legacy shaders: dxbc-spirv ${KISAK_DXBC_SPIRV_COMMIT} from ${KISAK_DXBC_SPIRV_DIR}")

# The library: the files dxbc-spirv's meson.build lists, without the SM4/5
# front end (D3D9 bytecode only).
file(GLOB KISAK_DXBC_SPIRV_SOURCES CONFIGURE_DEPENDS
    "${KISAK_DXBC_SPIRV_DIR}/ir/*.cpp"
    "${KISAK_DXBC_SPIRV_DIR}/ir/passes/*.cpp"
    "${KISAK_DXBC_SPIRV_DIR}/sm3/*.cpp"
    "${KISAK_DXBC_SPIRV_DIR}/spirv/*.cpp"
    "${KISAK_DXBC_SPIRV_DIR}/util/*.cpp")
add_library(kisak_dxbc_spirv STATIC ${KISAK_DXBC_SPIRV_SOURCES})
target_compile_features(kisak_dxbc_spirv PUBLIC cxx_std_17)
target_include_directories(kisak_dxbc_spirv SYSTEM PUBLIC "${KISAK_DXBC_SPIRV_DIR}")
target_compile_definitions(kisak_dxbc_spirv PUBLIC DXBC_SPV_ENABLE_SM3 DXBC_SPV_ENABLE_SPIRV)
if(MSVC)
    target_compile_options(kisak_dxbc_spirv PRIVATE /W0 /bigobj)
    target_compile_definitions(kisak_dxbc_spirv PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(kisak_dxbc_spirv PRIVATE -w)
endif()

# Our part: engine-independent, every warning an error.
add_library(kisak_vk_legacy_shader STATIC
    "${SRC_DIR}/gfx_vk/shader/vk_legacy_shader.cpp")
target_compile_features(kisak_vk_legacy_shader PUBLIC cxx_std_20)
target_include_directories(kisak_vk_legacy_shader PUBLIC "${SRC_DIR}")
target_link_libraries(kisak_vk_legacy_shader PRIVATE kisak_dxbc_spirv)
if(MSVC)
    set(KISAK_VULKAN_SHADER_WARNINGS /W4 /WX /permissive-)
    target_compile_definitions(kisak_vk_legacy_shader PRIVATE NOMINMAX)
else()
    set(KISAK_VULKAN_SHADER_WARNINGS -Wall -Wextra -Werror)
endif()
target_compile_options(kisak_vk_legacy_shader PRIVATE ${KISAK_VULKAN_SHADER_WARNINGS})

add_executable(KisakCOD-vulkan-shaders "${SRC_DIR}/gfx_vk/tools/vk_shader_tool.cpp")
target_compile_options(KisakCOD-vulkan-shaders PRIVATE ${KISAK_VULKAN_SHADER_WARNINGS})
target_link_libraries(KisakCOD-vulkan-shaders PRIVATE kisak_vk_legacy_shader)

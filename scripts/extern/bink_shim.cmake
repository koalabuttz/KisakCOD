# Movies on 64-bit Windows (KISAK_WIN64_CINEMATICS). binkw32 exists only as a
# 32-bit DLL, so the target plays Bink movies through the FFmpeg Bink shim the
# Linux client uses (src/linux/client/cinematic, docs/linux-cinematics.md):
# the Bink 1.9 API on FFmpeg's libavcodec, with sound through OpenAL, and the
# shim's own Lock_Bink_textures/Unlock_Bink_textures (bink_textures.cpp, on
# RAD's binktextures.h) in place of RAD's binktextures.cpp: RAD locks the
# buffer the shim decodes into read-only.
#
# FFmpeg's libavcodec and libavutil (LGPL, shared libraries) are found under
# KISAK_FFMPEG_ROOT, a folder with include/, lib/ and bin/ such as a vcpkg
# installed/x64-windows tree or an FFmpeg "shared" release, or on the default
# search path. Their DLLs are copied next to the executable.

set(KISAK_FFMPEG_ROOT "" CACHE PATH "Folder holding FFmpeg's include/, lib/ and bin/ (64-bit Windows movies)")
file(TO_CMAKE_PATH "${KISAK_FFMPEG_ROOT}" KISAK_FFMPEG_ROOT_DIR)

find_path(KISAK_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    HINTS "${KISAK_FFMPEG_ROOT_DIR}" PATH_SUFFIXES include)
find_library(KISAK_AVCODEC_LIBRARY NAMES avcodec
    HINTS "${KISAK_FFMPEG_ROOT_DIR}" PATH_SUFFIXES lib)
find_library(KISAK_AVUTIL_LIBRARY NAMES avutil
    HINTS "${KISAK_FFMPEG_ROOT_DIR}" PATH_SUFFIXES lib)
if (NOT KISAK_FFMPEG_INCLUDE_DIR OR NOT KISAK_AVCODEC_LIBRARY OR NOT KISAK_AVUTIL_LIBRARY)
    message(FATAL_ERROR
        "KISAK_WIN64_CINEMATICS needs FFmpeg's libavcodec and libavutil (headers, import libraries and DLLs). "
        "Point KISAK_FFMPEG_ROOT at a folder with include/, lib/ and bin/, for example the output of "
        "'vcpkg install ffmpeg[core,avcodec]:x64-windows' (vcpkg/installed/x64-windows).")
endif()

target_sources(${PROJECT_NAME} PRIVATE
    "${SRC_DIR}/linux/client/cinematic/bink_audio_openal.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_decoder.h"
    "${SRC_DIR}/linux/client/cinematic/bink_decoder_ffmpeg.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_file.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_file.h"
    "${SRC_DIR}/linux/client/cinematic/bink_player.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_shim.h"
    "${SRC_DIR}/linux/client/cinematic/bink_textures.cpp"
    "${DEPS_DIR}/binklib/bink.h"
    "${DEPS_DIR}/binklib/binktextures.h"
    "${DEPS_DIR}/binklib/radbase.h")
source_group("binklib\\shim" FILES
    "${SRC_DIR}/linux/client/cinematic/bink_audio_openal.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_decoder.h"
    "${SRC_DIR}/linux/client/cinematic/bink_decoder_ffmpeg.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_file.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_file.h"
    "${SRC_DIR}/linux/client/cinematic/bink_player.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_shim.h"
    "${SRC_DIR}/linux/client/cinematic/bink_textures.cpp"
    "${DEPS_DIR}/binklib/bink.h"
    "${DEPS_DIR}/binklib/binktextures.h"
    "${DEPS_DIR}/binklib/radbase.h")

# The engine and the shim both see the Bink API as plain functions of this
# executable (radbase.h: no dllimport with __RADINEXE__).
target_compile_definitions(${PROJECT_NAME} PUBLIC CINEMA __RADINEXE__)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${KISAK_FFMPEG_INCLUDE_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE "${KISAK_AVCODEC_LIBRARY}" "${KISAK_AVUTIL_LIBRARY}")

# FFmpeg's DLLs (avcodec-NN.dll, avutil-NN.dll, and swresample-NN.dll where a
# build links it in) sit in bin/ beside lib/.
get_filename_component(KISAK_FFMPEG_LIB_DIR "${KISAK_AVCODEC_LIBRARY}" DIRECTORY)
file(GLOB KISAK_FFMPEG_DLLS
    "${KISAK_FFMPEG_LIB_DIR}/../bin/avcodec*.dll"
    "${KISAK_FFMPEG_LIB_DIR}/../bin/avutil*.dll"
    "${KISAK_FFMPEG_LIB_DIR}/../bin/swresample*.dll")
if (KISAK_FFMPEG_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KISAK_FFMPEG_DLLS} "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "COPYING FFMPEG DLLS"
        VERBATIM)
else()
    message(WARNING "No FFmpeg DLLs next to ${KISAK_FFMPEG_LIB_DIR}; copy avcodec and avutil next to the executable yourself")
endif()

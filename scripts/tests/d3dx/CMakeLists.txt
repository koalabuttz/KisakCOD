cmake_minimum_required(VERSION 3.20)
project(KisakD3DXRuntimeCheck LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This check requires a Windows target (native or cross-compiled).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure a 32-bit target, matching KisakCOD.")
endif()

add_executable(kisakcod-d3dx-runtime ../d3dx_shader_runtime.cpp)
target_compile_features(kisakcod-d3dx-runtime PRIVATE cxx_std_17)

if(MINGW)
    target_link_libraries(kisakcod-d3dx-runtime PRIVATE d3dx9_43)
    target_link_options(kisakcod-d3dx-runtime PRIVATE -static)
else()
    find_path(D3DX9_INCLUDE_DIR d3dx9.h
        HINTS "${DXSDK_DIR}/include" "${DXSDK_DIR}/Include" REQUIRED)
    find_library(D3DX9_LIBRARY d3dx9
        HINTS "${DXSDK_DIR}/release/lib/x86" "${DXSDK_DIR}/Lib/x86" REQUIRED)
    target_include_directories(kisakcod-d3dx-runtime PRIVATE "${D3DX9_INCLUDE_DIR}")
    target_link_libraries(kisakcod-d3dx-runtime PRIVATE "${D3DX9_LIBRARY}")
endif()

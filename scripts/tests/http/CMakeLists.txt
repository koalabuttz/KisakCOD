cmake_minimum_required(VERSION 3.18)
project(kisakcod-http-runtime-check C CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(KISAKCOD_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(HTTP_PROBE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/http-runtime-check.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${KISAKCOD_SOURCE_ROOT}/src/qcommon/dl_main.cpp"
    "${KISAKCOD_SOURCE_ROOT}/scripts/tests/test_http_downloads.py"
)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${KISAKCOD_SOURCE_ROOT}/scripts/tests/test_http_downloads.py"
        --source-root "${KISAKCOD_SOURCE_ROOT}" --emit-harness "${HTTP_PROBE_SOURCE}"
    RESULT_VARIABLE HTTP_PROBE_EMIT_RESULT
)
if(NOT HTTP_PROBE_EMIT_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate the production HTTP runtime probe")
endif()

add_executable(${PROJECT_NAME} "${HTTP_PROBE_SOURCE}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /UNDEBUG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
    if(NOT WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
include("${KISAKCOD_SOURCE_ROOT}/scripts/extern/curl.cmake")

include(FetchContent)

##### Tracy #####
option ( TRACY_ENABLE "" ON )
option ( TRACY_ON_DEMAND "" ON )
option ( TRACY_ONLY_LOCALHOST "" ON )
#option ( TRACY_FIBERS "" ON )

FetchContent_Declare (
	tracy
	GIT_REPOSITORY https://github.com/wolfpld/tracy.git
	GIT_TAG v0.12.2
	GIT_SHALLOW TRUE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable ( tracy )

# The tag above is mutable; refuse a checkout that is not the commit it named
# when this pin was set (v0.12.2 = c556831ddc6f).  A source directory supplied
# through FETCHCONTENT_SOURCE_DIR_TRACY without git metadata is not checked.
set(KISAK_TRACY_COMMIT c556831ddc6fe26d2fce01c14c97205a9dad46d5)
find_package(Git QUIET)
if(GIT_EXECUTABLE AND EXISTS "${tracy_SOURCE_DIR}/.git")
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${tracy_SOURCE_DIR}" rev-parse HEAD
		OUTPUT_VARIABLE KISAK_TRACY_HEAD
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE KISAK_TRACY_REV_RESULT)
	if(NOT KISAK_TRACY_REV_RESULT EQUAL 0 OR NOT KISAK_TRACY_HEAD STREQUAL KISAK_TRACY_COMMIT)
		message(FATAL_ERROR "tracy checkout is ${KISAK_TRACY_HEAD}, expected ${KISAK_TRACY_COMMIT} (v0.12.2); the tag may have moved")
	endif()
endif()

set_property(TARGET TracyClient PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#Tracy is default off, but turned on by simply defining TRACY_ENABLE
set_property(TARGET TracyClient PROPERTY INTERFACE_COMPILE_DEFINITIONS TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_BINARY_DIR}/_deps/tracy-src/public)

# Windows profiling builds (KISAK_WIN_TRACY in the root CMakeLists.txt): the
# game's PROF_SCOPED zones (src/universal/profile.h) become Tracy zones, with
# the same definitions as the Linux profiling builds.
if(KISAK_WIN_TRACY AND NOT KISAK_PLATFORM STREQUAL "linux")
	target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
	message(STATUS "${PROJECT_NAME}: Tracy profiler on (on demand, localhost only)")
endif()
# Linux: only profiling builds (KISAK_LINUX_TRACY) link the Tracy client; it
# starts a localhost listener as soon as it is linked in.
if(NOT KISAK_PLATFORM STREQUAL "linux" OR KISAK_LINUX_TRACY)
	target_link_libraries(${PROJECT_NAME} PUBLIC TracyClient)
endif()
#################
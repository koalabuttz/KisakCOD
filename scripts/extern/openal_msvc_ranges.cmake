# Patch step for the fetched OpenAL Soft 1.25.2 sources (see openal.cmake).
#
# MSVC 14.52 (Visual Studio 2026, the windows-11-arm image from 2026-11-13)
# rejects alc/alc.cpp's
#     std::views::transform(&ContextBase::EffectSlotCluster::operator*)
# with C3889 ("call to object of class type std::ranges::views::_Transform_fn:
# no matching call operator"): the address of unique_ptr's operator* no longer
# works as a transform function. OpenAL Soft's master replaced it with a
# dereferencing function object after 1.25.2; this does the same with a lambda.
# The tag has no newer release, and the engine pins that exact commit.
#
# Usage: cmake -DOPENAL_SRC=<source dir> -P openal_msvc_ranges.cmake
# Running it again, or on sources that no longer have the old text, changes nothing.
if(NOT OPENAL_SRC)
    message(FATAL_ERROR "openal_msvc_ranges.cmake: pass -DOPENAL_SRC=<openal source dir>")
endif()
set(alc_cpp "${OPENAL_SRC}/alc/alc.cpp")
file(READ "${alc_cpp}" text)
set(old "std::views::transform(&ContextBase::EffectSlotCluster::operator*)")
set(new "std::views::transform([](auto &cluster) -> decltype(auto) { return *cluster; })")
string(FIND "${text}" "${old}" position)
if(position EQUAL -1)
    message(STATUS "openal_msvc_ranges: alc.cpp has no EffectSlotCluster::operator* transform to patch")
else()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${alc_cpp}" "${text}")
    message(STATUS "openal_msvc_ranges: patched ${alc_cpp}")
endif()

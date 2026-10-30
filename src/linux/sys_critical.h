#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Native critical sections.
//
// Replaces the critical-section half of universal/win_common.cpp. The engine's
// recursion behaviour is required, not incidental: Com_Printf takes
// CRITSECT_CONSOLE and is called from code that already holds it, and the
// fatal-error path re-enters CRITSECT_FATAL_ERROR. Each section is therefore a
// genuinely recursive mutex.
//
// The std::mutex array named s_criticalSections exists only because
// <win32/win_local.h> declares it that way for non-Windows builds; the real
// locks live in this translation unit.
// ---------------------------------------------------------------------------

void Sys_InitializeCriticalSections();
void Sys_EnterCriticalSection(int critSect);
void Sys_LeaveCriticalSection(int critSect);

// Recursion depth of a section on the calling thread, for the self-test.
int Posix_CriticalSectionDepthForTest(int critSect);

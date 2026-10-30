#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine-facing threading declarations.
//
// The POSIX primitives themselves live in sys_posix.h / sys_posix.cpp so they
// can be tested without the engine. sys_threads.cpp implements the engine API
// declared in <qcommon/threads.h> on top of them.
// ---------------------------------------------------------------------------

#include "sys_posix.h"

#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine-free POSIX primitives.
//
// These are the parts of the Linux runtime that do not touch engine types, kept
// in their own translation unit so they can be built and exercised by a
// standalone test program without the engine. The engine-facing wrappers in
// sys_threads.cpp / sys_time.cpp / sys_critical.cpp / sys_memory.cpp delegate
// here, so the test exercises the same code the server ships.
// ---------------------------------------------------------------------------

// --- Monotonic clock -------------------------------------------------------

uint64_t Posix_MonotonicNanoseconds();

// --- Events ----------------------------------------------------------------
//
// Win32-compatible semantics, which the engine depends on:
//   manual-reset  remains signalled until reset; wakes every waiter
//   auto-reset    clears on release; wakes exactly one waiter

struct PosixEvent;

PosixEvent *Posix_CreateEvent(bool manualReset, bool initialState);
void Posix_SetEvent(PosixEvent *event);
void Posix_ResetEvent(PosixEvent *event);
void Posix_DestroyEvent(PosixEvent *event);

// Returns true if signalled, false on timeout. msec must be finite.
bool Posix_WaitEvent(PosixEvent *event, uint32_t msec);
void Posix_WaitEventInfinite(PosixEvent *event);

// --- Cooperative thread suspension ----------------------------------------
//
// Linux cannot safely suspend a thread asynchronously, so a suspend request is
// latched on the gate and the thread honours it at the next checkpoint. A
// checkpoint sits on the way into every blocking wait, immediately after that
// wait returns (timed-out waits included), and immediately before a thread runs
// engine code. Requests are counted so they nest exactly like the Win32 suspend
// count: two Sys_SuspendThread calls need two Sys_ResumeThread calls.
//
// Gates are usable before the thread they describe exists, which is how a
// thread is created suspended (CreateThread's CREATE_SUSPENDED).

void Posix_InitSuspendGates(int count);
// Adds one suspend request to the gate. May be called before the owning thread
// exists, so the thread observes the request as soon as it starts.
void Posix_RequestSuspend(int index);
// Drops one suspend request. The thread continues once the count reaches zero.
void Posix_RequestResume(int index);
// Outstanding suspend requests, i.e. how many resumes the gate still needs.
int Posix_SuspendCount(int index);
bool Posix_IsSuspendRequested(int index);
// Blocks while the gate holds any suspend request, so a released gate returns
// immediately. Safe to call from any thread, and ignores unknown indices.
void Posix_SuspendCheckpoint(int index);
// Waits until the thread on this gate has reached a checkpoint with a request
// outstanding, i.e. it really has stopped. Returns false if it did not park
// within msec: a thread that never reaches a checkpoint cannot be forced to
// stop, so callers must not treat a timeout as a stop.
bool Posix_WaitForSuspendAck(int index, uint32_t msec);

// --- Recursive critical sections ------------------------------------------

void Posix_InitCriticalSections(int count);
void Posix_LockCriticalSection(int index);
void Posix_UnlockCriticalSection(int index);
// Recursion depth of the section on the calling thread.
int Posix_CriticalSectionDepth(int index);
// Total successful lock acquisitions, used to prove recursion actually nests.
uint32_t Posix_CriticalSectionAcquisitions();

// --- Physical memory arena -------------------------------------------------
//
// Reserves address space with PROT_NONE and commits it page by page, so nothing
// is backed until it is used and any access before commit faults.

bool Posix_ArenaReserve(uint32_t size, uint8_t **baseOut, uint32_t *pageSizeOut);
// Makes [offset, offset+length) writable. Returns false and leaves errno set on
// failure. committedBytes is advanced as pages are committed.
bool Posix_ArenaCommit(uint8_t *base, uint32_t arenaSize, uint32_t *committedBytes, uint32_t offset, uint32_t length);
void Posix_ArenaRelease(uint8_t *base, uint32_t size);

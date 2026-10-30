#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Native console.
//
// Replaces win32/win_syscon.cpp for the native Linux target. The engine talks to
// the console in two directions:
//
//   outbound  Conbuf_AppendText / Sys_Print   -> stdout
//   inbound   Sys_ConsoleInput                -> one complete line, or NULL
//
// Sys_ConsoleInput must never block: the frame loop polls it every frame.
// stdin is therefore switched to non-blocking and its original flags are
// restored on shutdown. End-of-file is latched so a closed or redirected stdin
// stops producing work instead of spinning.
// ---------------------------------------------------------------------------

void Sys_CreateConsole();
void Sys_DestroyConsole();
void Sys_ShowConsole();

// Returns the next complete input line, or NULL when nothing is pending. The
// returned buffer stays valid until the next call.
char *Sys_ConsoleInput();

void Sys_SetErrorText(const char *buf);

void Conbuf_AppendText(const char *msg);
void Conbuf_AppendTextInMainThread(const char *msg);

// Rewrites a string for terminal output: collapses CR/LF runs to "\n" and drops
// the engine's ^-digit colour codes. Returns the number of bytes written.
uint32_t Conbuf_CleanText(const char *source, char *target, int sizeofTarget);

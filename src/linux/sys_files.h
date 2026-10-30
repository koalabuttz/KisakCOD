#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Native filesystem services.
//
// Replaces the path/directory half of universal/win_common.cpp. Two rules are
// load-bearing:
//
//   * The engine addresses files with backslashes ("main\\iwd\\x.ff") because it
//     was written for Windows. Paths are normalised to '/' only at the point of
//     calling into POSIX, never rewritten in place, and filenames are never
//     case-folded -- Linux filesystems are case-sensitive and silently matching
//     "Foo" to "foo" would make content lookup depend on directory order.
//   * File and directory errors are reported through errno as the OS set it, so
//     callers can distinguish "missing" from "permission denied".
// ---------------------------------------------------------------------------

// Copies path into out (size bytes), rewriting '\\' to '/'. The engine can also
// hand us drive-letter paths; those are passed through unchanged so the failure
// is visible rather than silently reinterpreted.
void Posix_NormalizePath(const char *path, char *out, int outSize);

// Length-bounded normalisation writing into a caller buffer.
bool Posix_IsAbsolutePath(const char *path);

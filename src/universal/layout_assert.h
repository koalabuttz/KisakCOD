#pragma once

// Struct layout checks on more than one target (upstream issue #74).
//
// The retail game is 32-bit x86, and many engine structs mirror its layout byte
// for byte: fastfile assets, the script VM, game and client state. Most of them
// hold pointers (or other pointer-sized members), so their retail sizes and
// offsets are only true where pointers are 4 bytes.
//
//   static_assert(LAYOUT_32BIT(sizeof(XModel) == 220));
//
// LAYOUT_32BIT(cond) is `cond` on 32-bit targets, so those builds check exactly
// what they checked before, and true on 64-bit targets. Use it only for a check
// that fails on 64-bit targets because of pointer size. A layout that must be the
// same on every target (anything read from disk or the network without
// conversion, or a struct with no pointer-sized members) keeps a plain
// static_assert, so 64-bit builds still check it.
#define LAYOUT_32BIT(cond) (sizeof(void *) != 4 || (cond))

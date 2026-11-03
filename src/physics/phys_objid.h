#pragma once

#include <cstdint>

struct dxBody;

// A physics object id: a dxBody kept in a 32-bit field. DynEntityClient,
// cpose_t, FxElem, FxUpdateElem and BreakablePiece hold one in an int, and
// DynEntityClient's 12-byte layout is stored in fastfiles. On i386 the id is the
// pointer itself, as in the original game. A 64-bit pointer does not fit, so
// there the id is a slot in a table of live bodies that dBodyCreate fills and
// dBodyDestroy (or dWorldDestroy) empties. 0 is no body on both.
#if defined(__i386__) || defined(_M_IX86)
inline int32_t Phys_IdFromObj(dxBody *body)
{
    return (int32_t)body;
}

inline dxBody *Phys_ObjFromId(int32_t id)
{
    return (dxBody *)id;
}

inline void Phys_RegisterObj(dxBody *) {}
inline void Phys_UnregisterObj(dxBody *) {}
#else
int32_t Phys_IdFromObj(dxBody *body);
dxBody *Phys_ObjFromId(int32_t id);
void Phys_RegisterObj(dxBody *body);
void Phys_UnregisterObj(dxBody *body);
#endif

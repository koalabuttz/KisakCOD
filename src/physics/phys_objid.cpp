// Physics object ids for 64-bit builds; see phys_objid.h. i386 keeps the
// original pointer-in-an-int ids and compiles nothing here.

#include "phys_objid.h"

#if !(defined(__i386__) || defined(_M_IX86))

#include <mutex>

#include <universal/assertive.h>

// Calls Com_Error; defined next to dBodyCreate so this file stays engine-free.
void Phys_ObjIdTableFull(int slots);

namespace
{
// The original body pool held 512 bodies; this leaves room for more.
constexpr int PHYS_OBJ_ID_SLOTS = 2048;
dxBody *s_physObjects[PHYS_OBJ_ID_SLOTS];
int s_physObjectsNextSlot;
std::mutex s_physObjectsLock;
} // namespace

void Phys_RegisterObj(dxBody *body)
{
    iassert(body);
    {
        std::lock_guard<std::mutex> lock(s_physObjectsLock);
        for (int n = 0; n < PHYS_OBJ_ID_SLOTS; ++n)
        {
            const int slot = (s_physObjectsNextSlot + n) % PHYS_OBJ_ID_SLOTS;
            if (!s_physObjects[slot])
            {
                s_physObjects[slot] = body;
                s_physObjectsNextSlot = (slot + 1) % PHYS_OBJ_ID_SLOTS;
                return;
            }
        }
    }
    Phys_ObjIdTableFull(PHYS_OBJ_ID_SLOTS);
}

void Phys_UnregisterObj(dxBody *body)
{
    std::lock_guard<std::mutex> lock(s_physObjectsLock);
    for (dxBody *&slot : s_physObjects)
    {
        if (slot == body)
        {
            slot = nullptr;
            return;
        }
    }
}

int32_t Phys_IdFromObj(dxBody *body)
{
    if (!body)
        return 0;
    {
        std::lock_guard<std::mutex> lock(s_physObjectsLock);
        for (int slot = 0; slot < PHYS_OBJ_ID_SLOTS; ++slot)
        {
            if (s_physObjects[slot] == body)
                return slot + 1;
        }
    }
    iassert(!"physics body was never registered");
    return 0;
}

dxBody *Phys_ObjFromId(int32_t id)
{
    if (!id)
        return nullptr;
    iassert(id > 0 && id <= PHYS_OBJ_ID_SLOTS);
    if (id <= 0 || id > PHYS_OBJ_ID_SLOTS)
        return nullptr;
    std::lock_guard<std::mutex> lock(s_physObjectsLock);
    dxBody *body = s_physObjects[id - 1];
    iassert(body);
    return body;
}

#endif

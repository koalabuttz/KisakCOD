// phys_objid_test.cpp -- physics object ids (src/physics/phys_objid.cpp) built
// for i386 and x86_64. Asset-free: dxBody is never defined, the "bodies" are
// addresses in a local array, and nothing calls into ODE.

#include <physics/phys_objid.h>

#include <cstdio>
#include <cstdlib>

namespace
{
int g_failures;
int g_asserts;
int g_tableFull;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS:" : "FAIL:", what);
    if (!ok)
        ++g_failures;
}

alignas(16) unsigned char g_storage[4096 * 16];

dxBody *Body(int index)
{
    return reinterpret_cast<dxBody *>(&g_storage[index * 16]);
}
} // namespace

void MyAssertHandler(const char *, int, int, const char *, ...)
{
    ++g_asserts;
}

void Phys_ObjIdTableFull(int)
{
    ++g_tableFull;
}

int main()
{
    check(Phys_IdFromObj(nullptr) == 0, "no body is id 0");
    check(Phys_ObjFromId(0) == nullptr, "id 0 is no body");

    Phys_RegisterObj(Body(1));
    Phys_RegisterObj(Body(2));
    const int32_t one = Phys_IdFromObj(Body(1));
    const int32_t two = Phys_IdFromObj(Body(2));
    check(one != 0 && two != 0 && one != two, "two live bodies get distinct nonzero ids");
    check(Phys_ObjFromId(one) == Body(1) && Phys_ObjFromId(two) == Body(2), "ids map back to their bodies");
    check(Phys_IdFromObj(Body(1)) == one, "a body keeps its id");

#if defined(__i386__)
    check(one == (int32_t)Body(1), "i386 ids are the body pointers, as in the original game");
    check(g_asserts == 0, "no assert fired");
    std::printf("phys_objid_test: done\n");
    return g_failures ? 1 : 0;
#else
    Phys_UnregisterObj(Body(1));
    Phys_RegisterObj(Body(3));
    const int32_t three = Phys_IdFromObj(Body(3));
    check(three != one && Phys_ObjFromId(two) == Body(2), "a new body does not take a just-freed id, and others keep theirs");

    Phys_UnregisterObj(Body(2));
    Phys_UnregisterObj(Body(3));
    for (int i = 0; i < 2048; ++i)
        Phys_RegisterObj(Body(100 + i));
    check(g_tableFull == 0, "2048 bodies fit");
    bool distinct = true;
    for (int i = 0; i < 2048; ++i)
        distinct = distinct && Phys_ObjFromId(Phys_IdFromObj(Body(100 + i))) == Body(100 + i);
    check(distinct, "every one of 2048 bodies round-trips through its id");
    Phys_RegisterObj(Body(3000));
    check(g_tableFull == 1, "the 2049th body reports a full table");
    Phys_UnregisterObj(Body(500));
    Phys_RegisterObj(Body(3001));
    check(g_tableFull == 1 && Phys_ObjFromId(Phys_IdFromObj(Body(3001))) == Body(3001),
          "a freed slot is reused once the table is full");

    check(g_asserts == 0, "no assert fired on valid use");
    Phys_IdFromObj(Body(500));
    check(g_asserts == 1, "asking for the id of an unregistered body asserts");
    check(Phys_ObjFromId(5000) == nullptr && g_asserts == 2, "an out-of-range id asserts and gives no body");
    std::printf("phys_objid_test: done\n");
    return g_failures ? 1 : 0;
#endif
}

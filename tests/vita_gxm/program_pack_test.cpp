// Host tool for the PS Vita's pack of compiled programs
// (src/gfx_gxm/gxm_program_pack.h), run by
// scripts/tests/test_vita_program_pack.py on packs the packing script made
// and on files that are not packs.
//
//     program_pack_test list <pack>           "<key> <size>" a program, in
//                                             the index's order
//     program_pack_test get <pack> <key>      the program's bytes
//
// Exit code 2: no such file; 3: a file that is not a pack; 4: no such key.

#include <gfx_gxm/gxm_program_pack.h>

#include <cstdio>
#include <cstdlib>

using namespace gfx_gxm;

int main(int argc, char **argv)
{
    if (argc < 3)
        return 64;
    ProgramPack pack;
    if (!pack.Open(argv[2]))
        return pack.Refused() ? 3 : 2;
    if (!strcmp(argv[1], "list"))
    {
        for (const PackEntry &entry : pack.Entries())
            printf("%016llx %u\n", (unsigned long long)entry.key, entry.size);
        return 0;
    }
    if (!strcmp(argv[1], "get") && argc == 4)
    {
        std::vector<uint8_t> program;
        if (!pack.Read(strtoull(argv[3], nullptr, 16), program))
            return 4;
        fwrite(program.data(), 1, program.size(), stdout);
        return 0;
    }
    return 64;
}

/* Translate one D3D9 bytecode file to SPIR-V with MojoShader's "spirv" profile.
 * Usage: mojo_spv <in> <out.spv>. Exit 0 on success, 1 on translator error. */
#include <stdio.h>
#include <stdlib.h>
#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

int main(int argc, char **argv)
{
    FILE *f; long n; unsigned char *buf; const MOJOSHADER_parseData *pd; int i;
    if (argc != 3) return 2;
    f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(n); if (fread(buf, 1, n, f) != (size_t)n) return 2; fclose(f);
    pd = MOJOSHADER_parse("spirv", "main", buf, (unsigned)n, NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (pd->error_count) {
        for (i = 0; i < pd->error_count; i++)
            fprintf(stderr, "%s\n", pd->errors[i].error);
        return 1;
    }
    f = fopen(argv[2], "wb");
    fwrite(pd->output, 1, pd->output_len - sizeof(SpirvPatchTable), f);
    fclose(f);
    MOJOSHADER_freeParseData(pd);
    return 0;
}

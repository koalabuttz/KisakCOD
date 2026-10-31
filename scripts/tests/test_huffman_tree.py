#!/usr/bin/env python3
"""Pin the message Huffman tree to the one the Windows client builds.

The static message codec in ``src/qcommon/huffman.cpp`` builds its tree from
``msg_hData``, and both ends of a connection must agree on it bit for bit or
compressed messages decode into garbage. ``Huff_BuildFromData`` orders its pool
of nodes by weight alone, which leaves the relative order of equal-weight nodes
to the C library's ``qsort``. ``msg_hData`` has ties (bytes 155/205 and
228/231), and glibc and the Windows client's runtime order them differently, so
a Linux server used to assign bytes 155 and 205 each other's 9-bit codes and a
live client misread every message containing them.

This test compiles the production ``huffman.cpp`` against the production
``msg_hData`` table and asserts the whole symbol table against the tree the
shipped Windows client produces, then round-trips real bitstreams through the
production encoder and decoder.

The expected table below was obtained by loading the installed Windows 1.7
client executable into an emulator, calling its own tree build, and walking
parent pointers from each symbol node. A separate recovery from the examined
Windows 1.0 client produced the same 256 codes; target identities and limits are
recorded in docs/retail-client-compatibility.md. Only the resulting numbers are
recorded here; no bytes from either executable are reproduced. A mismatch means
the server's codec no longer agrees with those examined clients.

Usage:
    python3 scripts/tests/test_huffman_tree.py [--cxx clang++] [--timeout 180]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HUFFMAN = ROOT / "src" / "qcommon" / "huffman.cpp"
MSG_SOURCE = ROOT / "src" / "qcommon" / "msg_mp.cpp"

# (bit length, bits with the first transmitted bit as the most significant bit)
# for symbols 0..255, in the order the retail client produces them.
EXPECTED = """\
    { 3, 0x004}, { 5, 0x014}, { 6, 0x036}, { 6, 0x035},
    { 6, 0x03E}, { 6, 0x032}, { 5, 0x000}, { 6, 0x00E},
    { 7, 0x057}, { 7, 0x031}, { 7, 0x006}, { 8, 0x0E5},
    { 8, 0x0C2}, { 8, 0x0D2}, { 8, 0x0BB}, { 7, 0x02D},
    { 7, 0x011}, { 7, 0x024}, { 8, 0x065}, { 8, 0x04E},
    { 9, 0x1FD}, { 9, 0x1D7}, { 7, 0x009}, { 8, 0x04F},
    { 8, 0x054}, { 9, 0x1E0}, { 8, 0x0EF}, { 8, 0x00E},
    { 8, 0x069}, { 8, 0x016}, { 8, 0x060}, { 8, 0x0C6},
    { 6, 0x014}, { 9, 0x1BA}, { 7, 0x017}, { 9, 0x1FC},
    { 9, 0x187}, { 9, 0x168}, { 9, 0x1BD}, { 9, 0x188},
    { 9, 0x1C6}, { 9, 0x069}, { 9, 0x06B}, { 9, 0x08E},
    { 8, 0x03D}, { 9, 0x1C0}, { 9, 0x1A6}, { 9, 0x159},
    { 7, 0x020}, { 7, 0x018}, { 8, 0x0F5}, { 8, 0x0F4},
    { 8, 0x0DC}, { 8, 0x05C}, { 8, 0x04C}, { 8, 0x04D},
    { 8, 0x0BC}, { 8, 0x00A}, { 9, 0x1D9}, { 9, 0x174},
    { 9, 0x1A7}, { 9, 0x1E2}, { 8, 0x06C}, { 8, 0x0E1},
    { 7, 0x02F}, { 8, 0x0B0}, { 8, 0x0F3}, { 8, 0x04A},
    { 7, 0x00C}, { 8, 0x008}, { 9, 0x1A0}, { 9, 0x1EE},
    { 8, 0x02B}, { 9, 0x19D}, { 9, 0x1C7}, { 9, 0x07C},
    { 9, 0x154}, { 9, 0x1FF}, { 7, 0x021}, { 7, 0x03B},
    { 8, 0x01C}, { 9, 0x012}, { 9, 0x182}, { 9, 0x013},
    { 9, 0x0F8}, { 9, 0x0DC}, { 9, 0x0CD}, { 9, 0x05B},
    { 9, 0x1CF}, { 9, 0x0E9}, { 9, 0x03B}, { 9, 0x186},
    { 9, 0x16A}, { 9, 0x022}, { 9, 0x055}, { 8, 0x05D},
    { 8, 0x0E9}, { 8, 0x01A}, { 9, 0x1C5}, { 9, 0x169},
    { 8, 0x014}, { 9, 0x0F9}, { 8, 0x044}, { 9, 0x175},
    { 8, 0x00B}, { 9, 0x07D}, {10, 0x3A9}, { 9, 0x078},
    { 8, 0x0C0}, { 9, 0x1BE}, { 9, 0x19F}, { 9, 0x1E4},
    { 8, 0x057}, { 9, 0x0FB}, { 9, 0x1FE}, { 9, 0x1CE},
    { 9, 0x173}, {10, 0x3F0}, { 9, 0x1A2}, { 9, 0x0F5},
    { 8, 0x01F}, { 9, 0x0D7}, { 9, 0x0FF}, { 9, 0x165},
    { 9, 0x1FA}, { 9, 0x198}, { 8, 0x046}, { 8, 0x0A8},
    { 6, 0x009}, { 8, 0x0A9}, { 8, 0x0C7}, { 8, 0x0BF},
    { 8, 0x04B}, { 9, 0x058}, { 9, 0x164}, { 9, 0x17A},
    { 8, 0x0E8}, { 9, 0x0EA}, { 9, 0x059}, { 9, 0x0F6},
    { 8, 0x029}, { 9, 0x0AC}, { 8, 0x0ED}, { 9, 0x19A},
    { 9, 0x1ED}, { 9, 0x06D}, { 9, 0x0AD}, { 9, 0x08B},
    { 9, 0x16C}, {10, 0x3A8}, { 9, 0x03A}, { 9, 0x01E},
    { 9, 0x171}, { 9, 0x0DD}, { 9, 0x158}, { 9, 0x0D0},
    { 9, 0x1A3}, { 9, 0x02B}, { 9, 0x02A}, { 9, 0x1CC},
    { 8, 0x06D}, { 9, 0x051}, { 9, 0x0B2}, {10, 0x3BA},
    { 9, 0x0F7}, {10, 0x3B9}, { 9, 0x01F}, {10, 0x388},
    { 9, 0x0AB}, {10, 0x2F7}, { 9, 0x19E}, {10, 0x3F1},
    { 9, 0x08A}, { 9, 0x1D8}, { 9, 0x06A}, { 9, 0x05A},
    { 8, 0x0B7}, { 9, 0x183}, { 9, 0x068}, { 9, 0x040},
    { 9, 0x0AA}, { 9, 0x16B}, { 8, 0x037}, { 9, 0x189},
    { 9, 0x1BF}, { 9, 0x041}, { 9, 0x023}, { 9, 0x0FA},
    { 9, 0x1D6}, { 9, 0x1FB}, { 9, 0x1E1}, { 8, 0x01E},
    { 7, 0x019}, { 8, 0x079}, { 8, 0x078}, { 9, 0x1D5},
    { 8, 0x058}, { 9, 0x0E8}, { 9, 0x0D6}, { 9, 0x16D},
    { 8, 0x01B}, { 9, 0x07F}, {10, 0x3CB}, {10, 0x389},
    { 9, 0x1BC}, { 9, 0x0CF}, { 9, 0x0B3}, { 9, 0x170},
    { 9, 0x1F9}, { 9, 0x06C}, { 9, 0x15A}, { 9, 0x079},
    {10, 0x3CA}, {11, 0x5ED}, { 9, 0x0C3}, { 9, 0x050},
    { 8, 0x0AB}, { 9, 0x0D1}, {10, 0x3BB}, { 9, 0x0CC},
    { 9, 0x155}, { 9, 0x0CE}, { 9, 0x1E3}, { 8, 0x010},
    { 8, 0x0E4}, { 9, 0x15B}, { 9, 0x172}, { 9, 0x0FE},
    { 9, 0x17C}, {10, 0x3B8}, { 9, 0x054}, { 9, 0x17D},
    { 9, 0x1CD}, { 9, 0x0F4}, { 9, 0x0C2}, { 9, 0x07E},
    { 9, 0x19B}, { 9, 0x08F}, { 9, 0x1C1}, { 8, 0x021},
    { 8, 0x0B3}, { 9, 0x199}, { 9, 0x0EB}, { 9, 0x1BB},
    { 9, 0x1EF}, { 9, 0x19C}, { 9, 0x1A1}, { 8, 0x017},
    { 8, 0x06A}, { 9, 0x1EC}, { 8, 0x06F}, { 8, 0x064},
    { 8, 0x07E}, { 8, 0x0B1}, { 8, 0x0C5}, { 6, 0x01C},
"""

HARNESS = """\
// Generated by scripts/tests/test_huffman_tree.py -- not a build target.
// The tree build, the node helpers and the bit I/O all come from the
// production src/qcommon/huffman.cpp; only the table and the expectations
// below are the test's own.

#include <universal/q_shared.h>
#include "qcommon/huffman.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_checks;
static int g_failures;

static void Check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        printf("  FAIL  %s\\n", what);
    }
    else
    {
        printf("  ok    %s\\n", what);
    }
}

__TABLE__

// Huff_Init zeroes through Com_Memset and the bit counters assert through
// MyAssertHandler; both live in the engine proper, so they are stubbed here.
void Com_Memset(void *dest, int val, unsigned int count)
{
    memset(dest, val, count);
}

void MyAssertHandler(const char *file, int line, int type, const char *fmt, ...)
{
    va_list ap;

    (void)type;
    va_start(ap, fmt);
    fprintf(stderr, "assertion from production huffman.cpp %s:%d: ", file, line);
    vfprintf(stderr, fmt, ap);
    fputc('\\n', stderr);
    va_end(ap);
    abort();
}

struct Code
{
    int length;
    int bits;
};

static const struct Code kExpected[256] =
{
__EXPECTED__
};

// Read one leaf's code by walking parent pointers. huffman_send emits a 1 for a
// right child and a 0 for a left one, so the last step is the most significant
// bit of the number kept here.
static void CodeOf(const nodetype *node, int *outLength, int *outBits, const nodetype **outRoot)
{
    int steps[64];
    int count = 0;
    int value = 0;
    int i;

    while (node->parent)
    {
        steps[count++] = (node->parent->right == node) ? 1 : 0;
        node = node->parent;
    }
    for (i = count - 1; i >= 0; --i)
        value = (value << 1) | steps[i];

    *outLength = count;
    *outBits = value;
    *outRoot = node;
}

// Same packing add_bit performs: the first bit of a code goes to the low bit of
// the current byte.
static void PushBits(uint8_t *dst, int *bitPosition, int length, int bits)
{
    int i;

    for (i = length - 1; i >= 0; --i)
    {
        int position = *bitPosition;
        int bit = (bits >> i) & 1;

        if ((position & 7) == 0)
            dst[position >> 3] = 0;
        dst[position >> 3] |= (uint8_t)(bit << (position & 7));
        ++(*bitPosition);
    }
}

static unsigned int g_seed = 0x4D2u;

static unsigned int NextRandom(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7FFFu;
}

int main(void)
{
    static huffman_t huff;
    static huffman_t again;
    static uint8_t encoded[8192];
    static uint8_t wanted[8192];
    static uint8_t payload[512];
    char label[256];
    int ch;
    int lengthMismatches = 0;
    int bitsMismatches = 0;
    int countMismatches = 0;
    int totalBits = 0;
    int encodedBits = 0;
    int wantedBits = 0;
    int roundTrips = 0;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("message Huffman tree from src/qcommon/huffman.cpp\\n");
    printf("sizeof(void*)=%u  sizeof(nodetype)=%u\\n",
           (unsigned)sizeof(void *), (unsigned)sizeof(nodetype));

    memset(&huff, 0, sizeof(huff));
    Huff_Init(&huff);
    Huff_BuildFromData(&huff.compressDecompress, msg_hData);

    printf("\\n== tree shape ==\\n");
    snprintf(label, sizeof(label), "nodeList holds %d nodes (256 leaves + placeholder + 256 merges = 513)",
             huff.compressDecompress.blocNode);
    Check(huff.compressDecompress.blocNode == 513, label);

    printf("\\n== symbol codes against the retail client's tree ==\\n");
    for (ch = 0; ch < 256; ++ch)
    {
        const nodetype *root = 0;
        int length = 0;
        int bits = 0;
        nodetype *leaf = huff.compressDecompress.loc[ch];

        if (!leaf)
        {
            printf("  FAIL  symbol %3d has no node\\n", ch);
            ++lengthMismatches;
            continue;
        }
        CodeOf(leaf, &length, &bits, &root);
        if (root != huff.compressDecompress.tree)
        {
            printf("  FAIL  symbol %3d does not reach huff->tree\\n", ch);
            ++lengthMismatches;
            continue;
        }
        if (length != kExpected[ch].length)
        {
            ++lengthMismatches;
            printf("  FAIL  symbol %3d length %d, client uses %d\\n", ch, length, kExpected[ch].length);
        }
        else if (bits != kExpected[ch].bits)
        {
            ++bitsMismatches;
            printf("  FAIL  symbol %3d code 0x%X (%d bits), client uses 0x%X\\n",
                   ch, bits, length, kExpected[ch].bits);
        }
        if (Huff_bitCount(&huff.compressDecompress, (uint32_t)ch) != kExpected[ch].length)
        {
            ++countMismatches;
            printf("  FAIL  symbol %3d Huff_bitCount disagrees with the tree walk\\n", ch);
        }
        totalBits += kExpected[ch].length;
    }
    snprintf(label, sizeof(label),
             "all 256 symbols match the client (%d length, %d bit-pattern, %d bit-count mismatches)",
             lengthMismatches, bitsMismatches, countMismatches);
    Check(lengthMismatches == 0 && bitsMismatches == 0 && countMismatches == 0, label);
    snprintf(label, sizeof(label), "code lengths total %d bits (expected 2182)", totalBits);
    Check(totalBits == 2182, label);

    printf("\\n== the bits that go on the wire ==\\n");
    memset(encoded, 0, sizeof(encoded));
    memset(wanted, 0, sizeof(wanted));
    for (ch = 0; ch < 256; ++ch)
    {
        Huff_offsetTransmit(&huff.compressDecompress, ch, encoded, &encodedBits);
        PushBits(wanted, &wantedBits, kExpected[ch].length, kExpected[ch].bits);
    }
    snprintf(label, sizeof(label), "encoder wrote %d bits, client codes total %d", encodedBits, wantedBits);
    Check(encodedBits == wantedBits, label);
    snprintf(label, sizeof(label), "encoded stream is byte-identical to the client's codes over %d bytes",
             (wantedBits + 7) / 8);
    Check(memcmp(encoded, wanted, (size_t)((wantedBits + 7) / 8)) == 0, label);

    printf("\\n== decoder reads the client's codes ==\\n");
    {
        int position = 0;
        int value = -1;

        for (ch = 0; ch < 256; ++ch)
        {
            if (!Huff_offsetReceive(huff.compressDecompress.tree, &value, wanted, &position, wantedBits) ||
                value != ch)
            {
                printf("  FAIL  symbol %3d decoded as %d\\n", ch, value);
                break;
            }
        }
        snprintf(label, sizeof(label), "decoded %d of 256 symbols from the client's bitstream", ch);
        Check(ch == 256 && position == wantedBits, label);
    }

    printf("\\n== round trips through the production codec ==\\n");
    for (ch = 0; ch < 256; ++ch)
    {
        int position = 0;
        int readBack = 0;
        int value = -1;

        memset(encoded, 0, sizeof(encoded));
        Huff_offsetTransmit(&huff.compressDecompress, ch, encoded, &position);
        if (Huff_offsetReceive(huff.compressDecompress.tree, &value, encoded, &readBack, position) &&
            value == ch && readBack == position)
        {
            ++roundTrips;
        }
        else
        {
            printf("  FAIL  symbol %3d did not round trip\\n", ch);
        }
    }
    snprintf(label, sizeof(label), "%d of 256 byte values survive transmit then receive", roundTrips);
    Check(roundTrips == 256, label);

    // Symbols drawn from the same generator each time, so a failure reproduces.
    {
        int streamFailures = 0;

        for (i = 0; i < 24; ++i)
        {
            int symbols = 1 + (int)(NextRandom() % 400);
            int position = 0;
            int readBack = 0;
            int value = -1;
            int matched = 1;
            int n;

            memset(encoded, 0, sizeof(encoded));
            for (n = 0; n < symbols; ++n)
            {
                payload[n] = (uint8_t)(NextRandom() % 256);
                Huff_offsetTransmit(&huff.compressDecompress, payload[n], encoded, &position);
            }
            for (n = 0; n < symbols; ++n)
            {
                if (!Huff_offsetReceive(huff.compressDecompress.tree, &value, encoded, &readBack, position) ||
                    value != payload[n])
                {
                    matched = 0;
                    printf("  FAIL  random stream %d: symbol %d decoded as %d\\n", i, n, value);
                    break;
                }
            }
            if (matched && readBack != position)
            {
                printf("  FAIL  random stream %d: decoder consumed %d of %d bits\\n", i, readBack, position);
                matched = 0;
            }
            if (!matched)
                ++streamFailures;
        }
        Check(streamFailures == 0, "24 generated streams of up to 400 symbols decode back exactly");
    }

    printf("\\n== the build does not depend on the pool's prior state ==\\n");
    memset(&again, 0, sizeof(again));
    Huff_Init(&again);
    Huff_BuildFromData(&again.compressDecompress, msg_hData);
    {
        int differing = 0;

        for (ch = 0; ch < 256; ++ch)
        {
            const nodetype *rootA = 0;
            const nodetype *rootB = 0;
            int lengthA = 0;
            int bitsA = 0;
            int lengthB = 0;
            int bitsB = 0;

            CodeOf(huff.compressDecompress.loc[ch], &lengthA, &bitsA, &rootA);
            CodeOf(again.compressDecompress.loc[ch], &lengthB, &bitsB, &rootB);
            if (lengthA != lengthB || bitsA != bitsB)
            {
                printf("  FAIL  symbol %3d differs between two builds\\n", ch);
                ++differing;
            }
        }
        Check(differing == 0, "a second build produces the same 256 codes");
    }

    printf("\\n%d checks, %d failures\\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
"""

TABLE_PATTERN = re.compile(r"const int msg_hData\[256\] =\s*\{(.*?)\n\};", re.S)


def table_text() -> str:
    """The production table, lifted verbatim so the test cannot drift from it."""
    source = MSG_SOURCE.read_text(encoding="utf-8", errors="replace")
    match = TABLE_PATTERN.search(source)
    if not match:
        raise SystemExit(f"could not find msg_hData in {MSG_SOURCE}")
    return "const int msg_hData[256] = {" + match.group(1) + "\n};\n"


def build_harness() -> str:
    return HARNESS.replace("__TABLE__", table_text()).replace("__EXPECTED__", EXPECTED)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"),
                        help="C++ compiler (default: clang++, or $CXX)")
    parser.add_argument("--timeout", type=int, default=180,
                        help="hard timeout for the compiled harness, in seconds")
    args = parser.parse_args()

    for needed in (HUFFMAN, MSG_SOURCE):
        if not needed.is_file():
            print(f"FAIL: {needed} is missing", file=sys.stderr)
            return 2

    workdir = Path(tempfile.mkdtemp(prefix="kisakcod_huffman_"))
    try:
        harness = workdir / "huffman_tree_harness.cpp"
        harness.write_text(build_harness(), encoding="utf-8")

        binary = workdir / "huffman_tree_harness"
        compile_cmd = [
            args.cxx, "-m32", "-fms-extensions", "-pthread", "-std=gnu++20", "-O1", "-g",
            "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-I", str(ROOT / "src"),
            str(HUFFMAN), str(harness),
            "-o", str(binary),
        ]

        compiled = subprocess.run(compile_cmd, capture_output=True, text=True)
        if compiled.returncode != 0:
            print("FAIL: harness did not compile", file=sys.stderr)
            print(" ".join(compile_cmd), file=sys.stderr)
            print(compiled.stdout, file=sys.stderr)
            print(compiled.stderr, file=sys.stderr)
            return 2
        if compiled.stderr.strip():
            print(compiled.stderr.strip(), file=sys.stderr)

        try:
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"FAIL: harness did not finish within {args.timeout}s", file=sys.stderr)
            return 2

        sys.stdout.write(run.stdout)
        if run.stderr.strip():
            sys.stderr.write(run.stderr)
        return run.returncode
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

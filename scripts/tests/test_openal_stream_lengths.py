#!/usr/bin/env python3
"""Check the table that keeps the lengths of the MP3 streams that have played.

dr_mp3 reads a whole MP3 to find its length unless the file has a Xing or Info
tag, and the game's streams have none. On the PS Vita that read held the main
thread for 0.75 s each time a 3.5 MB stream started (console run AY). The OpenAL
driver keeps each file's count after its first play (g_streamLengths).

The table's two functions are extracted from src/sound/snd_driver_openal.cpp
and compiled with a small harness: a kept file is found again, case does not
matter (the game's file system ignores it), another size of the same name is
another file, a name too long to hold is not kept, and a full table drops the
oldest file, not the newest.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/sound/snd_driver_openal.cpp"


def extract(source: str, pattern: str, name: str) -> str:
    signature = re.search(pattern, source, re.MULTILINE)
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    tokens = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""", re.DOTALL)
    depth = 0
    for token in tokens.finditer(source, signature.end() - 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                end = token.end()
                if source.startswith(";", end):
                    end += 1
                return source[signature.start():end]
    raise ValueError(f"{name} body was not terminated")


def production_code() -> str:
    source = SOURCE.read_text(errors="replace")
    parts = [extract(source, r"^struct AlStreamLength\s*\{", "AlStreamLength")]
    for name in ("AL_STREAM_LENGTHS", "g_streamLengths", "g_streamLengthCount", "g_streamLengthNext"):
        line = re.search(rf"^static [^\n]*\b{name}\b[^\n]*;$", source, re.MULTILINE)
        if line is None:
            raise ValueError(f"{name} was not found")
        parts.append(line.group(0))
    parts.append(extract(source, r"^static bool AL_FindStreamLength\([^)]*\)\s*\{", "AL_FindStreamLength"))
    parts.append(extract(source, r"^static void AL_KeepStreamLength\([^)]*\)\s*\{", "AL_KeepStreamLength"))
    return "\n\n".join(parts)


HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

static int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static void I_strncpyz(char *to, const char *from, int size)
{
    strncpy(to, from, size - 1);
    to[size - 1] = 0;
}

@PRODUCTION@

static int failures;
static void Check(bool ok, const char *what)
{
    if (!ok)
    {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

int main()
{
    uint64_t frames = 0;
    Check(!AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames), "an empty table finds nothing");

    AL_KeepStreamLength("sound/ambient/a.mp3", 1000, 4410000);
    Check(AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames) && frames == 4410000, "a kept file is found");
    frames = 0;
    Check(AL_FindStreamLength("SOUND/Ambient/A.MP3", 1000, &frames) && frames == 4410000, "case does not matter");
    Check(!AL_FindStreamLength("sound/ambient/a.mp3", 1001, &frames), "another size is another file");
    Check(!AL_FindStreamLength("sound/ambient/b.mp3", 1000, &frames), "another name is another file");

    // A name that would be cut is not kept, so it cannot be taken for a
    // shorter one.
    const std::string longName(sizeof(g_streamLengths[0].name), 'x');
    const int before = g_streamLengthCount;
    AL_KeepStreamLength(longName.c_str(), 5, 7);
    Check(g_streamLengthCount == before, "a name too long is not kept");
    Check(!AL_FindStreamLength(longName.c_str(), 5, &frames), "a name too long is not found");
    const std::string fits(sizeof(g_streamLengths[0].name) - 1, 'y');
    AL_KeepStreamLength(fits.c_str(), 5, 7);
    Check(AL_FindStreamLength(fits.c_str(), 5, &frames) && frames == 7, "the longest name that fits is kept");

    // Fill the table: the first file kept is the first to go.
    char name[64];
    for (int i = 0; g_streamLengthCount < AL_STREAM_LENGTHS; ++i)
    {
        snprintf(name, sizeof(name), "sound/fill%d.mp3", i);
        AL_KeepStreamLength(name, 10 + i, 100 + i);
    }
    Check(AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames), "a full table still holds its first file");
    AL_KeepStreamLength("sound/new.mp3", 77, 88);
    Check(g_streamLengthCount == AL_STREAM_LENGTHS, "the table does not grow past its size");
    Check(!AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames), "the oldest file went");
    Check(AL_FindStreamLength("sound/new.mp3", 77, &frames) && frames == 88, "the newest file is there");
    Check(AL_FindStreamLength(fits.c_str(), 5, &frames) && frames == 7, "the second oldest file is still there");

    if (failures)
        return 1;
    printf("ok: 14 checks of the stream length table\n");
    return 0;
}
"""


def main() -> int:
    code = HARNESS.replace("@PRODUCTION@", production_code())
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / "stream_lengths.cpp"
        binary = Path(folder) / "stream_lengths"
        source.write_text(code)
        build = subprocess.run(
            ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", str(source), "-o", str(binary)],
            capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stdout + build.stderr)
            print("FAIL: the harness did not compile")
            return 1
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        print(run.stdout + run.stderr, end="")
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())

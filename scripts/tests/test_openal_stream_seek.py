#!/usr/bin/env python3
"""Check that OpenAL sound streams seek where dr_wav and dr_mp3 ask, in loose files.

dr_wav and dr_mp3 pass C-library seek origins (start, current, end). The engine's
FS_Seek takes its own (0 from here, 1 from the end, 2 from the start) for every
handle: its zip branch reads them that way, and for a loose file it hands them to
FileWrapper_Seek (src/qcommon/com_fileaccess.cpp), which maps them to fseek. The
stream callbacks in src/sound/snd_driver_openal.cpp convert with AL_FsSeekOrigin.

This extracts AL_FsSeekOrigin and FileWrapper_Seek from the production sources,
compiles them with a small harness, and checks that a seek through both lands where
the same fseek lands, for every origin, on a temporary file. It also checks that
the stream callbacks convert for every handle, not only packed ones. No game data.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "src/sound/snd_driver_openal.cpp"
FILE_ACCESS = ROOT / "src/qcommon/com_fileaccess.cpp"


def extract(source: str, pattern: str, name: str) -> str:
    signature = re.search(pattern, source, re.MULTILINE)
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    depth = 0
    for index in range(signature.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start():index + 1]
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <cassert>
#include <cstdio>
#include <cstdlib>
#define iassert(expression) assert(expression)
static const int alwaysfails = 0;
static void MyAssertHandler(const char *, int, int, const char *) { std::abort(); }
static const char *va(const char *format, ...) { return format; }

%(production)s

int main()
{
    char path[] = "/tmp/kisak-seek-XXXXXX";
    FILE *file = fdopen(mkstemp(path), "w+b");
    for (int i = 0; i < 100; ++i)
        std::fputc(i, file);
    int failures = 0;
    // Each case: start position, offset, C-library origin.
    const int cases[][3] = {{50, 10, SEEK_SET}, {50, 10, SEEK_CUR}, {50, -10, SEEK_CUR},
                            {50, -10, SEEK_END}, {0, 0, SEEK_END}, {70, 0, SEEK_SET}};
    for (const auto &c : cases)
    {
        std::fseek(file, c[0], SEEK_SET);
        std::fseek(file, c[1], c[2]);
        const long expected = std::ftell(file);
        std::fseek(file, c[0], SEEK_SET);
        FileWrapper_Seek(file, c[1], AL_FsSeekOrigin(c[2]));
        const long got = std::ftell(file);
        std::printf("%%s from %%d, offset %%d, origin %%d: at %%ld, want %%ld\n",
                    got == expected ? "ok  " : "FAIL", c[0], c[1], c[2], got, expected);
        failures += got != expected;
    }
    std::fclose(file);
    std::remove(path);
    return failures ? 1 : 0;
}
"""


def main() -> int:
    driver = DRIVER.read_text(errors="replace")
    access = FILE_ACCESS.read_text(errors="replace")
    production = "\n\n".join([
        extract(access, r"^int FileWrapper_Seek\(FILE \*h, int offset, int origin\)\s*\{", "FileWrapper_Seek"),
        extract(driver, r"^static int AL_FsSeekOrigin\(int trueOrigin\)\s*\{", "AL_FsSeekOrigin"),
    ])
    calls = re.findall(r"FS_Seek\(stream->fsHandle, offset, ([^;]*)\);", driver)
    failures = 0
    if len(calls) != 2 or any(call != "AL_FsSeekOrigin((int)origin)" for call in calls):
        print(f"FAIL: the dr_wav and dr_mp3 seek callbacks must convert every origin with AL_FsSeekOrigin: {calls}")
        failures += 1
    else:
        print("ok   the dr_wav and dr_mp3 seek callbacks convert every origin")
    with tempfile.TemporaryDirectory(prefix="kisak-stream-seek-") as workdir:
        source = Path(workdir) / "seek.cpp"
        binary = Path(workdir) / "seek"
        source.write_text(HARNESS % {"production": production})
        build = ["c++", "-std=c++17", "-Wall", "-Wno-unused-function", str(source), "-o", str(binary)]
        if subprocess.run(build, check=False).returncode != 0:
            print("FAIL: the seek harness did not build")
            return 1
        failures += subprocess.run([str(binary)], check=False).returncode != 0
    if failures:
        print("FAIL: OpenAL stream seeks")
        return 1
    print("PASS: OpenAL stream seeks land where dr_wav and dr_mp3 ask")
    return 0


if __name__ == "__main__":
    sys.exit(main())

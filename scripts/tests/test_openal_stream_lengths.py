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

Console run BB showed the other two thirds of the stall: with a tell callback
dr_mp3 looks for a tag at the end of the file when it opens it, and in a
deflated .iwd entry a place is only reached by inflating up to it, so the open
read the file twice. A second harness opens a made-up MP3 (silent frames,
nothing from the game) through dr_mp3 itself over a file that works like such
an entry, and counts the bytes: the open the driver makes reads the file's
head only, and the open with a tell callback is shown to read it twice, so the
check fails if dr_mp3 or the driver's call changes.
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


OPEN_HARNESS = r"""
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// A file that works like a deflated .iwd entry does under FS_Seek: a place is
// reached by reading up to it, and a place behind by starting over.
struct Entry
{
    std::vector<uint8_t> bytes;
    size_t at = 0;
    uint64_t passed = 0; // bytes read or skipped over
};

static size_t Read(void *user, void *out, size_t count)
{
    Entry *entry = (Entry *)user;
    const size_t left = entry->bytes.size() - entry->at;
    if (count > left)
        count = left;
    if (out)
        memcpy(out, entry->bytes.data() + entry->at, count);
    entry->at += count;
    entry->passed += count;
    return count;
}

static drmp3_bool32 Seek(void *user, int offset, drmp3_seek_origin origin)
{
    Entry *entry = (Entry *)user;
    int64_t target = offset;
    if (origin == DRMP3_SEEK_CUR)
        target += (int64_t)entry->at;
    else if (origin == DRMP3_SEEK_END)
        target += (int64_t)entry->bytes.size();
    if (target < 0 || target > (int64_t)entry->bytes.size())
        return DRMP3_FALSE;
    if ((size_t)target < entry->at)
        entry->at = 0; // open the entry again
    Read(user, nullptr, (size_t)target - entry->at);
    return DRMP3_TRUE;
}

static drmp3_bool32 Tell(void *user, drmp3_int64 *cursor)
{
    *cursor = (drmp3_int64)((Entry *)user)->at;
    return DRMP3_TRUE;
}

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
    // 2000 silent frames: MPEG-1 layer 3, 128 kbit/s, 44.1 kHz, no padding
    // (417 bytes, 1152 samples each). A frame of zeros after its header is
    // one with nothing in it.
    const size_t frameBytes = 417, frames = 2000;
    Entry made;
    made.bytes.assign(frameBytes * frames, 0);
    for (size_t i = 0; i < frames; ++i)
    {
        uint8_t *frame = made.bytes.data() + i * frameBytes;
        frame[0] = 0xFF; frame[1] = 0xFB; frame[2] = 0x90; frame[3] = 0x00;
    }
    const uint64_t size = made.bytes.size();

    // The driver's open: no tell callback.
    {
        Entry entry = made;
        drmp3 mp3;
        Check(drmp3_init(&mp3, Read, Seek, @TELL@, NULL, &entry, NULL), "the made-up stream opens");
        Check(mp3.sampleRate == 44100 && mp3.channels == 2, "the made-up stream is 44.1 kHz stereo");
        Check(entry.passed < size / 8, "opening a stream reads its head, not the file");
        const uint64_t opened = entry.passed;
        Check(drmp3_get_pcm_frame_count(&mp3) == frames * 1152, "a read through the file counts its frames");
        Check(entry.passed - opened <= size + size / 8, "the count reads the file once");
        drmp3_int16 pcm[2 * 1152];
        Check(drmp3_read_pcm_frames_s16(&mp3, 1152, pcm) == 1152, "the stream plays from its start after the count");
        drmp3_uninit(&mp3);
    }

    // With a tell callback dr_mp3 looks for a tag at the end first: two reads
    // of the whole file here. If this stops holding, dr_mp3 changed and the
    // driver's reason to leave the callback out needs another look.
    {
        Entry entry = made;
        drmp3 mp3;
        Check(drmp3_init(&mp3, Read, Seek, Tell, NULL, &entry, NULL), "the made-up stream opens with a tell callback");
        Check(entry.passed >= 2 * size - 256, "with a tell callback the open reads the file twice");
        drmp3_uninit(&mp3);
    }

    if (failures)
        return 1;
    printf("ok: 8 checks of what opening a stream reads\n");
    return 0;
}
"""


def driver_tell_argument() -> str:
    """The tell callback the driver hands drmp3_init for a stream, as written."""
    source = SOURCE.read_text(errors="replace")
    call = re.search(r"drmp3_init\(&stream->mp3,\s*AL_StreamReadCallback,\s*AL_Mp3SeekCallback,\s*([A-Za-z_0-9]+)\s*,",
                     source)
    if call is None:
        raise ValueError("the driver's drmp3_init call for a stream was not found")
    return call.group(1)


def build_and_run(folder: Path, name: str, code: str, extra: list) -> int:
    source = folder / f"{name}.cpp"
    binary = folder / name
    source.write_text(code)
    build = subprocess.run(
        ["g++", "-std=c++20", "-O1", *extra, str(source), "-o", str(binary)],
        capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout + build.stderr)
        print(f"FAIL: the {name} harness did not compile")
        return 1
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    print(run.stdout + run.stderr, end="")
    return run.returncode


def main() -> int:
    tell = driver_tell_argument()
    if tell != "NULL":
        print(f"FAIL: the driver opens an MP3 stream with the tell callback {tell};"
              " dr_mp3 then reads a deflated stream twice at every start")
        return 1
    with tempfile.TemporaryDirectory() as folder:
        result = build_and_run(Path(folder), "stream_lengths", HARNESS.replace("@PRODUCTION@", production_code()),
                               ["-Wall", "-Wextra", "-Werror"])
        # dr_mp3 is not ours: no -Werror for it.
        opened = build_and_run(Path(folder), "stream_open", OPEN_HARNESS.replace("@TELL@", tell),
                               ["-I", str(ROOT / "deps")])
        return result or opened


if __name__ == "__main__":
    sys.exit(main())

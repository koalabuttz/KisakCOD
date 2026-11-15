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

    python3 scripts/tests/test_openal_stream_lengths.py
    python3 scripts/tests/test_openal_stream_lengths.py --survey <folder of .mp3 files>

Console run BB showed the other two thirds of the stall: with a tell callback
dr_mp3 looks for a tag at the end of the file when it opens it, and in a
deflated .iwd entry a place is only reached by inflating up to it, so the open
read the file twice. A second harness opens a made-up MP3 (silent frames,
nothing from the game) through dr_mp3 itself over a file that works like such
an entry, and counts the bytes: the open the driver makes reads the file's
head only, and the open with a tell callback is shown to read it twice, so the
check fails if dr_mp3 or the driver's call changes.

The first play of a file no longer reads it through either when its head says
constant bit rate and its size is a whole number of such frames: the size
gives the length (AL_Mp3ConstantRateHead, AL_Mp3FramesFromSize, extracted
here too). The same harness makes up constant
bit rate files of several rates, with and without tags, and holds the size's
answer against dr_mp3's own count of the same bytes; files that are not
constant bit rate must be turned down.
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
    parts.append(extract(source, r"^static bool AL_CountStreamLength\([^)]*\)\s*\{", "AL_CountStreamLength"))
    parts.append(extract(source, r"^static void AL_KeepStreamLength\([^)]*\)\s*\{", "AL_KeepStreamLength"))
    return "\n\n".join(parts)


def size_code() -> str:
    source = SOURCE.read_text(errors="replace")
    parts = [extract(source, r"^struct AlMp3Head\s*\{", "AlMp3Head")]
    for name in ("AL_MP3_HEAD_BYTES", "AL_MP3_HEAD_MIN_FRAMES"):
        line = re.search(rf"^static [^\n]*\b{name}\b[^\n]*;$", source, re.MULTILINE)
        if line is None:
            raise ValueError(f"{name} was not found")
        parts.append(line.group(0))
    parts.append(extract(source, r"^static uint32_t AL_Mp3FrameBytes\([^)]*\)\s*\{", "AL_Mp3FrameBytes"))
    parts.append(extract(source, r"^static bool AL_Mp3ConstantRateHead\([^)]*\)\s*\{", "AL_Mp3ConstantRateHead"))
    parts.append(extract(source, r"^static bool AL_Mp3FramesFromSize\([^)]*\)\s*\{", "AL_Mp3FramesFromSize"))
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

    AL_KeepStreamLength("sound/ambient/a.mp3", 1000, 4410000, false);
    Check(AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames) && frames == 4410000, "a kept file is found");
    frames = 0;
    Check(AL_FindStreamLength("SOUND/Ambient/A.MP3", 1000, &frames) && frames == 4410000, "case does not matter");
    Check(!AL_FindStreamLength("sound/ambient/a.mp3", 1001, &frames), "another size is another file");
    Check(!AL_FindStreamLength("sound/ambient/b.mp3", 1000, &frames), "another name is another file");

    // A name that would be cut is not kept, so it cannot be taken for a
    // shorter one.
    const std::string longName(sizeof(g_streamLengths[0].name), 'x');
    const int before = g_streamLengthCount;
    AL_KeepStreamLength(longName.c_str(), 5, 7, false);
    Check(g_streamLengthCount == before, "a name too long is not kept");
    Check(!AL_FindStreamLength(longName.c_str(), 5, &frames), "a name too long is not found");
    const std::string fits(sizeof(g_streamLengths[0].name) - 1, 'y');
    AL_KeepStreamLength(fits.c_str(), 5, 7, false);
    Check(AL_FindStreamLength(fits.c_str(), 5, &frames) && frames == 7, "the longest name that fits is kept");

    // Fill the table: the first file kept is the first to go.
    char name[64];
    for (int i = 0; g_streamLengthCount < AL_STREAM_LENGTHS; ++i)
    {
        snprintf(name, sizeof(name), "sound/fill%d.mp3", i);
        AL_KeepStreamLength(name, 10 + i, 100 + i, false);
    }
    Check(AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames), "a full table still holds its first file");
    AL_KeepStreamLength("sound/new.mp3", 77, 88, false);
    Check(g_streamLengthCount == AL_STREAM_LENGTHS, "the table does not grow past its size");
    Check(!AL_FindStreamLength("sound/ambient/a.mp3", 1000, &frames), "the oldest file went");
    Check(AL_FindStreamLength("sound/new.mp3", 77, &frames) && frames == 88, "the newest file is there");
    Check(AL_FindStreamLength(fits.c_str(), 5, &frames) && frames == 7, "the second oldest file is still there");

    // A length that came from a size gives way to the count of a full play,
    // once. A counted length does not.
    uint64_t was = 0;
    AL_KeepStreamLength("sound/music/sized.mp3", 2000, 5000, true);
    Check(!AL_CountStreamLength("sound/music/sized.mp3", 2000, 0, &was), "a play of no frames counts nothing");
    Check(!AL_CountStreamLength("sound/music/sized.mp3", 2001, 4000, &was), "a file of another size is not counted");
    Check(AL_CountStreamLength("SOUND/music/sized.mp3", 2000, 3848, &was) && was == 5000, "a full play replaces a length from a size");
    Check(AL_FindStreamLength("sound/music/sized.mp3", 2000, &frames) && frames == 3848, "the count is what is found after");
    Check(!AL_CountStreamLength("sound/music/sized.mp3", 2000, 3000, &was), "a counted length is not replaced again");
    Check(AL_FindStreamLength("sound/music/sized.mp3", 2000, &frames) && frames == 3848, "and stays as counted");
    AL_KeepStreamLength("sound/music/right.mp3", 2000, 5000, true);
    Check(!AL_CountStreamLength("sound/music/right.mp3", 2000, 5000, &was), "a size that was right changes nothing");
    Check(!AL_CountStreamLength("sound/music/right.mp3", 2000, 4000, &was), "and the file then counts as counted");
    Check(!AL_CountStreamLength("sound/new.mp3", 77, 99, &was), "a length counted by the decoder is left alone");
    Check(AL_FindStreamLength("sound/new.mp3", 77, &frames) && frames == 88, "and is still what it was");
    Check(!AL_CountStreamLength("sound/unknown.mp3", 1, 99, &was), "a file not in the table is not counted");

    if (failures)
        return 1;
    printf("ok: 25 checks of the stream length table\n");
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

@SIZE@

// A made-up constant bit rate file: `frames` silent layer 3 frames with the
// header bytes given, padded the way an encoder pads (a frame takes the extra
// byte when the running total falls a byte behind), after `before` bytes and
// with `after` bytes behind the last frame.
static std::vector<uint8_t> MadeUp(uint8_t h1, uint8_t h2, uint32_t frames, const std::vector<uint8_t> &before,
                                   size_t after)
{
    const uint8_t plain[4] = {0xFF, h1, h2, 0x00};
    uint32_t rate, samples, bits;
    const uint32_t unpadded = AL_Mp3FrameBytes(plain, &rate, &samples, &bits);
    const uint64_t numerator = (uint64_t)(samples / 8) * bits; // bytes a frame, times the sample rate
    std::vector<uint8_t> file = before;
    uint64_t written = 0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint64_t due = (uint64_t)(i + 1) * numerator / rate;
        const bool padded = due - written > unpadded;
        const size_t bytes = unpadded + (padded ? 1 : 0);
        const size_t at = file.size();
        file.resize(at + bytes, 0);
        file[at] = 0xFF; file[at + 1] = h1; file[at + 2] = (uint8_t)(h2 | (padded ? 2 : 0)); file[at + 3] = 0x00;
        written += bytes;
    }
    file.resize(file.size() + after, 0x20);
    return file;
}

static uint64_t CountedByDrMp3(const std::vector<uint8_t> &file)
{
    Entry entry;
    entry.bytes = file;
    drmp3 mp3;
    if (!drmp3_init(&mp3, Read, Seek, NULL, NULL, &entry, NULL))
        return 0;
    const uint64_t frames = drmp3_get_pcm_frame_count(&mp3);
    drmp3_uninit(&mp3);
    return frames;
}

// The driver's way: the head decides, the size answers.
static bool FromSize(const std::vector<uint8_t> &file, uint64_t *frames)
{
    AlMp3Head head;
    const uint32_t headBytes = file.size() < AL_MP3_HEAD_BYTES ? (uint32_t)file.size() : AL_MP3_HEAD_BYTES;
    return AL_Mp3ConstantRateHead(file.data(), headBytes, &head)
           && AL_Mp3FramesFromSize(head, (uint32_t)file.size(), frames);
}

static std::vector<uint8_t> Id3v2(uint32_t body)
{
    std::vector<uint8_t> tag(10 + body, 0);
    tag[0] = 'I'; tag[1] = 'D'; tag[2] = '3'; tag[3] = 3;
    tag[6] = (uint8_t)((body >> 21) & 0x7F); tag[7] = (uint8_t)((body >> 14) & 0x7F);
    tag[8] = (uint8_t)((body >> 7) & 0x7F); tag[9] = (uint8_t)(body & 0x7F);
    return tag;
}

static int sizeChecks;
static void CheckSizes()
{
    struct Kind { const char *name; uint8_t h1, h2; uint32_t samples; };
    const Kind kinds[] = {
        {"MPEG-1 128 kbit/s 44.1 kHz", 0xFB, 0x90, 1152},
        {"MPEG-1 192 kbit/s 48 kHz", 0xFB, 0xB4, 1152},
        {"MPEG-1 96 kbit/s 32 kHz", 0xFB, 0x78, 1152},
        {"MPEG-2 64 kbit/s 22.05 kHz", 0xF3, 0x80, 576},
    };
    const uint32_t counts[] = {16, 999, 3001};
    char what[160];
    for (const Kind &kind : kinds)
    {
        for (uint32_t frames : counts)
        {
            // Bare, and behind the two ID3v2 tag sizes the game's files have.
            const std::vector<uint8_t> files[] = {
                MadeUp(kind.h1, kind.h2, frames, {}, 0),
                MadeUp(kind.h1, kind.h2, frames, Id3v2(1014), 0),
                MadeUp(kind.h1, kind.h2, frames, Id3v2(17910), 0),
            };
            for (const std::vector<uint8_t> &file : files)
            {
                uint64_t bySize = 0;
                const bool constant = FromSize(file, &bySize);
                const uint64_t counted = CountedByDrMp3(file);
                snprintf(what, sizeof(what), "%s, %u frames: the size says %llu, dr_mp3 counts %llu", kind.name,
                         (unsigned)frames, (unsigned long long)bySize, (unsigned long long)counted);
                Check(constant && bySize == counted && counted == (uint64_t)frames * kind.samples, what);
                ++sizeChecks;
            }

            // With an ID3v1 tag's 128 bytes at the end. The decoder, which no
            // longer looks for that tag (see the open below), cannot confirm
            // the last frame against what follows it and leaves it out. The
            // size counts it. A file that fits in the head shows its tail
            // there and is read through.
            const std::vector<uint8_t> tagged = MadeUp(kind.h1, kind.h2, frames, {}, 128);
            uint64_t bySize = 0;
            const bool constant = FromSize(tagged, &bySize);
            const uint64_t counted = CountedByDrMp3(tagged);
            snprintf(what, sizeof(what), "%s, %u frames and a tag at the end: the size says %llu, dr_mp3 counts %llu",
                     kind.name, (unsigned)frames, (unsigned long long)bySize, (unsigned long long)counted);
            Check(counted == (uint64_t)(frames - 1) * kind.samples
                      && (tagged.size() <= AL_MP3_HEAD_BYTES ? !constant : constant && bySize == counted + kind.samples),
                  what);
            ++sizeChecks;

            // Both tags, as every one of the game's music files has them.
            const std::vector<uint8_t> both = MadeUp(kind.h1, kind.h2, frames, Id3v2(1014), 128);
            uint64_t bothBySize = 0;
            const bool bothConstant = FromSize(both, &bothBySize);
            snprintf(what, sizeof(what), "%s, %u frames between two tags: the size says %llu", kind.name,
                     (unsigned)frames, (unsigned long long)bothBySize);
            Check(both.size() <= AL_MP3_HEAD_BYTES ? !bothConstant
                                                    : bothConstant && bothBySize == (uint64_t)frames * kind.samples,
                  what);
            ++sizeChecks;
        }
    }

    uint64_t frames = 0;
    Check(!FromSize(MadeUp(0xFB, 0x90, 15, {}, 0), &frames), "fewer frames than the head must show: read through");
    Check(!FromSize(MadeUp(0xFB, 0x90, 400, Id3v2(40000), 0), &frames), "a tag longer than the head: read through");
    Check(!FromSize(MadeUp(0xFB, 0x90, 400, std::vector<uint8_t>(7, 0), 0), &frames),
          "bytes before the first frame that are no tag: read through");

    // Two bit rates in turn, as a variable bit rate file has.
    std::vector<uint8_t> varied;
    for (int i = 0; i < 200; ++i)
    {
        const std::vector<uint8_t> one = MadeUp(0xFB, (i & 1) ? 0x90 : 0xB0, 1, {}, 0);
        varied.insert(varied.end(), one.begin(), one.end());
    }
    Check(!FromSize(varied, &frames), "frames of two bit rates: read through");
    Check(CountedByDrMp3(varied) == 200u * 1152u, "dr_mp3 still counts the two-rate file");

    // One bit rate through the head and another after it: the head cannot
    // see that, but the size no longer fits a whole number of frames. (The
    // game has six such files.)
    std::vector<uint8_t> late = MadeUp(0xFB, 0x90, 100, {}, 0); // 41.8 KB: past the head
    const std::vector<uint8_t> tail = MadeUp(0xFB, 0xA0, 99, {}, 0);
    late.insert(late.end(), tail.begin(), tail.end());
    Check(!FromSize(late, &frames), "a rate change after the head: the size does not fit, read through");
    // Bytes that are neither frames nor a 128 byte tag behind the last frame.
    Check(!FromSize(MadeUp(0xFB, 0x90, 400, {}, 57), &frames), "a tail that is no tag: read through");
    Check(!FromSize(MadeUp(0xFB, 0x90, 400, {}, 200), &frames), "a longer tail that is no tag: read through");
    // What stays unseen: a later stretch of another rate that happens to be
    // a whole number of the head's frames long, to the byte (98 frames at
    // 128 kbit/s are 40960 bytes, 98 at 192 are 61440: 245 frames at 128).
    // The check is here so that this stays a known limit.
    std::vector<uint8_t> unseen = MadeUp(0xFB, 0x90, 98, {}, 0);
    const std::vector<uint8_t> fitting = MadeUp(0xFB, 0xB0, 98, {}, 0);
    unseen.insert(unseen.end(), fitting.begin(), fitting.end());
    Check(FromSize(unseen, &frames) && frames != CountedByDrMp3(unseen),
          "a rate change after the head whose bytes fit whole frames is not seen (known limit)");
    sizeChecks += 9;
}

// --survey: what the driver would do with real files, next to dr_mp3's count.
static int Survey(int count, char **paths)
{
    int constantFiles = 0, exact = 0, oneFrameOff = 0, further = 0, endTags = 0, headOnly = 0;
    for (int i = 0; i < count; ++i)
    {
        FILE *in = fopen(paths[i], "rb");
        if (!in)
        {
            printf("cannot read %s\n", paths[i]);
            continue;
        }
        std::vector<uint8_t> file;
        uint8_t chunk[65536];
        for (size_t got; (got = fread(chunk, 1, sizeof(chunk), in)) > 0;)
            file.insert(file.end(), chunk, chunk + got);
        fclose(in);
        const bool endTag = file.size() > 128 && !memcmp(file.data() + file.size() - 128, "TAG", 3);
        endTags += endTag;
        uint64_t bySize = 0;
        const bool constant = FromSize(file, &bySize);
        const uint64_t counted = CountedByDrMp3(file);
        if (!constant)
        {
            AlMp3Head head;
            const uint32_t headBytes = file.size() < AL_MP3_HEAD_BYTES ? (uint32_t)file.size() : AL_MP3_HEAD_BYTES;
            const bool headConstant = AL_Mp3ConstantRateHead(file.data(), headBytes, &head);
            headOnly += headConstant;
            printf("%s: %zu bytes, %s, read through: %llu frames%s", paths[i], file.size(),
                   headConstant ? "constant bit rate in its head but its size does not fit" : "not constant bit rate in its head",
                   (unsigned long long)counted, endTag ? ", tag at the end" : "");
            if (headConstant)
                printf(", %u bit/s from byte %u", (unsigned)head.bitsPerSecond, (unsigned)head.audioStart);
            printf("\n");
            continue;
        }
        ++constantFiles;
        const long long off = (long long)bySize - (long long)counted;
        if (off == 0)
            ++exact;
        else if (off == 1152 || off == -1152 || off == 576 || off == -576)
            ++oneFrameOff;
        else
            ++further;
        printf("%s: %zu bytes, constant bit rate, by size %llu frames, counted %llu, difference %lld%s\n", paths[i],
               file.size(), (unsigned long long)bySize, (unsigned long long)counted, off, endTag ? ", tag at the end" : "");
    }
    printf("survey: %d files, %d taken as constant bit rate (head and size); of those %d exact, %d one MP3 frame off,"
           " %d further off; %d more are constant in their head only and are read through;"
           " %d files have an ID3v1 tag at the end\n",
           count, constantFiles, exact, oneFrameOff, further, headOnly, endTags);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return Survey(argc - 1, argv + 1);
    CheckSizes();

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
    printf("ok: 8 checks of what opening a stream reads, %d of a length from the size\n", sizeChecks);
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


def build_and_run(folder: Path, name: str, code: str, extra: list, arguments: list = ()) -> int:
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
    run = subprocess.run([str(binary), *arguments], capture_output=True, text=True)
    print(run.stdout + run.stderr, end="")
    return run.returncode


def main() -> int:
    tell = driver_tell_argument()
    # --survey <folder>: instead of the checks, say for every .mp3 under the
    # folder what the driver would do with it and what dr_mp3 counts. For a
    # machine that has the game's files; they are read, nothing is written.
    if len(sys.argv) == 3 and sys.argv[1] == "--survey":
        files = sorted(str(path) for path in Path(sys.argv[2]).rglob("*") if path.suffix.lower() == ".mp3")
        if not files:
            print(f"no .mp3 under {sys.argv[2]}")
            return 1
        with tempfile.TemporaryDirectory() as folder:
            return build_and_run(Path(folder), "stream_survey",
                                 OPEN_HARNESS.replace("@TELL@", tell).replace("@SIZE@", size_code()),
                                 ["-I", str(ROOT / "deps")], files)
    if tell != "NULL":
        print(f"FAIL: the driver opens an MP3 stream with the tell callback {tell};"
              " dr_mp3 then reads a deflated stream twice at every start")
        return 1
    with tempfile.TemporaryDirectory() as folder:
        result = build_and_run(Path(folder), "stream_lengths", HARNESS.replace("@PRODUCTION@", production_code()),
                               ["-Wall", "-Wextra", "-Werror"])
        # dr_mp3 is not ours: no -Werror for it.
        opened = build_and_run(Path(folder), "stream_open",
                               OPEN_HARNESS.replace("@TELL@", tell).replace("@SIZE@", size_code()),
                               ["-I", str(ROOT / "deps")])
        return result or opened


if __name__ == "__main__":
    sys.exit(main())

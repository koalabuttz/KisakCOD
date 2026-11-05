# Cinematics on Linux (Bink shim)

The game's movies (`main/video/*.bik`) are Bink 1 files. The engine plays them
with RAD's Bink library, which exists only for Windows (`deps/binklib`). No
build played them before this: `r_cinematic.cpp` is built without `CINEMA` on
every platform, Windows included.

On Linux, `KisakCOD-mp` can play them through a Bink shim. It is off by
default.

```sh
cmake ... -DKISAK_BUILD_LINUX_MP_CLIENT=ON -DKISAK_LINUX_CINEMATICS=ON
```

It needs libavcodec and libavutil (FFmpeg 6 or 7) for the target
architecture, found with pkg-config. Debian and Ubuntu package names:
`libavcodec-dev libavutil-dev`, with `:i386` for an i386 build. For an i386
build on an x86_64 host, point pkg-config at the i386 files:

```sh
PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig cmake ...
```

With the option on, `r_cinematic.cpp` is built with `CINEMA` through the native
D3D9 boundary. The engine code is the Windows code. The only changes are
Linux paths and a hunk allocator that returned a pointer as an `int`.

## How it fits together

`src/linux/client/cinematic`:

| File | What it does |
| --- | --- |
| `bink_file.cpp` | Reads the Bink 1 layout: header, audio track table, frame offsets, and each frame's audio and video packets. Revisions b to i only; the game's files are all revision i. |
| `bink_decoder.h` | The decoder interface. Any Bink 1 decoder can stand behind it. |
| `bink_decoder_ffmpeg.cpp` | The decoders on libavcodec: `binkvideo`, `binkaudio_dct` and `binkaudio_rdft`, set up as FFmpeg's own demuxer sets them up. libavformat is not used. |
| `bink_player.cpp` | The Bink 1.9 API that `r_cinematic.cpp` calls, declared by `deps/binklib/bink.h`. |
| `bink_audio_openal.cpp` | Movie sound: one streaming source in the engine's OpenAL context. |
| `bink_textures.h/.cpp` | `BINKTEXTURESET` and `Lock_Bink_textures`/`Unlock_Bink_textures`, in place of RAD's `binktextures.cpp`. RAD's file also compiles YUV shaders with D3DX, which the engine never uses. |

The engine keeps its own way of playing a movie. It decodes into two sets of
system-memory textures. Each new frame is copied into the textures the
`cinematic` material draws with, and that material converts YUV to RGB. The
shim decodes each frame into the buffer after the current one, as Bink does.

**Timing.** Frame *n* is due *(n − 1) / fps* seconds after the first frame was
shown, on a monotonic clock. Time spent paused is left out. When a movie loops,
both decoders start over.

**Sound.** Sound is decoded up to 0.75 s ahead of the picture and queued on the
OpenAL source, which starts with the first frame. The engine's volume
(`BinkSetMixBinVolumes`) becomes the source gain. Without an OpenAL context,
movies play without sound.

**IO.** The engine's background IO controls have nothing to steer. The shim
reads each frame with `pread` when it needs it. It reports its read buffer as
full, so the engine never pauses a movie for data. Memory-resident movies
(playback flag 8) are read whole by the engine, as on Windows.

**File names.** Script and console names can use any letter case, and the
engine builds paths with `\`. `KisakBink_ResolvePath` turns separators into `/`
and matches each missing path component without regard to case.

**Errors.** `BinkGetError` hands out the last error once. The engine checks for
an empty error after most Bink calls, so a movie that failed to open must not
fail the checks of the next one.

## Checks

`KisakCOD-bink-shim-test` (`tests/linux_cinematic/bink_shim_test.cpp`) needs no
game data:

- The file reader runs on Bink files the test makes up, and refuses bad ones.
- The Bink API runs on those files with a stand-in decoder, audio output and
  clock, the way `r_cinematic.cpp` drives it. This covers frame timing, the
  two buffers, pausing, looping, sound queued ahead, volume, path lookup and
  errors.
- libavcodec's Bink decoders are opened as the shim opens them.

CI builds the x86_64 client with the option on, checks that it links
libavcodec, and runs the test (`.github/workflows/build-kisakcod-linux.yaml`,
"x86_64 multiplayer client").

Playing a game movie needs the game files. That runs on a developer machine,
for example from the console:

```
cinematic <name>
```

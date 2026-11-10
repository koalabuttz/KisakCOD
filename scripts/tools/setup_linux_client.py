#!/usr/bin/env python3
"""Build a playable x86_64 Linux multiplayer client in one command.

    python3 scripts/tools/setup_linux_client.py [--prefix DIR] [--package DIR]

It does what docs/linux-mp-client.md otherwise asks for by hand:

  1. checks that the build tools and the OpenAL files are installed, and names
     the Debian/Ubuntu packages for what is missing (it installs nothing);
  2. fetches SDL3 at the revision the project pins and builds it into
     <prefix>/sdl3;
  3. fetches DXVK Native at the pinned 2.7.1 revision, applies the upstream
     fix that fullscreen on a Wayland session needs, and builds its D3D9
     library into <prefix>/dxvk;
  4. configures and builds KisakCOD-mp against the two;
  5. with --package, packs the client and those libraries into a folder that
     runs on another PC (scripts/tools/package_linux_client.py). That client
     is built without movies unless --movies is given, because a movie build
     needs the build machine's own FFmpeg version on every PC it runs on.

Everything it writes is under --prefix (default: kisak-sdk next to the
repository) and the build folder. A step whose result is already there at the
same revision and settings is skipped, so running it again after `git pull`
only rebuilds the client. No game file is read: the game folder is only named
when the client is started.

x86_64 only. The i386 client still needs the manual recipe.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SDL3_URL = "https://github.com/libsdl-org/SDL.git"
SDL3_REVISION = "877399b2b2cf21e67554ed9046410f268ce1d1b2"  # release-3.2.10
DXVK_URL = "https://github.com/doitsujin/dxvk.git"
DXVK_REVISION = "c3dd74be6baec53786d4e064a572185b70347a17"  # v2.7.1
# Upstream DXVK commits applied on top of the pinned revision, oldest first.
# a1abc9c4: "[wsi] Don't treat SDL_SetWindowPosition failure as fatal". DXVK
# 2.7.1 positions the window before going fullscreen, a Wayland session
# refuses to position it, and 2.7.1 gives up; with this the client goes
# fullscreen there (docs/linux-mp-client.md, "The game window and input").
DXVK_FIXES = ("a1abc9c4a913aedc2a575a2ee8cc98011b4e5fa3",)

MARKER = ".kisak-setup"  # in every folder this tool made and may remake

TOOLS = (
    # program names (any one), Debian/Ubuntu package
    (("git",), "git"),
    (("cmake",), "cmake"),
    (("ninja",), "ninja-build"),
    (("clang",), "clang"),
    (("clang++",), "clang"),
    (("meson",), "meson"),
    (("glslang", "glslangValidator"), "glslang-tools"),
    (("pkg-config",), "pkg-config"),
)

OPENAL_DIRS = ("/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib")


class SetupError(Exception):
    pass


class Runner:
    def __init__(self, dry_run: bool):
        self.dry_run = dry_run

    def run(self, command: list[str], cwd: Path | None = None, env: dict[str, str] | None = None,
            stdin: bytes | None = None) -> None:
        shown = " ".join(shlex.quote(part) for part in command)
        if env:
            shown = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items()) + " " + shown
        print(f"+ {shown}" + (f"   (in {cwd})" if cwd else ""), flush=True)
        if self.dry_run:
            return
        full_env = dict(os.environ)
        full_env.update(env or {})
        done = subprocess.run(command, cwd=cwd, env=full_env, input=stdin)
        if done.returncode != 0:
            raise SetupError(f"this step failed with exit status {done.returncode}: {shown}")

    def output(self, command: list[str], cwd: Path | None = None) -> bytes:
        done = subprocess.run(command, cwd=cwd, capture_output=True)
        if done.returncode != 0:
            raise SetupError(f"{' '.join(command)} failed: {done.stderr.decode(errors='replace').strip()}")
        return done.stdout


def missing_tools() -> list[tuple[str, str]]:
    missing = []
    for names, package in TOOLS:
        if not any(shutil.which(name) for name in names):
            missing.append((" or ".join(names), package))
    return missing


def find_openal(args: argparse.Namespace) -> tuple[Path, Path | None]:
    """The OpenAL library to link and, when it is not a system header, its include folder."""
    include = Path(args.openal_include) if args.openal_include else None
    if include is not None and not (include / "AL" / "al.h").is_file():
        raise SetupError(f"--openal-include {include} holds no AL/al.h")
    if args.openal_library:
        library = Path(args.openal_library)
        if not library.is_file():
            raise SetupError(f"--openal-library {library} is not a file")
    else:
        library = next((Path(folder) / "libopenal.so.1" for folder in OPENAL_DIRS
                        if (Path(folder) / "libopenal.so.1").is_file()), None)
        if library is None:
            raise SetupError(
                "no libopenal.so.1 found. Install OpenAL Soft (Debian/Ubuntu: libopenal-dev), "
                "or name the library with --openal-library")
    if include is None and not Path("/usr/include/AL/al.h").is_file():
        raise SetupError(
            "the OpenAL headers (AL/al.h) are not installed. Install libopenal-dev, or unpack it somewhere "
            "(apt-get download libopenal-dev; dpkg-deb -x) and pass --openal-include <that tree>/usr/include")
    return library, include


def stamp_matches(folder: Path, stamp: str) -> bool:
    marker = folder / MARKER
    return marker.is_file() and marker.read_text(encoding="utf-8") == stamp


def fresh_folder(folder: Path, runner: Runner) -> None:
    """Empty a folder this tool made earlier; refuse to touch anyone else's."""
    if folder.exists():
        if not (folder / MARKER).is_file() and any(folder.iterdir()):
            raise SetupError(
                f"{folder} exists and was not made by this tool (no {MARKER} file in it). "
                "Move it away or choose another --prefix")
        print(f"+ remove {folder}", flush=True)
        if not runner.dry_run:
            shutil.rmtree(folder)
    if not runner.dry_run:
        folder.mkdir(parents=True)
        (folder / MARKER).write_text("in progress\n", encoding="utf-8")


def fetch(runner: Runner, source: Path, url: str, revision: str, submodules: bool) -> None:
    fresh_folder(source, runner)
    runner.run(["git", "init", "-q", str(source)])
    runner.run(["git", "-C", str(source), "remote", "add", "origin", url])
    runner.run(["git", "-C", str(source), "fetch", "-q", "--depth", "1", "origin", revision])
    runner.run(["git", "-C", str(source), "checkout", "-q", "--detach", "FETCH_HEAD"])
    if not runner.dry_run:
        head = runner.output(["git", "-C", str(source), "rev-parse", "HEAD"]).decode().strip()
        if head != revision:
            raise SetupError(f"{source} is at {head}, expected the pinned {revision}")
    if submodules:
        runner.run(["git", "-C", str(source), "submodule", "update", "--init", "--depth", "1"])


def sdl3_video_drivers(build: Path) -> list[str]:
    """The desktop video drivers an SDL3 build folder was configured with."""
    found = set()
    for header in build.rglob("SDL_build_config.h"):
        for match in re.finditer(r"^#define SDL_VIDEO_DRIVER_(WAYLAND|X11)[ \t]+1[ \t]*$",
                                 header.read_text(encoding="utf-8", errors="replace"), re.MULTILINE):
            found.add(match.group(1).lower())
    return sorted(found)


def apply_fixes(runner: Runner, source: Path, base: str, fixes: tuple[str, ...]) -> None:
    """Apply upstream commits, as patches, to a tree fetched at `base`."""
    for fix in fixes:
        runner.run(["git", "-C", str(source), "fetch", "-q", "--depth", "2", "origin", fix])
        print(f"+ git -C {source} diff {fix}^ {fix} | git -C {source} apply", flush=True)
        if runner.dry_run:
            continue
        patch = runner.output(["git", "-C", str(source), "diff", f"{fix}^", fix])
        if not patch.strip():
            raise SetupError(f"commit {fix} has an empty diff")
        done = subprocess.run(["git", "-C", str(source), "apply"], input=patch)
        if done.returncode != 0:
            raise SetupError(f"commit {fix} does not apply to the pinned revision {base}")


def build_sdl3(runner: Runner, args: argparse.Namespace, prefix: Path) -> Path:
    install = prefix / "sdl3"
    stamp = f"SDL3 {SDL3_REVISION} x86_64 console={int(args.sdl_console_build)}\n"
    if stamp_matches(install, stamp):
        print(f"SDL3 is up to date in {install}")
        return install
    source = prefix / "src" / "SDL3"
    build = prefix / "build" / "SDL3"
    fetch(runner, source, SDL3_URL, SDL3_REVISION, submodules=False)
    fresh_folder(build, runner)
    fresh_folder(install, runner)
    configure = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_C_FLAGS=-m64",
        f"-DCMAKE_INSTALL_PREFIX={install}", "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_EXAMPLES=OFF",
    ]
    if args.sdl_console_build:
        # A machine with no desktop headers (a CI runner): SDL's dummy video
        # driver only. Not a build to play with.
        configure += ["-DSDL_UNIX_CONSOLE_BUILD=ON", "-DSDL_X11=OFF", "-DSDL_WAYLAND=OFF"]
    runner.run(configure)
    runner.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)])
    runner.run(["cmake", "--install", str(build)])
    if not runner.dry_run:
        if not (install / "lib" / "libSDL3.so.0").exists():
            raise SetupError(f"the SDL3 build left no {install}/lib/libSDL3.so.0")
        drivers = sdl3_video_drivers(build)
        print(f"SDL3 desktop video drivers: {' '.join(drivers) or 'none'}")
        if not args.sdl_console_build and "wayland" not in drivers:
            print("setup_linux_client: note: this SDL3 has no Wayland support, so the game will run through "
                  "XWayland on a Wayland desktop. For native Wayland install libwayland-dev, libxkbcommon-dev, "
                  f"libdecor-0-dev and libegl1-mesa-dev, remove {install} and run this again", file=sys.stderr)
        (install / MARKER).write_text(stamp, encoding="utf-8")
    return install


def build_dxvk(runner: Runner, args: argparse.Namespace, prefix: Path, sdl3: Path) -> Path:
    install = prefix / "dxvk"
    stamp = f"DXVK {DXVK_REVISION} + {' '.join(DXVK_FIXES)} x86_64 d3d9 sdl3={SDL3_REVISION}\n"
    if stamp_matches(install, stamp):
        print(f"DXVK is up to date in {install}")
        return install
    source = prefix / "src" / "dxvk"
    build = prefix / "build" / "dxvk"
    fetch(runner, source, DXVK_URL, DXVK_REVISION, submodules=True)
    apply_fixes(runner, source, DXVK_REVISION, DXVK_FIXES)
    fresh_folder(build, runner)
    fresh_folder(install, runner)
    if not runner.dry_run:
        (build / MARKER).unlink()  # meson wants an empty build folder
    runner.run(
        ["meson", "setup", str(build), str(source), "--buildtype", "release",
         "--prefix", str(install), "--libdir", "lib",
         "-Denable_d3d9=true", "-Denable_dxgi=false", "-Denable_d3d8=false",
         "-Denable_d3d10=false", "-Denable_d3d11=false",
         "-Dnative_sdl3=enabled", "-Dnative_sdl2=disabled", "-Dnative_glfw=disabled"],
        # The compiler the tool checked for and the client is built with, not
        # whatever `cc` happens to be.
        env={"CC": "clang", "CXX": "clang++", "PKG_CONFIG_PATH": str(sdl3 / "lib" / "pkgconfig")})
    runner.run(["ninja", "-C", str(build), "-j", str(args.jobs), "install"])
    if not runner.dry_run:
        (build / MARKER).write_text("build folder\n", encoding="utf-8")
        for needed in (install / "lib" / "libdxvk_d3d9.so", install / "include" / "dxvk" / "d3d9.h"):
            if not needed.exists():
                raise SetupError(f"the DXVK build left no {needed}")
        (install / MARKER).write_text(stamp, encoding="utf-8")
    return install


def build_client(runner: Runner, args: argparse.Namespace, sdl3: Path, dxvk: Path,
                 openal: Path, openal_include: Path | None) -> Path:
    build = Path(args.build_dir).resolve()
    out = build / "bin"
    configure = [
        "cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
        "-DKISAK_PLATFORM=linux", "-DKISAK_LINUX_ARCH=x86_64",
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={out}",
        "-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON",
        f"-DKISAK_DXVK_NATIVE_INCLUDE_DIR={dxvk / 'include' / 'dxvk'}",
        f"-DKISAK_DXVK_NATIVE_LIBRARY={dxvk / 'lib' / 'libdxvk_d3d9.so'}",
        "-DKISAK_BUILD_LINUX_MP_CLIENT=ON", f"-DKISAK_OPENAL_LIBRARY={openal}",
        f"-DSDL3_DIR={sdl3 / 'lib' / 'cmake' / 'SDL3'}",
        f"-DKISAK_LINUX_PORTABLE_RUNPATH={'ON' if args.package else 'OFF'}",
    ]
    if openal_include is not None:
        configure.append(f"-DKISAK_OPENAL_INCLUDE_DIR={openal_include}")
    if args.package and not args.movies:
        configure.append("-DKISAK_LINUX_CINEMATICS=OFF")
    configure += args.cmake_arg
    runner.run(configure)
    runner.run(["cmake", "--build", str(build), "--target", "KisakCOD-mp", "--parallel", str(args.jobs)])
    return out / "KisakCOD-mp"


def package_client(runner: Runner, args: argparse.Namespace, prefix: Path, client: Path) -> None:
    label = "unknown revision"
    if not runner.dry_run:
        try:
            label = runner.output(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"]).decode().strip()
        except SetupError:
            pass
        label += f", SDL3 {SDL3_REVISION[:8]}, DXVK {DXVK_REVISION[:8]} + {' '.join(fix[:8] for fix in DXVK_FIXES)}"
    command = [
        sys.executable, str(ROOT / "scripts" / "tools" / "package_linux_client.py"),
        "--binary", str(client),
        "--lib-dir", str(prefix / "sdl3" / "lib"), "--lib-dir", str(prefix / "dxvk" / "lib"),
        "--license", f"SDL3={prefix / 'src' / 'SDL3' / 'LICENSE.txt'}",
        "--license", f"DXVK={prefix / 'src' / 'dxvk' / 'LICENSE'}",
        "--license", f"KisakCOD={ROOT / 'LICENSE'}",
        "--label", label, "--out", str(Path(args.package).resolve()), "--archive",
    ]
    runner.run(command)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch and build pinned SDL3 and DXVK, then build the x86_64 Linux multiplayer client.")
    parser.add_argument("--prefix", default=str(ROOT.parent / "kisak-sdk"),
                        help="where SDL3 and DXVK are fetched, built and installed (default: kisak-sdk next to "
                             "the repository)")
    parser.add_argument("--build-dir", default=str(ROOT / "build-linux" / "mp-x64"),
                        help="the client's build folder (default: build-linux/mp-x64 in the repository)")
    parser.add_argument("--build-type", default="RelWithDebInfo",
                        choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="parallel build jobs")
    parser.add_argument("--package", metavar="DIR", default="",
                        help="also pack the client and its libraries into this new folder (and DIR.tar.gz); "
                             "builds the client with the run path a packed folder needs")
    parser.add_argument("--movies", action="store_true",
                        help="with --package: do not turn movies off. The packed client then starts only on a PC "
                             "that has the same FFmpeg library versions as this one")
    parser.add_argument("--sdk-only", action="store_true", help="build SDL3 and DXVK and stop before the client")
    parser.add_argument("--openal-library", default="", help="the libopenal.so.1 to link (default: the system's)")
    parser.add_argument("--openal-include", default="",
                        help="a folder that holds AL/al.h, when the OpenAL headers are not installed")
    parser.add_argument("--sdl-console-build", action="store_true",
                        help="build SDL3 without desktop support, for a machine with no X11 or Wayland headers "
                             "(CI); the result cannot open a real window")
    parser.add_argument("--cmake-arg", action="append", default=[],
                        help="one more argument for the client's CMake configure (repeat)")
    parser.add_argument("--dry-run", action="store_true", help="print the commands and run nothing")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    runner = Runner(args.dry_run)
    prefix = Path(args.prefix).resolve()
    try:
        if os.uname().machine not in ("x86_64", "amd64"):
            raise SetupError(f"this builds the x86_64 client on an x86_64 machine; this one is {os.uname().machine}")
        missing = missing_tools()
        if missing:
            lines = "\n".join(f"  {name}  (package {package})" for name, package in missing)
            packages = " ".join(dict.fromkeys(package for _name, package in missing))
            raise SetupError(f"these build tools are missing:\n{lines}\nOn Debian/Ubuntu: sudo apt install {packages}")
        openal, openal_include = (Path("unused"), None) if args.sdk_only else find_openal(args)
        if args.package and Path(args.package).exists() and any(Path(args.package).iterdir()):
            raise SetupError(f"--package {args.package} exists and is not empty; choose a new folder")
        sdl3 = build_sdl3(runner, args, prefix)
        dxvk = build_dxvk(runner, args, prefix, sdl3)
        if args.sdk_only:
            print(f"SDL3: {sdl3}\nDXVK: {dxvk}")
            return 0
        client = build_client(runner, args, sdl3, dxvk, openal, openal_include)
        if args.package:
            package_client(runner, args, prefix, client)
            print(f"\nPacked. Start it with: {Path(args.package).resolve() / 'run-mp.sh'} --game <game folder>")
        else:
            libs = f"{sdl3 / 'lib'}:{dxvk / 'lib'}"
            print(f"\nBuilt {client}\nStart it with:\n  LD_LIBRARY_PATH={shlex.quote(libs)} "
                  f"{shlex.quote(str(client))} +set fs_basepath <game folder>")
    except SetupError as error:
        print(f"setup_linux_client: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

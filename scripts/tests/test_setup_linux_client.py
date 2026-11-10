#!/usr/bin/env python3
"""Asset-free, network-free test of scripts/tools/setup_linux_client.py.

It does not build SDL3, DXVK or the client: the hosted "Linux client setup
tool" workflow does that when the tool changes. Here the tool's own logic is
checked: its pins are the ones the hosted jobs use, a dry run prints the whole
plan and writes nothing, a folder somebody else made is never removed, a
finished step is skipped, and fetching a pinned revision and applying a later
commit to it works, on a made-up local repository.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "tools" / "setup_linux_client.py"
LINUX_WORKFLOW = ROOT / ".github" / "workflows" / "build-kisakcod-linux.yaml"
CLIENT_MODULE = ROOT / "scripts" / "platform" / "linux" / "mp_client.cmake"

spec = importlib.util.spec_from_file_location("setup_linux_client", TOOL)
setup = importlib.util.module_from_spec(spec)
sys.modules["setup_linux_client"] = setup
spec.loader.exec_module(setup)

GIT = shutil.which("git")


def run_tool(arguments: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = setup.main(arguments)
    return status, out.getvalue(), err.getvalue()


def git(folder: Path, *arguments: str) -> str:
    done = subprocess.run(
        [GIT, "-C", str(folder), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
         "-c", "commit.gpgsign=false", *arguments],
        check=True, capture_output=True, text=True)
    return done.stdout.strip()


class PinTests(unittest.TestCase):
    def test_the_pins_are_the_ones_the_hosted_jobs_build(self):
        workflow = LINUX_WORKFLOW.read_text(encoding="utf-8")
        sdl3 = set(re.findall(r"^\s+SDL3_REVISION: ([0-9a-f]{40})$", workflow, re.MULTILINE))
        dxvk = set(re.findall(r"^\s+DXVK_REVISION: ([0-9a-f]{40})$", workflow, re.MULTILINE))
        self.assertEqual(sdl3, {setup.SDL3_REVISION})
        self.assertEqual(dxvk, {setup.DXVK_REVISION})

    def test_the_fixes_are_full_hashes_and_the_wayland_one_is_there(self):
        for fix in setup.DXVK_FIXES:
            self.assertRegex(fix, r"^[0-9a-f]{40}$")
        self.assertIn("a1abc9c4a913aedc2a575a2ee8cc98011b4e5fa3", setup.DXVK_FIXES)

    def test_every_build_option_the_tool_passes_exists(self):
        """A renamed CMake option would otherwise be passed and silently ignored."""
        known = CLIENT_MODULE.read_text(encoding="utf-8")
        for path in (ROOT / "scripts" / "platform" / "linux").glob("*"):
            if path.is_file():
                known += path.read_text(encoding="utf-8", errors="replace")
        known += (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
        source = TOOL.read_text(encoding="utf-8")
        options = set(re.findall(r"-D(KISAK_[A-Z0-9_]+)", source))
        self.assertIn("KISAK_LINUX_PORTABLE_RUNPATH", options)
        for option in sorted(options):
            self.assertIn(option, known, option)


@unittest.skipUnless(sys.platform.startswith("linux"), "needs Linux")
class PlanTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="kisak-setup-test-"))
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.prefix = self.temp / "sdk"
        self.build = self.temp / "build"
        (self.temp / "al" / "AL").mkdir(parents=True)
        (self.temp / "al" / "AL" / "al.h").write_text("/* made up */\n")
        (self.temp / "libopenal.so.1").write_bytes(b"made up")
        self.base = [
            "--prefix", str(self.prefix), "--build-dir", str(self.build), "--jobs", "3",
            "--openal-library", str(self.temp / "libopenal.so.1"), "--openal-include", str(self.temp / "al"),
        ]
        which = mock.patch.object(setup.shutil, "which", lambda name: f"/usr/bin/{name}")
        which.start()
        self.addCleanup(which.stop)
        machine = mock.patch.object(setup.os, "uname", lambda: mock.Mock(machine="x86_64"))
        machine.start()
        self.addCleanup(machine.stop)

    def test_a_dry_run_prints_the_plan_and_writes_nothing(self):
        status, out, err = run_tool(self.base + ["--dry-run"])
        self.assertEqual((status, err), (0, ""))
        self.assertFalse(self.prefix.exists())
        self.assertFalse(self.build.exists())
        lines = [line for line in out.splitlines() if line.startswith("+ ")]
        text = "\n".join(lines)
        # SDL3 first, then DXVK with its fix, then the client.
        order = [text.index(f"fetch -q --depth 1 origin {setup.SDL3_REVISION}"),
                 text.index(f"fetch -q --depth 1 origin {setup.DXVK_REVISION}"),
                 text.index(f"fetch -q --depth 2 origin {setup.DXVK_FIXES[0]}"),
                 text.index("meson setup"),
                 text.index("-DKISAK_BUILD_LINUX_MP_CLIENT=ON"),
                 text.index("--target KisakCOD-mp")]
        self.assertEqual(order, sorted(order))
        self.assertIn(f"diff {setup.DXVK_FIXES[0]}^ {setup.DXVK_FIXES[0]} | git", text)
        meson = next(line for line in lines if "meson setup" in line)
        for flag in ("-Denable_d3d9=true", "-Denable_dxgi=false", "-Denable_d3d8=false", "-Denable_d3d10=false",
                     "-Denable_d3d11=false", "-Dnative_sdl3=enabled", "-Dnative_sdl2=disabled",
                     "-Dnative_glfw=disabled", f"--prefix {self.prefix / 'dxvk'}"):
            self.assertIn(flag, meson)
        self.assertTrue(meson.startswith(
            f"+ CC=clang CXX=clang++ PKG_CONFIG_PATH={self.prefix / 'sdl3' / 'lib' / 'pkgconfig'} meson setup "))
        client = next(line for line in lines if "-DKISAK_BUILD_LINUX_MP_CLIENT=ON" in line)
        for flag in ("-DKISAK_PLATFORM=linux", "-DKISAK_LINUX_ARCH=x86_64", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                     "-DKISAK_BUILD_LINUX_NATIVE_D3D9=ON", "-DKISAK_LINUX_PORTABLE_RUNPATH=OFF",
                     f"-DKISAK_DXVK_NATIVE_LIBRARY={self.prefix / 'dxvk' / 'lib' / 'libdxvk_d3d9.so'}",
                     f"-DKISAK_DXVK_NATIVE_INCLUDE_DIR={self.prefix / 'dxvk' / 'include' / 'dxvk'}",
                     f"-DSDL3_DIR={self.prefix / 'sdl3' / 'lib' / 'cmake' / 'SDL3'}",
                     f"-DKISAK_OPENAL_LIBRARY={self.temp / 'libopenal.so.1'}",
                     f"-DKISAK_OPENAL_INCLUDE_DIR={self.temp / 'al'}"):
            self.assertIn(flag, client)
        self.assertNotIn("KISAK_LINUX_CINEMATICS", client)
        self.assertNotIn("SDL_UNIX_CONSOLE_BUILD", text)
        self.assertNotIn("package_linux_client.py", text)
        self.assertIn("LD_LIBRARY_PATH=", out)

    def test_a_package_gets_the_portable_run_path_and_no_movies(self):
        package = self.temp / "package"
        status, out, _err = run_tool(self.base + ["--dry-run", "--package", str(package)])
        self.assertEqual(status, 0)
        client = next(line for line in out.splitlines() if "-DKISAK_BUILD_LINUX_MP_CLIENT=ON" in line)
        self.assertIn("-DKISAK_LINUX_PORTABLE_RUNPATH=ON", client)
        self.assertIn("-DKISAK_LINUX_CINEMATICS=OFF", client)
        pack = next(line for line in out.splitlines() if "package_linux_client.py" in line)
        for part in (f"--binary {self.build / 'bin' / 'KisakCOD-mp'}", f"--lib-dir {self.prefix / 'sdl3' / 'lib'}",
                     f"--lib-dir {self.prefix / 'dxvk' / 'lib'}", f"--out {package}", "--archive",
                     "--license SDL3=", "--license DXVK=", "--license KisakCOD="):
            self.assertIn(part, pack)
        self.assertFalse(package.exists())

        status, out, _err = run_tool(self.base + ["--dry-run", "--package", str(package), "--movies"])
        self.assertEqual(status, 0)
        self.assertNotIn("KISAK_LINUX_CINEMATICS", out)

    def test_a_package_folder_that_holds_something_is_refused_before_any_step(self):
        package = self.temp / "package"
        package.mkdir()
        (package / "keep.txt").write_text("mine\n")
        status, out, err = run_tool(self.base + ["--dry-run", "--package", str(package)])
        self.assertEqual(status, 1)
        self.assertIn("exists and is not empty", err)
        self.assertNotIn("+ ", out)

    def test_extra_configure_arguments_and_the_console_build_are_passed_on(self):
        status, out, _err = run_tool(self.base + ["--dry-run", "--sdl-console-build", "--build-type", "Debug",
                                                  "--cmake-arg=-DKISAK_LINUX_HTTP_DOWNLOADS=ON"])
        self.assertEqual(status, 0)
        self.assertIn("-DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF", out)
        client = next(line for line in out.splitlines() if "-DKISAK_BUILD_LINUX_MP_CLIENT=ON" in line)
        self.assertTrue(client.endswith("-DKISAK_LINUX_HTTP_DOWNLOADS=ON"))
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", client)

    def test_sdk_only_stops_before_the_client_and_needs_no_openal(self):
        status, out, _err = run_tool(["--prefix", str(self.prefix), "--dry-run", "--sdk-only"])
        self.assertEqual(status, 0)
        self.assertIn("meson setup", out)
        self.assertNotIn("KISAK_BUILD_LINUX_MP_CLIENT", out)

    def test_missing_tools_are_named_with_their_packages(self):
        present = {"git", "cmake", "clang", "clang++", "pkg-config", "glslangValidator"}
        with mock.patch.object(setup.shutil, "which", lambda name: name if name in present else None):
            status, out, err = run_tool(self.base + ["--dry-run"])
        self.assertEqual(status, 1)
        self.assertIn("ninja  (package ninja-build)", err)
        self.assertIn("meson  (package meson)", err)
        self.assertNotIn("glslang", err)
        self.assertIn("sudo apt install ninja-build meson", err)
        self.assertNotIn("+ ", out)

    def test_another_machine_type_is_refused(self):
        with mock.patch.object(setup.os, "uname", lambda: mock.Mock(machine="aarch64")):
            status, _out, err = run_tool(self.base + ["--dry-run"])
        self.assertEqual(status, 1)
        self.assertIn("aarch64", err)

    def test_openal_problems_are_explained(self):
        status, _out, err = run_tool(["--prefix", str(self.prefix), "--dry-run",
                                      "--openal-library", str(self.temp / "none.so")])
        self.assertEqual(status, 1)
        self.assertIn("is not a file", err)
        status, _out, err = run_tool(["--prefix", str(self.prefix), "--dry-run",
                                      "--openal-library", str(self.temp / "libopenal.so.1"),
                                      "--openal-include", str(self.temp)])
        self.assertEqual(status, 1)
        self.assertIn("holds no AL/al.h", err)

    def test_finished_steps_are_skipped(self):
        for name, stamp in (
                ("sdl3", f"SDL3 {setup.SDL3_REVISION} x86_64 console=0\n"),
                ("dxvk", f"DXVK {setup.DXVK_REVISION} + {' '.join(setup.DXVK_FIXES)} x86_64 d3d9 "
                         f"sdl3={setup.SDL3_REVISION}\n")):
            (self.prefix / name).mkdir(parents=True)
            (self.prefix / name / setup.MARKER).write_text(stamp)
        status, out, _err = run_tool(self.base + ["--dry-run"])
        self.assertEqual(status, 0)
        self.assertIn("SDL3 is up to date", out)
        self.assertIn("DXVK is up to date", out)
        self.assertNotIn("git", "\n".join(line for line in out.splitlines() if line.startswith("+ ")))
        self.assertIn("-DKISAK_BUILD_LINUX_MP_CLIENT=ON", out)

        # Another SDL3 setting makes SDL3 stale, but not DXVK.
        status, out, _err = run_tool(self.base + ["--dry-run", "--sdl-console-build"])
        self.assertEqual(status, 0)
        self.assertNotIn("SDL3 is up to date", out)
        self.assertIn("DXVK is up to date", out)

    def test_a_folder_somebody_else_made_is_never_removed(self):
        (self.prefix / "src" / "SDL3").mkdir(parents=True)
        (self.prefix / "src" / "SDL3" / "notes.txt").write_text("mine\n")
        status, _out, err = run_tool(self.base + ["--sdk-only"])
        self.assertEqual(status, 1)
        self.assertIn("was not made by this tool", err)
        self.assertEqual((self.prefix / "src" / "SDL3" / "notes.txt").read_text(), "mine\n")

    def test_a_folder_the_tool_made_is_emptied(self):
        folder = self.prefix / "build" / "SDL3"
        folder.mkdir(parents=True)
        (folder / setup.MARKER).write_text("old\n")
        (folder / "stale.o").write_text("old\n")
        with contextlib.redirect_stdout(io.StringIO()):
            setup.fresh_folder(folder, setup.Runner(dry_run=False))
        self.assertEqual(sorted(path.name for path in folder.iterdir()), [setup.MARKER])
        self.assertEqual((folder / setup.MARKER).read_text(), "in progress\n")

    def test_the_built_video_drivers_are_read_from_the_sdl_build(self):
        config = self.temp / "sdl-build" / "include-config-release" / "build_config"
        config.mkdir(parents=True)
        (config / "SDL_build_config.h").write_text(
            "#define SDL_VIDEO_DRIVER_DUMMY 1\n"
            "#define SDL_VIDEO_DRIVER_WAYLAND 1\n"
            "#define SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC \"libwayland-client.so.0\"\n"
            "/* #undef SDL_VIDEO_DRIVER_X11 */\n"
            "#define SDL_VIDEO_DRIVER_X11_XRANDR 1\n")
        self.assertEqual(setup.sdl3_video_drivers(self.temp / "sdl-build"), ["wayland"])
        self.assertEqual(setup.sdl3_video_drivers(self.temp / "al"), [])


@unittest.skipUnless(sys.platform.startswith("linux") and GIT, "needs Linux and git")
class FetchTests(unittest.TestCase):
    """The fetch and the fix, on a made-up repository standing in for DXVK."""

    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="kisak-setup-fetch-"))
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.origin = self.temp / "origin"
        self.origin.mkdir()
        git(self.origin, "init", "-q", "-b", "main")
        git(self.origin, "config", "uploadpack.allowAnySHA1InWant", "true")
        (self.origin / "window.cpp").write_text("one\nif (!move()) return false;\nthree\n")
        git(self.origin, "add", "window.cpp")
        git(self.origin, "commit", "-q", "-m", "the pinned release")
        self.pinned = git(self.origin, "rev-parse", "HEAD")
        (self.origin / "other.cpp").write_text("unrelated later work\n")
        git(self.origin, "add", "other.cpp")
        git(self.origin, "commit", "-q", "-m", "later work the pin does not have")
        (self.origin / "window.cpp").write_text("one\nmove();\nthree\n")
        git(self.origin, "add", "window.cpp")
        git(self.origin, "commit", "-q", "-m", "the fix")
        self.fix = git(self.origin, "rev-parse", "HEAD")
        (self.origin / "window.cpp").write_text("one\nmove();\nthree\nfour\n")
        git(self.origin, "add", "window.cpp")
        git(self.origin, "commit", "-q", "-m", "newest")
        self.url = self.origin.as_uri()
        self.runner = setup.Runner(dry_run=False)
        self.source = self.temp / "sdk" / "src" / "dxvk"

    def fetch(self, revision: str) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            setup.fetch(self.runner, self.source, self.url, revision, submodules=False)

    def test_the_pinned_revision_is_fetched_and_only_the_fix_is_applied(self):
        self.fetch(self.pinned)
        self.assertEqual(git(self.source, "rev-parse", "HEAD"), self.pinned)
        self.assertFalse((self.source / "other.cpp").exists())
        with contextlib.redirect_stdout(io.StringIO()):
            setup.apply_fixes(self.runner, self.source, self.pinned, (self.fix,))
        self.assertEqual((self.source / "window.cpp").read_text(), "one\nmove();\nthree\n")
        # Only the fix: the work between the pin and the fix did not come along.
        self.assertFalse((self.source / "other.cpp").exists())
        self.assertEqual(git(self.source, "rev-parse", "HEAD"), self.pinned)

        # Fetching again starts from a clean tree.
        self.fetch(self.pinned)
        self.assertIn("return false", (self.source / "window.cpp").read_text())

    def test_a_fix_that_does_not_apply_stops_the_setup(self):
        self.fetch(self.pinned)
        (self.source / "window.cpp").write_text("something else entirely\n")
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(setup.SetupError) as raised:
            with mock.patch.object(setup.subprocess, "run", self.quiet_run):
                setup.apply_fixes(self.runner, self.source, self.pinned, (self.fix,))
        self.assertIn("does not apply to the pinned revision", str(raised.exception))

    def test_an_unknown_revision_stops_the_setup(self):
        with self.assertRaises(setup.SetupError), mock.patch.object(setup.subprocess, "run", self.quiet_run):
            self.fetch("0" * 40)

    @staticmethod
    def quiet_run(command, **kwargs):
        if "capture_output" not in kwargs:
            kwargs.setdefault("stdout", subprocess.DEVNULL)
            kwargs.setdefault("stderr", subprocess.DEVNULL)
        return REAL_RUN(command, **kwargs)


REAL_RUN = subprocess.run


if __name__ == "__main__":
    unittest.main()

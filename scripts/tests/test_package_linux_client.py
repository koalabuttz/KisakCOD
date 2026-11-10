#!/usr/bin/env python3
"""Asset-free test of scripts/tools/package_linux_client.py.

Builds a made-up "game" with the host C compiler: a binary that needs one
library from a made-up SDK folder, which in turn needs a second library next to
it. Packs it, moves the SDK away, and runs the packed launcher, so a pass means
the folder really carries what the binary needs. No game file and no real SDK
is involved.
"""

from __future__ import annotations

import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "tools" / "package_linux_client.py"

spec = importlib.util.spec_from_file_location("package_linux_client", TOOL)
packer = importlib.util.module_from_spec(spec)
sys.modules["package_linux_client"] = packer
spec.loader.exec_module(packer)

CC = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc") or shutil.which("clang")

DEP_SOURCE = 'const char *kdep_word(void) { return "deep"; }\n'
SDK_SOURCE = (
    "const char *kdep_word(void);\n"
    "const char *ksdk_word(void) { return kdep_word(); }\n"
)
GAME_SOURCE = r"""
#include <stdio.h>
const char *ksdk_word(void);
int main(int argc, char **argv)
{
    printf("word=%s\n", ksdk_word());
    for (int i = 1; i < argc; ++i)
        printf("arg=%s\n", argv[i]);
    return 0;
}
"""


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


@unittest.skipUnless(sys.platform.startswith("linux") and CC, "needs Linux and a C compiler")
class PackageLinuxClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="kisak-package-test-")
        cls.base = Path(cls.temp.name)
        cls.sdk = cls.base / "sdk" / "lib"
        cls.build = cls.base / "build"
        cls.sdk.mkdir(parents=True)
        cls.build.mkdir()
        for name, text in (("dep.c", DEP_SOURCE), ("sdk.c", SDK_SOURCE), ("game.c", GAME_SOURCE)):
            (cls.base / name).write_text(text)

        def cc(*arguments):
            done = run([CC, *arguments])
            if done.returncode != 0:
                raise RuntimeError(f"{CC} {' '.join(arguments)} failed:\n{done.stderr}")

        cc("-shared", "-fPIC", "-Wl,-soname,libkdep.so.1", str(cls.base / "dep.c"),
           "-o", str(cls.sdk / "libkdep.so.1.2.3"))
        (cls.sdk / "libkdep.so.1").symlink_to("libkdep.so.1.2.3")
        (cls.sdk / "libkdep.so").symlink_to("libkdep.so.1")
        cc("-shared", "-fPIC", "-Wl,-soname,libksdk.so.0", str(cls.base / "sdk.c"),
           "-L", str(cls.sdk), "-lkdep", "-o", str(cls.sdk / "libksdk.so.0"))
        (cls.sdk / "libksdk.so").symlink_to("libksdk.so.0")
        # The made-up SDK also holds a file named like the C library. It must
        # never be packed.
        cc("-shared", "-fPIC", "-Wl,-soname,libm.so.6", str(cls.base / "dep.c"), "-o", str(cls.sdk / "libm.so.6"))

        common = [str(cls.base / "game.c"), "-Wl,--no-as-needed", "-L", str(cls.sdk), "-lksdk", "-lm",
                  f"-Wl,-rpath-link,{cls.sdk}"]
        cls.portable = cls.build / "portable" / "KisakCOD-mp"
        cls.portable.parent.mkdir()
        cc(*common, "-Wl,--enable-new-dtags", "-Wl,-rpath,$ORIGIN/lib", "-o", str(cls.portable))
        cls.absolute = cls.build / "absolute" / "KisakCOD-mp"
        cls.absolute.parent.mkdir()
        cc(*common, "-Wl,--enable-new-dtags", f"-Wl,-rpath,{cls.sdk}", "-o", str(cls.absolute))
        cls.second = cls.build / "portable" / "KisakCOD-dedi"
        cc(*common, "-Wl,--enable-new-dtags", "-Wl,-rpath,$ORIGIN/lib", "-o", str(cls.second))

        cls.game = cls.base / "game folder"
        (cls.game / "main").mkdir(parents=True)
        (cls.game / "zone").mkdir()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix="out-", dir=self.base)) / "KisakCOD-linux"

    def pack(self, *extra, binary=None, lib_dirs=None):
        command = [sys.executable, str(TOOL), "--binary", str(binary or self.portable), "--out", str(self.out)]
        for directory in ([self.sdk] if lib_dirs is None else lib_dirs):
            command += ["--lib-dir", str(directory)]
        return run(command + list(extra))

    def launch(self, *arguments, cwd=None, env=None, launcher="run-mp.sh"):
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("LD_LIBRARY_PATH", "KISAKCOD_GAME_DIR")}
        environment.update(env or {})
        return run([str(self.out / launcher), *arguments], cwd=cwd or self.base, env=environment)

    def test_elf_reader_matches_the_files(self):
        elf = packer.Elf(self.portable)
        self.assertIn("libksdk.so.0", elf.needed)
        self.assertIn("libm.so.6", elf.needed)
        self.assertEqual(elf.runpath, ["$ORIGIN/lib"])
        self.assertEqual(elf.runpath_tag, "RUNPATH")
        library = packer.Elf(self.sdk / "libksdk.so.0")
        self.assertEqual(library.soname, "libksdk.so.0")
        self.assertIn("libkdep.so.1", library.needed)
        self.assertTrue(library.same_kind(elf))
        self.assertEqual(packer.Elf(self.absolute).runpath, [str(self.sdk)])
        with self.assertRaises(packer.PackageError):
            packer.Elf(self.base / "game.c")

    def test_packed_folder_runs_without_the_sdk(self):
        done = self.pack("--label", "test build 1")
        self.assertEqual(done.returncode, 0, done.stderr)
        files = sorted(path.relative_to(self.out).as_posix() for path in self.out.rglob("*") if path.is_file())
        self.assertEqual(files, ["KisakCOD-mp", "MANIFEST.txt", "README.txt",
                                 "lib/libkdep.so.1", "lib/libksdk.so.0", "run-mp.sh"])
        self.assertFalse((self.out / "lib" / "libkdep.so.1").is_symlink())
        self.assertTrue(os.access(self.out / "run-mp.sh", os.X_OK))
        self.assertTrue(os.access(self.out / "KisakCOD-mp", os.X_OK))

        readme = (self.out / "README.txt").read_text()
        self.assertIn("Build: test build 1", readme)
        self.assertIn("./run-mp.sh --game", readme)
        self.assertIn("libm.so.6", readme.split("What this PC must provide")[1].split("In this folder")[0])
        self.assertNotIn(str(self.base), readme)

        manifest = (self.out / "MANIFEST.txt").read_text().splitlines()
        self.assertEqual(manifest[0], packer.MANIFEST_HEADER)
        listed = {}
        for line in manifest[3:]:
            digest, size, name = line.split("  ")
            listed[name] = (digest, int(size))
        self.assertEqual(sorted(listed), [name for name in files if name != "MANIFEST.txt"])
        for name, (digest, size) in listed.items():
            data = (self.out / name).read_bytes()
            self.assertEqual((hashlib.sha256(data).hexdigest(), len(data)), (digest, size), name)

        hidden = self.sdk.parent.with_name("sdk-moved-away")
        self.sdk.parent.rename(hidden)
        try:
            done = self.launch("--game", str(self.game), "+set", "r_fullscreen", "0")
        finally:
            hidden.rename(self.sdk.parent)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.splitlines(), [
            "word=deep", "arg=+set", "arg=fs_basepath", f"arg={self.game.resolve()}",
            "arg=+set", "arg=r_fullscreen", "arg=0"])

    def test_launcher_finds_the_game_folder(self):
        self.assertEqual(self.pack().returncode, 0)
        by_env = self.launch("+map", "x", env={"KISAKCOD_GAME_DIR": str(self.game)})
        self.assertEqual(by_env.returncode, 0, by_env.stderr)
        self.assertIn(f"arg={self.game.resolve()}", by_env.stdout)
        by_cwd = self.launch(cwd=self.game)
        self.assertEqual(by_cwd.returncode, 0, by_cwd.stderr)
        self.assertIn(f"arg={self.game.resolve()}", by_cwd.stdout)
        own = self.launch("+set", "fs_basepath", "/somewhere else", "+map", "x")
        self.assertEqual(own.returncode, 0, own.stderr)
        self.assertEqual(own.stdout.splitlines(),
                         ["word=deep", "arg=+set", "arg=fs_basepath", "arg=/somewhere else", "arg=+map", "arg=x"])
        # Through a symlink and from another folder, the launcher still finds its own lib/.
        link = self.base / f"link-{self.out.parent.name}"
        link.symlink_to(self.out / "run-mp.sh")
        linked = run([str(link), "--game", str(self.game)], cwd="/")
        self.assertEqual(linked.returncode, 0, linked.stderr)
        self.assertIn("word=deep", linked.stdout)

    def test_launcher_refuses_without_a_game_folder(self):
        self.assertEqual(self.pack().returncode, 0)
        none = self.launch()
        self.assertEqual(none.returncode, 2)
        self.assertIn("no game folder", none.stderr)
        self.assertEqual(none.stdout, "")
        wrong = self.launch("--game", str(self.base))
        self.assertEqual(wrong.returncode, 2)
        self.assertIn("has no main folder", wrong.stderr)
        missing = self.launch("--game")
        self.assertEqual(missing.returncode, 2)
        self.assertIn("--game needs a folder", missing.stderr)

    def test_build_machine_runpath_is_refused(self):
        done = self.pack(binary=self.absolute)
        self.assertEqual(done.returncode, 1)
        self.assertIn("KISAK_LINUX_PORTABLE_RUNPATH", done.stderr)
        self.assertFalse(self.out.exists())
        allowed = self.pack("--allow-build-runpath", binary=self.absolute)
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertIn("keep this package on the machine that built it", (self.out / "README.txt").read_text())

    def test_system_libraries_are_never_packed(self):
        self.assertEqual(self.pack().returncode, 0)
        self.assertFalse((self.out / "lib" / "libm.so.6").exists())
        for name in ("libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libvulkan.so.1",
                     "ld-linux-x86-64.so.2", "libGL.so.1", "libpthread.so.0"):
            self.assertIsNotNone(packer.SYSTEM_ONLY.match(name), name)
        for name in ("libSDL3.so.0", "libdxvk_d3d9.so.0", "libopenal.so.1", "libcurl.so.4", "libmad.so.0"):
            self.assertIsNone(packer.SYSTEM_ONLY.match(name), name)

    def test_a_library_can_be_left_to_the_system(self):
        done = self.pack("--system-only", "libkdep.so.1")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue((self.out / "lib" / "libksdk.so.0").exists())
        self.assertFalse((self.out / "lib" / "libkdep.so.1").exists())
        self.assertIn("libkdep.so.1    (needed by libksdk.so.0)", (self.out / "README.txt").read_text())

    def test_missing_libraries_are_listed_not_packed(self):
        done = self.pack(lib_dirs=[])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertFalse((self.out / "lib").exists())
        self.assertIn("libksdk.so.0    (needed by KisakCOD-mp)", (self.out / "README.txt").read_text())

    def test_a_library_of_another_architecture_is_skipped(self):
        other = self.base / f"other-{self.out.parent.name}"
        other.mkdir()
        data = bytearray((self.sdk / "libksdk.so.0").read_bytes())
        machine = struct.unpack_from("<H", data, 18)[0]
        struct.pack_into("<H", data, 18, 183 if machine != 183 else 62)
        (other / "libksdk.so.0").write_bytes(data)
        done = self.pack(lib_dirs=[other, self.sdk])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("skipped", done.stdout)
        self.assertEqual((self.out / "lib" / "libksdk.so.0").read_bytes(), (self.sdk / "libksdk.so.0").read_bytes())

    def test_also_packs_a_library_nothing_lists(self):
        extra = self.base / f"extra-{self.out.parent.name}"
        extra.mkdir()
        shutil.copyfile(self.sdk / "libkdep.so.1.2.3", extra / "libkloaded.so.2")
        done = self.pack("--also", "libkloaded.so.2", lib_dirs=[self.sdk, extra])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue((self.out / "lib" / "libkloaded.so.2").exists())
        absent = self.pack("--also", "libknowhere.so.9")
        self.assertEqual(absent.returncode, 1)

    def test_two_binaries_licenses_and_archive(self):
        license_text = self.base / "LICENSE-sdk.txt"
        license_text.write_text("made-up licence\n")
        bad = self.pack("--license", "no-separator")
        self.assertEqual(bad.returncode, 1)
        self.assertIn("NAME=FILE", bad.stderr)
        self.assertFalse(self.out.exists())
        done = self.pack("--binary", str(self.second), "--license", f"ksdk={license_text}", "--archive")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((self.out / "licenses" / "ksdk.txt").read_text(), "made-up licence\n")
        dedi = self.launch("--game", str(self.game), launcher="run-dedi.sh")
        self.assertEqual(dedi.returncode, 0, dedi.stderr)
        self.assertIn("word=deep", dedi.stdout)
        archive = self.out.parent / "KisakCOD-linux.tar.gz"
        with tarfile.open(archive) as tar:
            members = {member.name: member for member in tar.getmembers()}
        self.assertIn("KisakCOD-linux/run-mp.sh", members)
        self.assertIn("KisakCOD-linux/lib/libkdep.so.1", members)
        self.assertEqual(members["KisakCOD-linux/run-mp.sh"].mode & 0o111, 0o111)
        self.assertEqual(members["KisakCOD-linux/lib/libkdep.so.1"].mode & 0o111, 0)
        self.assertTrue(all(member.uid == 0 and member.uname == "" for member in members.values()))

    def test_an_existing_folder_is_left_alone(self):
        self.out.mkdir(parents=True)
        (self.out / "keep.txt").write_text("mine\n")
        done = self.pack()
        self.assertEqual(done.returncode, 1)
        self.assertIn("not an empty folder", done.stderr)
        self.assertEqual(sorted(path.name for path in self.out.iterdir()), ["keep.txt"])


if __name__ == "__main__":
    unittest.main()

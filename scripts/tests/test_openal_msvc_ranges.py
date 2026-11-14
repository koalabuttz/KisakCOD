#!/usr/bin/env python3
"""The MSVC patch step for the fetched OpenAL Soft (scripts/extern/openal_msvc_ranges.cmake).

MSVC 14.52 rejects the range adaptor built from unique_ptr's operator* in
OpenAL Soft 1.25.2's alc/alc.cpp (C3889), which broke the "MSVC ARM64 build"
job. The patch rewrites that one line as a lambda, and must be harmless to run
twice or on sources that no longer have the line.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "scripts/extern/openal_msvc_ranges.cmake"
OLD = "std::views::transform(&ContextBase::EffectSlotCluster::operator*)"
NEW = "std::views::transform([](auto &cluster) -> decltype(auto) { return *cluster; })"
SOURCE = f"""#include <ranges>
void f(Context *context)
{{
    std::ranges::for_each(context->mEffectSlotClusters
        | {OLD}
        | std::views::join, [](EffectSlotBase &slot)
    {{
        slot.mWetBuffer.clear(); /* keeps; semicolons, [brackets] and "quotes" */
    }});
}}
"""


@unittest.skipUnless(shutil.which("cmake"), "cmake is not installed")
class OpenAlMsvcRangesTests(unittest.TestCase):
    def run_patch(self, source_dir, *extra):
        return subprocess.run(["cmake", f"-DOPENAL_SRC={source_dir}", *extra, "-P", str(PATCH)],
                              capture_output=True, text=True)

    def make_tree(self, text):
        tree = Path(tempfile.mkdtemp(prefix="openal_patch_"))
        self.addCleanup(shutil.rmtree, tree, ignore_errors=True)
        (tree / "alc").mkdir()
        (tree / "alc/alc.cpp").write_text(text, encoding="utf-8", newline="")
        return tree

    def test_the_old_adaptor_is_replaced_and_nothing_else_changes(self):
        tree = self.make_tree(SOURCE)
        result = self.run_patch(tree)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((tree / "alc/alc.cpp").read_text(encoding="utf-8"), SOURCE.replace(OLD, NEW))

    def test_running_it_twice_changes_nothing_more(self):
        tree = self.make_tree(SOURCE)
        self.assertEqual(self.run_patch(tree).returncode, 0)
        once = (tree / "alc/alc.cpp").read_bytes()
        self.assertEqual(self.run_patch(tree).returncode, 0)
        self.assertEqual((tree / "alc/alc.cpp").read_bytes(), once)

    def test_sources_without_the_line_are_left_alone(self):
        tree = self.make_tree("int main() { return 0; }\n")
        result = self.run_patch(tree)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((tree / "alc/alc.cpp").read_text(encoding="utf-8"), "int main() { return 0; }\n")

    def test_a_missing_source_directory_argument_fails(self):
        result = subprocess.run(["cmake", "-P", str(PATCH)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OPENAL_SRC", result.stderr)

    def test_the_fetch_applies_the_patch_for_msvc_only(self):
        text = (ROOT / "scripts/extern/openal.cmake").read_text(encoding="utf-8")
        self.assertIn("openal_msvc_ranges.cmake", text)
        self.assertIn("if (MSVC)\n\tset(KISAK_OPENAL_PATCH PATCH_COMMAND", text)
        self.assertIn("GIT_TAG 1.25.2", text)  # the patch is written for this tag
        self.assertIn("${KISAK_OPENAL_PATCH}", text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Self-tests for check_rhi_boundary.py.

These test the checker, not the renderer: what it counts as a Direct3D 9 call,
what it skips, and that a regression, a stale allowlist entry and a malformed
allowlist each reach the caller as the documented exit status. Source trees are
fabricated in a temporary directory; nothing is compiled. The last case runs
the checker on this checkout, which is what CI relies on.
"""

from contextlib import redirect_stdout, redirect_stderr
from importlib import util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts" / "tests" / "check_rhi_boundary.py"


def load_checker():
    spec = util.spec_from_file_location("check_rhi_boundary", CHECKER)
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = load_checker()


class Tree:
    """A fake repository with renderer sources and an allowlist."""

    def __init__(self, files, allowlist=""):
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)
        (self.root / "src" / "gfx_d3d").mkdir(parents=True)
        for name, text in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        allow = self.root / "scripts" / "tests" / "rhi_boundary_allowlist.txt"
        allow.parent.mkdir(parents=True)
        allow.write_text(allowlist, encoding="utf-8")

    def run(self, *extra):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            status = checker.main(["--root", str(self.root), *extra])
        return status, out.getvalue() + err.getvalue()

    def close(self):
        self._dir.cleanup()


class ScanText(unittest.TestCase):
    def test_counts_method_calls_through_pointers(self):
        text = "void f(){\n device->SetRenderState(D3DRS_ZENABLE, 0);\n (*fence)->Issue(1);\n}\n"
        self.assertEqual(checker.scan_text(text), [(2, "SetRenderState"), (3, "Issue")])

    def test_counts_free_functions(self):
        text = "d3d9 = Direct3DCreate9(32);\nD3DPERF_BeginEvent(0, L\"x\");\n"
        self.assertEqual(checker.scan_text(text), [(1, "Direct3DCreate9"), (2, "D3DPERF_BeginEvent")])

    def test_skips_comments_and_strings(self):
        text = (
            '// device->SetIndices(ib);\n'
            '/* dx.device->Present(0, 0, 0, 0);\n   still a comment */\n'
            'RB_LogPrint("device->SetIndices( ib )\\n");\n'
            "char c = '>';\n"
        )
        self.assertEqual(checker.scan_text(text), [])

    def test_line_numbers_survive_block_comments(self):
        text = "/*\n\n*/\nx->Release();\n"
        self.assertEqual(checker.scan_text(text), [(4, "Release")])

    def test_ignores_engine_methods_with_other_names(self):
        self.assertEqual(checker.scan_text("list->Append(item);\nstate->prim.device;\n"), [])


class Check(unittest.TestCase):
    def tree(self, files, allowlist=""):
        tree = Tree(files, allowlist)
        self.addCleanup(tree.close)
        return tree

    def test_clean_tree_passes(self):
        tree = self.tree({
            "src/gfx_d3d/r_draw.cpp": "void f(){ R_HW_DisableScissor(device); }\n",
            "src/gfx_d3d/rhi/r_rhi_d3d9_state.cpp": "void g(){ device->SetRenderState(a, 0); }\n",
        })
        status, output = tree.run()
        self.assertEqual(status, 0, output)
        self.assertIn("PASS", output)

    def test_interface_directory_is_exempt_at_any_depth(self):
        tree = self.tree({"src/gfx_d3d/rhi/d3d9/impl.cpp": "void g(){ device->Clear(0,0,0,0,0,0); }\n"})
        self.assertEqual(tree.run()[0], 0)

    def test_new_direct_call_is_a_regression(self):
        tree = self.tree({"src/gfx_d3d/rb_new.cpp": "void f(){ device->SetScissorRect(&r); }\n"})
        status, output = tree.run()
        self.assertEqual(status, 1)
        self.assertIn("REGRESSION src/gfx_d3d/rb_new.cpp", output)
        self.assertIn("1:SetScissorRect", output)

    def test_headers_are_scanned(self):
        tree = self.tree({"src/gfx_d3d/r_inline.h": "inline void f(){ t->Release(); }\n"})
        self.assertEqual(tree.run()[0], 1)

    def test_more_calls_than_allowed_is_a_regression(self):
        tree = self.tree(
            {"src/gfx_d3d/r_x.cpp": "void f(){ a->Lock(0,0,0,0); a->Unlock(); }\n"},
            "src/gfx_d3d/r_x.cpp: 1: not moved yet\n")
        status, output = tree.run()
        self.assertEqual(status, 1)
        self.assertIn("allowlist allows 1", output)

    def test_allowlisted_calls_pass(self):
        tree = self.tree(
            {"src/gfx_d3d/r_x.cpp": "void f(){ a->Lock(0,0,0,0); a->Unlock(); }\n"},
            "# comment\n\nsrc/gfx_d3d/r_x.cpp: 2: not moved yet  # trailing comment\n")
        self.assertEqual(tree.run()[0], 0)

    def test_fewer_calls_than_allowed_is_stale(self):
        tree = self.tree(
            {"src/gfx_d3d/r_x.cpp": "void f(){ a->Unlock(); }\n"},
            "src/gfx_d3d/r_x.cpp: 2: not moved yet\n")
        status, output = tree.run()
        self.assertEqual(status, 1)
        self.assertIn("STALE src/gfx_d3d/r_x.cpp", output)

    def test_entry_for_a_clean_or_missing_file_is_stale(self):
        tree = self.tree({}, "src/gfx_d3d/gone.cpp: 3: moved\n")
        status, output = tree.run()
        self.assertEqual(status, 1)
        self.assertIn("STALE src/gfx_d3d/gone.cpp", output)

    def test_malformed_allowlist_is_a_usage_error(self):
        for text in ("src/gfx_d3d/r_x.cpp: two: reason\n",
                     "src/gfx_d3d/r_x.cpp: 0: reason\n",
                     "src/gfx_d3d/r_x.cpp: 1\n",
                     "src/gfx_d3d/r_x.cpp: 1: a\nsrc/gfx_d3d/r_x.cpp: 1: b\n"):
            with self.subTest(text=text):
                tree = self.tree({"src/gfx_d3d/r_x.cpp": "void f(){ a->Unlock(); }\n"}, text)
                self.assertEqual(tree.run()[0], 2)

    def test_missing_renderer_directory_is_a_usage_error(self):
        with tempfile.TemporaryDirectory() as empty:
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(out):
                self.assertEqual(checker.main(["--root", empty]), 2)

    def test_list_prints_every_call(self):
        tree = self.tree({"src/gfx_d3d/r_x.cpp": "void f(){\n a->Lock(0,0,0,0);\n a->Unlock();\n}\n"})
        status, output = tree.run("--list")
        self.assertEqual(status, 0)
        self.assertIn("src/gfx_d3d/r_x.cpp:2: Lock", output)
        self.assertIn("src/gfx_d3d/r_x.cpp:3: Unlock", output)


class ThisCheckout(unittest.TestCase):
    def test_repository_matches_its_allowlist(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            status = checker.main([])
        self.assertEqual(status, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()

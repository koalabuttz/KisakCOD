#!/usr/bin/env python3
"""Self-tests for scripts/tools/strip_cdecl.py (upstream issue #35).

The tool rewrites hundreds of engine files in one pass, so its edge cases are
pinned here: spacing after the keyword is removed cleanly, every ABI-changing
convention is left alone, literals and prose comments are untouched, and file
bytes (CRLF, non-UTF-8) round-trip.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools" / "strip_cdecl.py"
spec = importlib.util.spec_from_file_location("strip_cdecl", TOOL)
strip_cdecl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(strip_cdecl)


def strip(text: str) -> str:
    return strip_cdecl.strip(text)[0]


class RewriteTests(unittest.TestCase):
    def test_declarations_and_definitions(self):
        cases = {
            "void __cdecl Com_Init(char *line);": "void Com_Init(char *line);",
            "char *__cdecl Cmd_Argv(int arg)\n{": "char *Cmd_Argv(int arg)\n{",
            "const char **__cdecl FS_List();": "const char **FS_List();",
            "Material* __cdecl Material_Register();": "Material* Material_Register();",
            "static int __cdecl Compare(const void *a, const void *b)": "static int Compare(const void *a, const void *b)",
            "void __cdecl EntHandle::Init()": "void EntHandle::Init()",
            "int __cdecl\nyyparse()": "int\nyyparse()",
            "void __cdecl  Twice();": "void Twice();",
            "void\t__cdecl\tTabbed();": "void\tTabbed();",
        }
        for before, after in cases.items():
            with self.subTest(before=before):
                self.assertEqual(strip(before), after)

    def test_function_pointer_types_and_casts(self):
        cases = {
            "void(__cdecl *callback)(int);": "void(*callback)(int);",
            "void (__cdecl *callback)(int);": "void (*callback)(int);",
            "void(__cdecl* callback)(int);": "void(* callback)(int);",
            "void(__cdecl * cb)(int);": "void(* cb)(int);",
            "qsort(a, n, 4, (int(__cdecl *)(const void *, const void *))Cmp);":
                "qsort(a, n, 4, (int(*)(const void *, const void *))Cmp);",
            "((void(__cdecl*)(char*))drawFunc)(text);": "((void(*)(char*))drawFunc)(text);",
            "void(__cdecl *__cdecl GetOperationFunction(int op))(int);":
                "void(*GetOperationFunction(int op))(int);",
            "XModel* (__cdecl* GetXModel)(const char*);": "XModel* (* GetXModel)(const char*);",
        }
        for before, after in cases.items():
            with self.subTest(before=before):
                self.assertEqual(strip(before), after)

    def test_identifiers_containing_the_word_are_untouched(self):
        text = "int __cdecl CG_Generic_char_const_____cdecl_int__(int);"
        self.assertEqual(strip(text), "int CG_Generic_char_const_____cdecl_int__(int);")
        self.assertEqual(strip("int my__cdecl;"), "int my__cdecl;")

    def test_other_conventions_are_kept(self):
        text = (
            "LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);\n"
            "DWORD WINAPI Thread(void *);\n"
            "void __stdcall Stdcall(int);\n"
            "void __fastcall Fast(int);\n"
            "((int(__thiscall *)(IDirect3D9 *))f)(d);\n"
            "void (__stdcall *pfn)(int);\n"
        )
        self.assertEqual(strip(text), text)

    def test_preprocessor_lines_about_the_keyword_are_kept(self):
        text = (
            "#ifndef __cdecl\n"
            "#define __cdecl __attribute__((cdecl))\n"
            "#endif\n"
            "#define\tQDECL\t__cdecl\n"
        )
        self.assertEqual(strip(text), text)

    def test_literals_are_kept(self):
        text = (
            '#error "needs Clang for __cdecl/__stdcall"\n'
            'const char *s = "void __cdecl F()";\n'
            'const char *r = R"x(int __cdecl G())x";\n'
        )
        self.assertEqual(strip(text), text)

    def test_prose_comments_are_kept_and_commented_out_code_is_cleaned(self):
        prose = (
            "// __cdecl: a1 = brush_t* (the brush DEF to save)\n"
            "// ebx; normalised to __cdecl here.)\n"
            "// __usercall hWnd@<esi> -> __cdecl.\n"
            "// Clang has __cdecl/__int32 as keywords\n"
            "// (__stdcall, __cdecl, __intN) are keywords\n"
            "// convention is what __cdecl means on Windows\n"
        )
        self.assertEqual(strip(prose), prose)
        self.assertEqual(
            strip("//std::_Sort<T *, int, bool(__cdecl *)(T const &)>(\n"),
            "//std::_Sort<T *, int, bool(*)(T const &)>(\n",
        )
        self.assertEqual(
            strip("/* int __cdecl Old(int a); */ int __cdecl New(int a);"),
            "/* int Old(int a); */ int New(int a);",
        )

    def test_braces_and_quotes_in_comments_do_not_confuse_the_scan(self):
        text = "// don't \"\nvoid __cdecl After();\n"
        self.assertEqual(strip(text), "// don't \"\nvoid After();\n")
        text = "char c = '\"'; void __cdecl F();"
        self.assertEqual(strip(text), "char c = '\"'; void F();")


class FileTests(unittest.TestCase):
    def test_write_preserves_bytes_and_check_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "unit.cpp"
            original = b"// caf\xe9\r\nvoid __cdecl F();\r\nint x;\r\n"
            source.write_bytes(original)
            untouched = root / "notes.txt"
            untouched.write_bytes(b"void __cdecl F();\n")

            self.assertEqual(strip_cdecl.main(["--check", str(root)]), 1)
            self.assertEqual(source.read_bytes(), original)

            self.assertEqual(strip_cdecl.main(["--write", str(root)]), 0)
            self.assertEqual(source.read_bytes(), b"// caf\xe9\r\nvoid F();\r\nint x;\r\n")
            self.assertEqual(untouched.read_bytes(), b"void __cdecl F();\n")

            self.assertEqual(strip_cdecl.main(["--check", str(root)]), 0)

    def test_third_party_deps_are_never_rewritten(self):
        header = strip_cdecl.REPO / "deps" / "msslib" / "mss.h"
        if not header.is_file():
            self.skipTest("deps/msslib/mss.h is not in this checkout")
        self.assertEqual(list(strip_cdecl.iter_files([str(header)])), [])


if __name__ == "__main__":
    unittest.main()

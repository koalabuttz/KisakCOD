#!/usr/bin/env python3
"""Self-test for scripts/audit/retail_float_int_scan.py on synthetic listings.

No executable is needed: the listings below are hand-written MSVC-style code
and clang i686-pc-windows-msvc output for small test functions.
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "audit"))
import retail_float_int_scan as scan  # noqa: E402

# MSVC shape: three _ftol2_sse snaps of trDelta, one fraction, then the helper
# and its x87 fallback (whose fistp must not count as a rounding site).
MSVC_LISTING = """
  401000:	push   esi
  401001:	mov    esi,DWORD PTR [esp+0x8]
  401005:	fld    DWORD PTR [esi+0x24]
  401008:	call   0x402000
  40100d:	mov    DWORD PTR [esp+0x8],eax
  401011:	fild   DWORD PTR [esp+0x8]
  401015:	fstp   DWORD PTR [esi+0x24]
  401018:	fld    DWORD PTR [esi+0x28]
  40101b:	call   0x402000
  401020:	mov    DWORD PTR [esp+0x8],eax
  401024:	fild   DWORD PTR [esp+0x8]
  401028:	fstp   DWORD PTR [esi+0x28]
  40102b:	fld    DWORD PTR [esi+0x2c]
  40102e:	call   0x402000
  401033:	mov    DWORD PTR [esp+0x8],eax
  401037:	fild   DWORD PTR [esp+0x8]
  40103b:	fstp   DWORD PTR [esi+0x2c]
  40103e:	pop    esi
  40103f:	ret
  401040:	fld    DWORD PTR [esp+0x4]
  401044:	fld    st(0)
  401046:	call   0x402000
  40104b:	mov    DWORD PTR [esp+0x4],eax
  40104f:	fild   DWORD PTR [esp+0x4]
  401053:	fsubp  st(1),st
  401055:	ret
  402000:	cmp    DWORD PTR ds:0x500000,0x0
  402007:	je     0x402030
  402009:	push   ebp
  40200a:	mov    ebp,esp
  40200c:	sub    esp,0x8
  40200f:	and    esp,0xfffffff8
  402012:	fstp   QWORD PTR [esp]
  402015:	cvttsd2si eax,QWORD PTR [esp]
  40201a:	leave
  40201b:	ret
  402030:	push   ebp
  402031:	mov    ebp,esp
  402033:	sub    esp,0x20
  402036:	fld    st(0)
  402038:	fst    DWORD PTR [esp+0x18]
  40203c:	fistp  QWORD PTR [esp+0x10]
  402040:	fild   QWORD PTR [esp+0x10]
  402044:	fsubp  st(1),st
  402046:	leave
  402047:	ret
"""

# clang -O2 -mno-sse: truncating casts go through fldcw; an inline-asm fistp
# snap (Sys_SnapVector shape) spills through a stack temporary first.
CLANG_X87_LISTING = """
   0:	sub    esp,0x24
   3:	mov    eax,DWORD PTR [esp+0x28]
   7:	fld    DWORD PTR [eax+0x24]
   a:	fnstcw WORD PTR [esp+0x4]
   e:	movzx  ecx,WORD PTR [esp+0x4]
  13:	or     ecx,0xc00
  19:	mov    WORD PTR [esp+0xa],cx
  1e:	fldcw  WORD PTR [esp+0xa]
  22:	fistp  DWORD PTR [esp+0x14]
  26:	fldcw  WORD PTR [esp+0x4]
  2a:	mov    ecx,DWORD PTR [esp+0x14]
  2e:	mov    DWORD PTR [esp+0x20],ecx
  32:	fild   DWORD PTR [esp+0x20]
  36:	fstp   DWORD PTR [eax+0x24]
  39:	fld    DWORD PTR [eax+0x28]
  3c:	fnstcw WORD PTR [esp+0x2]
  40:	movzx  ecx,WORD PTR [esp+0x2]
  45:	or     ecx,0xc00
  4b:	mov    WORD PTR [esp+0x8],cx
  50:	fldcw  WORD PTR [esp+0x8]
  54:	fistp  DWORD PTR [esp+0xc]
  58:	fldcw  WORD PTR [esp+0x2]
  5c:	mov    ecx,DWORD PTR [esp+0xc]
  60:	mov    DWORD PTR [esp+0x18],ecx
  64:	fild   DWORD PTR [esp+0x18]
  68:	fstp   DWORD PTR [eax+0x28]
  6b:	fld    DWORD PTR [eax+0x2c]
  6e:	fnstcw WORD PTR [esp]
  71:	movzx  ecx,WORD PTR [esp]
  75:	or     ecx,0xc00
  7b:	mov    WORD PTR [esp+0x6],cx
  80:	fldcw  WORD PTR [esp+0x6]
  84:	fistp  DWORD PTR [esp+0x10]
  88:	fldcw  WORD PTR [esp]
  8b:	mov    ecx,DWORD PTR [esp+0x10]
  8f:	mov    DWORD PTR [esp+0x1c],ecx
  93:	fild   DWORD PTR [esp+0x1c]
  97:	fstp   DWORD PTR [eax+0x2c]
  9a:	add    esp,0x24
  9d:	ret
  9e:	nop
  9f:	nop
  a0:	sub    esp,0x14
  a3:	mov    eax,DWORD PTR [esp+0x18]
  a7:	fld    DWORD PTR [eax]
  a9:	fstp   DWORD PTR [esp]
  ac:	fld    DWORD PTR [esp]
  af:	fistp  DWORD PTR [esp+0x4]
  b3:	mov    ecx,DWORD PTR [esp+0x4]
  b7:	mov    DWORD PTR [esp+0x8],ecx
  bb:	fild   DWORD PTR [esp+0x8]
  bf:	fstp   DWORD PTR [eax]
  c1:	fld    DWORD PTR [eax+0x4]
  c4:	fstp   DWORD PTR [esp]
  c7:	fld    DWORD PTR [esp]
  ca:	fistp  DWORD PTR [esp+0x4]
  ce:	mov    ecx,DWORD PTR [esp+0x4]
  d2:	mov    DWORD PTR [esp+0xc],ecx
  d6:	fild   DWORD PTR [esp+0xc]
  da:	fstp   DWORD PTR [eax+0x4]
  dd:	fld    DWORD PTR [eax+0x8]
  e0:	fstp   DWORD PTR [esp]
  e3:	fld    DWORD PTR [esp]
  e6:	fistp  DWORD PTR [esp+0x4]
  ea:	mov    ecx,DWORD PTR [esp+0x4]
  ee:	mov    DWORD PTR [esp+0x10],ecx
  f2:	fild   DWORD PTR [esp+0x10]
  f6:	fstp   DWORD PTR [eax+0x8]
  f9:	add    esp,0x14
  fc:	ret
  fd:	nop
  fe:	nop
  ff:	nop
 100:	sub    esp,0xc
 103:	fld    DWORD PTR [esp+0x10]
 107:	fnstcw WORD PTR [esp]
 10a:	movzx  eax,WORD PTR [esp]
 10e:	or     eax,0xc00
 113:	mov    WORD PTR [esp+0x2],ax
 118:	fldcw  WORD PTR [esp+0x2]
 11c:	fist   DWORD PTR [esp+0x4]
 120:	fldcw  WORD PTR [esp]
 123:	mov    eax,DWORD PTR [esp+0x4]
 127:	mov    DWORD PTR [esp+0x8],eax
 12b:	fisub  DWORD PTR [esp+0x8]
 12f:	add    esp,0xc
 132:	ret
"""


class ScanTest(unittest.TestCase):
    def test_msvc_helper_snaps_truncate(self):
        report = scan.scan(scan.parse_objdump(MSVC_LISTING))
        self.assertEqual(dict(report.kinds), {"call-ftol": 4})
        self.assertEqual([(a, k) for a, k, _ in report.helpers], [(0x402000, "cvtt"), (0x402030, "fallback")])
        self.assertEqual(len(report.triples), 1)
        self.assertEqual(report.triples[0][0].src, ("esi", 0x24))
        self.assertEqual(report.triples[0][0].dst, ("esi", 0x24))
        ops = sorted(s.next_op for s in report.sites)
        self.assertEqual(ops, ["fstp", "fstp", "fstp", "fsubp"])

    def test_x87_rounding_and_control_word_truncation(self):
        report = scan.scan(scan.parse_objdump(CLANG_X87_LISTING))
        self.assertEqual(report.kinds["fistp"], 3)
        self.assertEqual(report.kinds["fistp-cw"], 4)
        kinds = {t[0].src: t[0].kind for t in report.triples}
        self.assertEqual(kinds, {("eax", 0x24): "fistp-cw", ("eax", 0): "fistp"})
        self.assertIn(("fistp-cw", "fisub"), {(s.kind, s.next_op) for s in report.sites})

    def test_memory_operand_parsing(self):
        self.assertEqual(scan.parse_mem("DWORD PTR [esi+eax*4+0x3c]"), ("esi+eax*4", 0x3C))
        self.assertEqual(scan.parse_mem("DWORD PTR ds:0x946428"), ("abs", 0x946428))
        self.assertEqual(scan.parse_mem("DWORD PTR [ebp-0x10]"), ("ebp", -0x10))

    def test_report_prints_no_bytes(self):
        text = scan.format_report(scan.scan(scan.parse_objdump(MSVC_LISTING)), 10)
        self.assertIn("call-ftol", text)
        self.assertNotIn("ds:0x500000", text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Check how KisakCOD's bots throw grenades, on the host.

``src/bots/bot_grenades.h`` finds the view pitch whose arc comes down where a
bot aims, the way the game throws a grenade (docs/bots.md). It makes no engine
call, so this test builds ``tests/bots/bot_grenades_test.cpp`` with the host
compiler and runs it: the flattest throw that gets there, higher for further,
lower for a target below, none past the weapon's reach, and the arc ends at
the target.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_grenades.py [--cxx c++] [--m32]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_bot_motor  # noqa: E402

SOURCE = test_bot_motor.ROOT / "tests" / "bots" / "bot_grenades_test.cpp"

if __name__ == "__main__":
    sys.exit(test_bot_motor.main("test_bot_grenades", SOURCE, __doc__,
                                 "the bot grenades hold on the host"))

#!/usr/bin/env python3
"""Check KisakCOD's bot perception on the host.

``src/bots/bot_perception.h`` decides what a multiplayer bot knows of the
other players from what its eyes and ears get (docs/bots.md). It makes no
engine call, so this test builds ``tests/bots/bot_perception_test.cpp`` with
the host compiler and runs it: nobody off the screen, behind cover or too far
is noticed; nobody is noticed faster than the human floor over 2000 random
runs; distance, the screen's edge, stillness, crouching and cover slow
noticing; a lost player is remembered, then forgotten; sounds are heard in
range and only roughly.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_perception.py [--cxx c++] [--m32]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_bot_motor  # noqa: E402

SOURCE = test_bot_motor.ROOT / "tests" / "bots" / "bot_perception_test.cpp"

if __name__ == "__main__":
    sys.exit(test_bot_motor.main("test_bot_perception", SOURCE, __doc__,
                                 "the bot perception holds on the host"))

#!/usr/bin/env python3
"""Check how KisakCOD's bots decide what to do, on the host.

``src/bots/bot_mind.h`` scores a multiplayer bot's intents (roam, hunt,
fight, take cover) from what it knows (docs/bots.md). It makes no engine
call, so this test builds ``tests/bots/bot_mind_test.cpp`` with the host
compiler and runs it: a bot fights what it sees, takes cover to reload or
heal only when it knows some, hunts a lead, roams otherwise, leans with its
personality and does not flip back and forth; it roams to places it has not
seen and where enemies were; it pre-aims the corner of its way.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_mind.py [--cxx c++] [--m32]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_bot_motor  # noqa: E402

SOURCE = test_bot_motor.ROOT / "tests" / "bots" / "bot_mind_test.cpp"

if __name__ == "__main__":
    sys.exit(test_bot_motor.main("test_bot_mind", SOURCE, __doc__,
                                 "the bot mind holds on the host"))

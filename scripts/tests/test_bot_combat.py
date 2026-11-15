#!/usr/bin/env python3
"""Check how KisakCOD's bots fight, on the host.

``src/bots/bot_combat.h`` is a multiplayer bot's hand and trigger
(docs/bots.md). It makes no engine call, so this test builds
``tests/bots/bot_combat_test.cpp`` with the host compiler and runs it: the
hand waits for its reaction onset, never turns faster than its top speed,
lands a big flick off and closes in with corrections, trails a strafing
player; a semi-automatic is clicked at a human pace, an automatic held or
fired in bursts; the view kicks when the bot fires, as a player's client
kicks it, and the hand pulls back only part of it, late.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_combat.py [--cxx c++] [--m32]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_bot_motor  # noqa: E402

SOURCE = test_bot_motor.ROOT / "tests" / "bots" / "bot_combat_test.cpp"

if __name__ == "__main__":
    sys.exit(test_bot_motor.main("test_bot_combat", SOURCE, __doc__,
                                 "the bot combat holds on the host"))

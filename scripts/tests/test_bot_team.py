#!/usr/bin/env python3
"""Check how KisakCOD's bots play as a team, on the host.

``src/bots/bot_team.h`` holds a team's callouts and its commander, and
``src/bots/bot_spots.h`` the spots of a map and which see each other
(docs/bots.md). They make no engine call, so this test builds
``tests/bots/bot_team_test.cpp`` with the host compiler and runs it: callouts
are heard late, rounded and only for a while; objectives are read off the
compass icons; roles fit the team's size, the objectives and the bots'
leanings; the spot table is filled a step at a time and finds cover from a
threat and places to watch a target from.

Built 64-bit with AddressSanitizer and UBSan when available, and 32-bit
(``--m32`` makes the 32-bit build a requirement), as the i386 game builds it.

Usage:
    python3 scripts/tests/test_bot_team.py [--cxx c++] [--m32]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_bot_motor  # noqa: E402

SOURCE = test_bot_motor.ROOT / "tests" / "bots" / "bot_team_test.cpp"

if __name__ == "__main__":
    sys.exit(test_bot_motor.main("test_bot_team", SOURCE, __doc__,
                                 "the bot team holds on the host"))

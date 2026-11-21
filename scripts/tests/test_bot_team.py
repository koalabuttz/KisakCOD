#!/usr/bin/env python3
"""Check how KisakCOD's bots play as a team, on the host.

``src/bots/bot_team.h`` holds a team's callouts and its commander, and
``src/bots/bot_spots.h`` the spots of a map and which see each other
(docs/bots.md). They make no engine call, so this test builds
``tests/bots/bot_team_test.cpp`` with the host compiler and runs it: callouts
are heard late, rounded and only for a while; objectives are read off the
compass icons; roles fit the team's size, the objectives and the bots'
leanings; the spot table is filled a step at a time and finds cover from a
threat and places to watch a target from; a person's name never shows with
the bots' "[BOT]" tag, and the server looks at the whole name for it.

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

SERVER = test_bot_motor.ROOT / "src" / "server_mp" / "sv_client_mp.cpp"


def check_server_reads_whole_name():
    """SV_KeepBotTagForBots hands the whole name to the checked step.

    The host test checks PersonNameWithoutBotTag with whole names; a server
    that looked at a short copy would let through a tag that comes after 64
    colour codes, which the game does not show."""
    text = SERVER.read_text(encoding="utf-8", errors="replace")
    start = text.find("void SV_KeepBotTagForBots(client_t *cl)\n{")
    end = text.find("\n}\n", start)
    if start < 0 or end < 0:
        return "SV_KeepBotTagForBots was not found in sv_client_mp.cpp"
    body = text[start:end]
    if "char name[sizeof(cl->userinfo)];" not in body:
        return "SV_KeepBotTagForBots must copy the name into a buffer as large as the userinfo"
    if 'I_strncpyz(name, Info_ValueForKey(cl->userinfo, "name"), sizeof(name));' not in body:
        return "SV_KeepBotTagForBots must copy the whole name value"
    if "kisak::bots::PersonNameWithoutBotTag(name, kept, sizeof(kept))" not in body:
        return "SV_KeepBotTagForBots must ask PersonNameWithoutBotTag, the step the host test checks"
    if "WearsBotTag" in body or "RemoveBotTag" in body:
        return "SV_KeepBotTagForBots must not look for the tag by itself"
    return None


if __name__ == "__main__":
    problem = check_server_reads_whole_name()
    if problem:
        print(f"test_bot_team: {problem}", file=sys.stderr)
        sys.exit(1)
    print("PASS: the server hands a person's whole name to the bot tag check")
    sys.exit(test_bot_motor.main("test_bot_team", SOURCE, __doc__,
                                 "the bot team holds on the host"))

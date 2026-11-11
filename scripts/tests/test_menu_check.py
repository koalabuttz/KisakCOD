#!/usr/bin/env python3
"""Host test for scripts/runs/menu_check.py: no game, no display.

Checks the parts of the menu check that need neither: the parser of the
client's ``listmenuitems`` answer, against the format strings the client prints
it with (read from src/ui_mp/ui_main_mp.cpp, so the two cannot drift apart),
the item matching, and the key helpers. The clicking itself needs a game and
runs on a test machine (docs/runs.md).
"""

from __future__ import annotations

import os
import re
import sys
import tempfile
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "runs"))
import menu_check  # noqa: E402

failures = 0
checks = 0


def check(condition, label):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL  {label}")


def c_format(fmt, values):
    """printf-style rendering of the client's own format string (%s, %i, %.0f)."""
    out = []
    pos = 0
    values = list(values)
    for match in re.finditer(r"%(\.0f|i|s)", fmt):
        out.append(fmt[pos:match.start()])
        value = values.pop(0)
        out.append(f"{value:.0f}" if match[1] == ".0f" else str(value))
        pos = match.end()
    out.append(fmt[pos:])
    check(not values, "every value was used by the format")
    return "".join(out)


def client_formats():
    source = (ROOT / "src" / "ui_mp" / "ui_main_mp.cpp").read_text(encoding="utf-8", errors="replace")
    body = source[source.index("void UI_ListMenuItems_f()"):]
    body = body[:body.index("\n}\n") if "\n}\n" in body else body.index("\r\n}\r\n")]
    literals = {}
    # Adjacent string literals in one Com_Printf call are joined, as C does.
    for call in re.finditer(r"Com_Printf\(CON_CHANNEL_DONT_FILTER,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)", body):
        text = "".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"", call[1]))
        text = text.replace('\\"', '"').replace("\\n", "")
        literals[text.split(" ", 1)[0]] = text
    return literals


def test_format_contract():
    formats = client_formats()
    check(set(formats) >= {"menustack", "menuitem", "menurow", "menuend"},
          f"the client prints the four line kinds (found {sorted(formats)})")
    stack = c_format(formats["menustack"], [2, "pc_join_unranked", 640, 360])
    item = c_format(formats["menuitem"], ["pc_join_unranked", 7, 1, "refresh", 1, 100, 600, 120, 30, 160, 615,
                                         "-", "Refresh List", "uiScript RefreshServers"])
    row = c_format(formats["menurow"], ["pc_join_unranked", 12, 0, 1, 220, 140,
                                        "KisakCOD menu check|Crash|0/24|12"])
    listing = menu_check.parse_listing([stack, item, row, "menuend"])
    check(listing["count"] == 2 and listing["top"] == "pc_join_unranked", "the stack line parses")
    check(len(listing["items"]) == 1, "the item line parses")
    if listing["items"]:
        parsed = listing["items"][0]
        check(parsed["click"] == (160, 615) and parsed["visible"] and parsed["item"] == 7, "item fields")
        check(parsed["action"] == "uiScript RefreshServers" and parsed["text"] == "Refresh List", "item text and action")
    check(len(listing["rows"]) == 1, "the row line parses")
    if listing["rows"]:
        parsed = listing["rows"][0]
        check(parsed["selected"] and parsed["click"] == (220, 140) and parsed["row"] == 0, "row fields")
    check(menu_check.rows_naming(listing, "kisakcod MENU check"), "rows are found by server name, any case")


def test_matching():
    lines = [
        "menustack count=1 top=main cursor=0,0",
        'menuitem menu=main item=0 type=1 name=join visible=1 rect=0,0,10,10 click=5,5 dvar=- '
        'text="^3Join Game" action="play mouse_click; open pc_join_unranked"',
        'menuitem menu=main item=1 type=1 name=hidden visible=0 rect=0,0,10,10 click=5,15 dvar=- '
        'text="Join Game" action="open pc_join_unranked"',
        'menuitem menu=main item=2 type=4 name=- visible=1 rect=0,0,10,10 click=5,25 dvar=cdkey1 text="" action=""',
        'menuitem menu=main item=3 type=4 name=- visible=1 rect=0,0,10,10 click=5,35 dvar=cdkey10 text="" action=""',
    ]
    listing = menu_check.parse_listing(lines)
    found = menu_check.find_items(listing, action=r"pc_join_unranked")
    check([item["item"] for item in found] == [0], "hidden items are not matched")
    check(menu_check.find_items(listing, text=r"^join game$"), "text matches without colour codes")
    fields = menu_check.find_items(listing, dvar=r"cdkey[1-5]")
    check([item["item"] for item in fields] == [2], "a dvar pattern matches the whole name")
    check(menu_check.find_items(listing, action=r"pc_join", visible_only=False).__len__() == 2,
          "visible_only=False includes hidden items")
    named = menu_check.parse_listing([
        "menustack count=1 top=pc_join_unranked cursor=0,0",
        'menuitem menu=pc_join_unranked item=38 type=8 name=sourcefield visible=1 rect=0,0,10,10 click=680,14 '
        'dvar=- text="" action="play mouse_click"',
        'menuitem menu=pc_join_unranked item=43 type=1 name=refreshFilter visible=1 rect=0,0,10,10 click=1032,44 '
        'dvar=- text="Quick Refresh" action="uiScript RefreshFilter"',
    ])
    found = menu_check.find_items(named, name=r"source(field)?")
    check([item["item"] for item in found] == [38], "the source control is found by its name, not Quick Refresh")
    check(menu_check.parse_listing(["menuitem broken"])["items"] == [], "a broken line is ignored")


def test_key_helpers():
    parts = menu_check.split_key(menu_check.TEST_KEY)
    check(len(parts) == 5 and all(len(part) == 4 for part in parts), "the key splits into five fields of four")
    check("".join(parts) == re.sub(r"-", "", menu_check.TEST_KEY), "the fields hold the whole key")
    wrong = menu_check.wrong_checksum(menu_check.TEST_KEY)
    check(len(wrong) == 20 and wrong[:-1] == "".join(parts).upper()[:-1] and wrong[-1] != parts[-1][-1],
          "the mistyped key differs only in its last character")
    try:
        menu_check.split_key("ABCD-EFGH")
        check(False, "a short key is refused")
    except ValueError:
        check(True, "a short key is refused")


def test_dvar_pattern():
    pattern = re.compile(menu_check.DVAR_RE.format(name=re.escape("ui_netSource")), re.I)
    match = pattern.search('"ui_netSource" is: "2^7" default: "1^7"')
    check(match is not None and match["value"] == "2", "an integer dvar's value is read without its colour code")
    pattern = re.compile(menu_check.DVAR_RE.format(name=re.escape("ui_cdkeyvalid")), re.I)
    match = pattern.search('"ui_cdkeyvalid" is: "Key Code is valid^7" default: "^7"')
    check(match is not None and match["value"] == "Key Code is valid", "a text dvar's value is read")
    match = pattern.search('"ui_cdkeyvalid" is: "^7" default: "^7"')
    check(match is not None and match["value"] == "", "an empty dvar reads as empty")


def test_profile_seed():
    with tempfile.TemporaryDirectory() as work:
        home = Path(work) / "client-home"
        menu_check.seed_profile(home)
        profiles = home / "players" / "profiles"
        check((profiles / menu_check.PROFILE).is_dir(), "the profile folder is made under players/profiles")
        check((profiles / "active.txt").read_text().split() == [menu_check.PROFILE], "active.txt names it")
        (profiles / "active.txt").write_text("other\n")
        menu_check.seed_profile(home)
        check((profiles / "active.txt").read_text() == "other\n", "a second start keeps the active profile")


FAKE_XVFB = textwrap.dedent("""\
    #!/usr/bin/env python3
    # Like Xvfb in WSL: -displayfd reports nothing, an explicit :N works.
    import os, signal, sys, time
    if "-displayfd" in sys.argv:
        sys.exit(1)
    with open(os.environ["FAKE_X_STATE"], "w") as state:
        state.write(sys.argv[1].lstrip(":"))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        time.sleep(1)
    """)

FAKE_XDOTOOL = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    try:
        live = open(os.environ["FAKE_X_STATE"]).read()
    except OSError:
        live = ""
    sys.exit(0 if os.environ.get("DISPLAY", "") == ":" + live and live else 1)
    """)


def test_display_fallback():
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        for name, text in (("Xvfb", FAKE_XVFB), ("xdotool", FAKE_XDOTOOL)):
            (work / name).write_text(text)
            (work / name).chmod(0o755)
        os.environ["FAKE_X_STATE"] = str(work / "state")
        display = None
        try:
            display = menu_check.Display(str(work / "Xvfb"), str(work / "xdotool"), 640, 480, work / "xvfb.log")
            number = display.name.lstrip(":")
            check(number.isdigit() and display.env["DISPLAY"] == display.name,
                  f"an explicit display is used when -displayfd reports none (got {display.name!r})")
            check(display.process.poll() is None, "the fallback display is running")
        except RuntimeError as error:
            check(False, f"an explicit display is used when -displayfd reports none ({error})")
        finally:
            if display:
                display.close()
                check(display.process.poll() is not None, "close stops the display")
            os.environ.pop("FAKE_X_STATE", None)


def main():
    test_format_contract()
    test_matching()
    test_key_helpers()
    test_dvar_pattern()
    test_profile_seed()
    test_display_fallback()
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

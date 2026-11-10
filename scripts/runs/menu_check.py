#!/usr/bin/env python3
"""Click through the multiplayer menus of the native client and check what they show.

Not a CI check: it needs a Call of Duty 4 install, so it runs on a machine that
has one (docs/runs.md), on a private X display (Xvfb) where nothing is seen and
nobody has to sit at the screen. It starts a dedicated server and the client,
reads the open menus with the client's ``listmenuitems`` console command, and
clicks and types with xdotool, the way a player would:

1. the key code menu: a mistyped key is refused and saves nothing; the made-up
   test key is accepted and saved as codkey.txt (mode 600) in the client's home;
2. Join Game, source Local, Refresh: the server is listed by name;
3. the server selected, Add to Favourites; source Favourites, Refresh: listed;
4. Join from that list: the server sees the player connect (with the key code
   saved in step 1), and the client disconnects again;
5. the client restarted: Join Game, Favourites, Refresh: still listed.

Writes ``<out>/results.json`` (one entry per check), ``<out>/client*.log``,
``<out>/server.log`` and ``<out>/xvfb.log``. Exit status 0 when every check
passed, 1 when one failed, 2 on a usage or set-up error.

The key typed is the project's made-up test key unless ``--key`` names another;
it is only ever typed into the menu and compared with the saved file, never
printed or logged. Use a made-up key: the logs and the home folder stay on the
machine, but nothing here needs a real one.

Example (WSL on the PC)::

    python3 scripts/runs/menu_check.py --client build/bin/KisakCOD-mp \\
        --server build/bin/KisakCOD-dedi --game-dir ~/cod4 --out ~/runs/menu-1

Menus are found by what they do, not by their place on the screen: an item is
matched by its action script (``open pc_join_unranked``, ``uiScript
RefreshServers``, ...), its dvar or its text, so a different resolution or menu
layout does not break it. When something is not found the run stops that check
and writes the last menu listing into results.json, which says what was there.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import shlex
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from perf_run import Game  # noqa: E402  (the same stdin/stdout wrapper as frame-rate runs)

TEST_KEY = "K1SAK-C0DTE-STKEY-98B44"
HOSTNAME = "KisakCOD menu check"
PROFILE = "menucheck"

ITEM_RE = re.compile(
    r'^menuitem menu=(?P<menu>\S+) item=(?P<item>\d+) type=(?P<type>-?\d+) name=(?P<name>\S+) '
    r'visible=(?P<visible>[01]) rect=(?P<rect>\S+) click=(?P<cx>-?\d+),(?P<cy>-?\d+) '
    r'dvar=(?P<dvar>\S+) text="(?P<text>[^"]*)" action="(?P<action>[^"]*)"$')
ROW_RE = re.compile(
    r'^menurow menu=(?P<menu>\S+) item=(?P<item>\d+) row=(?P<row>\d+) selected=(?P<selected>[01]) '
    r'click=(?P<cx>-?\d+),(?P<cy>-?\d+) text="(?P<text>[^"]*)"$')
STACK_RE = re.compile(r'^menustack count=(?P<count>\d+) top=(?P<top>\S+) cursor=\S+$')
DVAR_RE = '^"{name}" is: "(?P<value>[^"]*?)(?:\\^7)?"'
COLOR_RE = re.compile(r'\^\d')


def plain(text: str) -> str:
    """Menu text without colour codes, for matching."""
    return COLOR_RE.sub("", text).strip()


def parse_listing(lines):
    """The lines of one ``listmenuitems`` answer, as a dict."""
    listing = {"count": 0, "top": "-", "items": [], "rows": []}
    for line in lines:
        match = STACK_RE.match(line)
        if match:
            listing["count"] = int(match["count"])
            listing["top"] = match["top"]
            continue
        match = ITEM_RE.match(line)
        if match:
            item = match.groupdict()
            item["item"] = int(item["item"])
            item["visible"] = item["visible"] == "1"
            item["click"] = (int(item.pop("cx")), int(item.pop("cy")))
            listing["items"].append(item)
            continue
        match = ROW_RE.match(line)
        if match:
            row = match.groupdict()
            row["item"] = int(row["item"])
            row["row"] = int(row["row"])
            row["selected"] = row["selected"] == "1"
            row["click"] = (int(row.pop("cx")), int(row.pop("cy")))
            listing["rows"].append(row)
    return listing


def find_items(listing, action=None, text=None, dvar=None, menu=None, visible_only=True):
    """Items of the listing that match every pattern given (case-insensitive regexes)."""
    found = []
    for item in listing["items"]:
        if visible_only and not item["visible"]:
            continue
        if menu is not None and item["menu"] != menu:
            continue
        if action is not None and not re.search(action, item["action"], re.I):
            continue
        if text is not None and not re.search(text, plain(item["text"]), re.I):
            continue
        if dvar is not None and not re.fullmatch(dvar, item["dvar"], re.I):
            continue
        found.append(item)
    return found


def rows_naming(listing, name):
    return [row for row in listing["rows"] if name.lower() in plain(row["text"]).lower()]


def split_key(key: str):
    """The key as the menu's five fields of four take it."""
    bare = re.sub(r"[^0-9A-Za-z]", "", key)
    if len(bare) != 20:
        raise ValueError("a key code is 20 letters and digits")
    return [bare[i:i + 4] for i in range(0, 20, 4)]


def wrong_checksum(key: str) -> str:
    """The same key with its last character changed, so its checksum no longer matches."""
    bare = re.sub(r"[^0-9A-Za-z]", "", key).upper()
    last = "0" if bare[-1] != "0" else "1"
    return bare[:-1] + last


def seed_profile(home: Path, name: str = PROFILE):
    """A new home has no player profile, and the menus then open "You need a
    player profile" pop-ups over everything. Gives the home one, as the game
    would after the player made it: players/profiles/<name>/ and active.txt."""
    profiles = home / "players" / "profiles"
    (profiles / name).mkdir(parents=True, exist_ok=True)
    active = profiles / "active.txt"
    if not active.exists():
        active.write_text(name + "\n")


# --- the private display --------------------------------------------------------------------------

class Display:
    def __init__(self, xvfb, xdotool, width, height, log_path):
        self.xdotool = xdotool
        self.log = open(log_path, "w")
        screen = ["-screen", "0", f"{width}x{height}x24", "-nolisten", "tcp"]
        number = self._start_with_displayfd(xvfb, screen)
        if not number:
            # In WSL /tmp/.X11-unix is a read-only mount: -displayfd finds no
            # display it can make a socket for and reports nothing, while an
            # explicit number still works (abstract socket).
            number = self._start_with_number(xvfb, screen)
        if not number:
            raise RuntimeError("Xvfb did not start a display (see xvfb.log)")
        self.name = f":{number}"
        self.env = dict(os.environ, DISPLAY=self.name)
        self.window = None
        self.pid = None
        self.origin = (0, 0)
        self.size = (0, 0)

    def _start_with_displayfd(self, xvfb, screen):
        read_fd, write_fd = os.pipe()
        self.process = subprocess.Popen([xvfb, "-displayfd", str(write_fd), *screen],
                                        pass_fds=(write_fd,), stdout=self.log, stderr=subprocess.STDOUT)
        os.close(write_fd)
        number = ""
        if select.select([read_fd], [], [], 15)[0]:
            number = os.read(read_fd, 64).decode().strip()
        os.close(read_fd)
        if number.isdigit() and self.process.poll() is None:
            return number
        self._stop_process()
        self.log.write("\n[menu_check] -displayfd reported no display; trying explicit numbers\n")
        self.log.flush()
        return None

    def _start_with_number(self, xvfb, screen):
        for number in range(90, 200):
            if Path(f"/tmp/.X{number}-lock").exists() or Path(f"/tmp/.X11-unix/X{number}").exists():
                continue
            if self._display_answers(number):
                continue  # someone else's display
            self.process = subprocess.Popen([xvfb, f":{number}", *screen],
                                            stdout=self.log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and self.process.poll() is None:
                if self._display_answers(number):
                    return str(number)
                time.sleep(0.5)
            self._stop_process()
        return None

    def _display_answers(self, number):
        try:
            result = subprocess.run([self.xdotool, "getdisplaygeometry"], env=dict(os.environ, DISPLAY=f":{number}"),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        except subprocess.TimeoutExpired:
            return False
        return result.returncode == 0

    def _stop_process(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def xdo(self, *args, check=True):
        result = subprocess.run([self.xdotool, *map(str, args)], env=self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        if check and result.returncode != 0:
            raise RuntimeError(f"xdotool {args[0]} failed: {result.stdout.strip()}")
        return result.stdout.strip()

    def find_window(self, pid, timeout):
        """Waits for the client's window. The client opens short-lived windows
        first (a 1x1 probe, a splash), so the game's window is the largest one
        it shows, and it is looked up again before every action (focus)."""
        self.pid = pid
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._pick_window() and self.size[0] >= 320:
                return True
            time.sleep(1)
        return False

    def _pick_window(self):
        out = self.xdo("search", "--onlyvisible", "--pid", self.pid, check=False)
        best = None
        for wid in (line.strip() for line in out.splitlines()):
            if not wid.isdigit():
                continue
            geometry = self.xdo("getwindowgeometry", wid, check=False)
            pos = re.search(r"Position: (-?\d+),(-?\d+)", geometry)
            size = re.search(r"Geometry: (\d+)x(\d+)", geometry)
            if not pos or not size:
                continue
            area = int(size[1]) * int(size[2])
            if best is None or area > best[0]:
                best = (area, wid, (int(pos[1]), int(pos[2])), (int(size[1]), int(size[2])))
        if best is None:
            return False
        _, self.window, self.origin, self.size = best
        return True

    def focus(self):
        """Gives the game's window the keyboard focus. With no window manager
        nothing else does, and the client drops mouse and keys without it.
        True when the X server says the window has it."""
        for _ in range(5):
            self._pick_window()
            if self.window is None:
                time.sleep(0.5)
                continue
            if self.xdo("getwindowfocus", "-f", check=False) == self.window:
                return True
            self.xdo("windowfocus", "--sync", self.window, check=False)
            time.sleep(0.3)  # SDL takes the focus change on its next pump
            if self.xdo("getwindowfocus", "-f", check=False) == self.window:
                return True
        return False

    def click(self, point):
        self.focus()
        x, y = point
        self.xdo("mousemove", self.origin[0] + x, self.origin[1] + y)
        time.sleep(0.4)  # the menu takes focus from the mouse move first
        self.xdo("click", "1")
        time.sleep(0.6)

    def type_text(self, text):
        self.focus()
        self.xdo("type", "--delay", "60", text)
        time.sleep(0.3)

    def key(self, name, times=1):
        self.focus()
        for _ in range(times):
            self.xdo("key", name)
            time.sleep(0.15)

    def close(self):
        self._stop_process()
        self.log.close()


# --- the run -------------------------------------------------------------------------------------

class Run:
    def __init__(self, args):
        self.args = args
        self.out = args.out
        self.results = {"checks": [], "notes": []}
        self.display = None
        self.server = None
        self.client = None
        self.client_launches = 0
        self.last_listing = None

    # results

    def check(self, name, passed, detail=""):
        entry = {"check": name, "result": "pass" if passed else "FAIL", "detail": detail}
        if not passed and self.last_listing is not None:
            entry["menus"] = self.last_listing
        self.results["checks"].append(entry)
        print(f"{'PASS' if passed else 'FAIL'}  {name}" + (f": {detail}" if detail else ""), flush=True)
        return passed

    def note(self, text):
        self.results["notes"].append(text)
        print(f"note  {text}", flush=True)

    # the game

    def common_args(self, home):
        args = ["+set", "fs_basepath", str(self.args.game_dir), "+set", "fs_homepath", str(home)]
        return args

    def start_server(self):
        home = self.out / "server-home"
        home.mkdir(parents=True, exist_ok=True)
        command = [str(self.args.server), *self.common_args(home),
                   "+set", "net_port", "28960", "+set", "dedicated", "2",
                   "+set", "sv_allowUnverifiedGuids", "1", "+set", "sv_hostname", HOSTNAME,
                   "+set", "sv_maprotation", "gametype war map mp_crash",
                   *shlex.split(self.args.server_args), "+map", "mp_crash"]
        self.server = Game(command, home, dict(os.environ), self.out / "server.log")
        if not self.wait_for_server(self.args.server_timeout):
            raise RuntimeError("the server did not answer getinfo on 127.0.0.1:28960")

    def wait_for_server(self, timeout):
        import socket
        deadline = time.monotonic() + timeout
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(1)
            while time.monotonic() < deadline and self.server.alive():
                self.server.wait_quiet(0.5, 1.5)
                try:
                    sock.sendto(b"\xff\xff\xff\xffgetinfo menucheck", ("127.0.0.1", 28960))
                    data, _ = sock.recvfrom(4096)
                    if b"infoResponse" in data:
                        return True
                except OSError:
                    pass
        return False

    def start_client(self):
        self.client_launches += 1
        home = self.out / "client-home"
        seed_profile(home)
        env = dict(self.display.env, ALSOFT_DRIVERS="null")
        command = [str(self.args.client), *self.common_args(home),
                   "+set", "com_introPlayed", "1", "+set", "snd_volume", "0", "+set", "developer", "1",
                   "+set", "r_fullscreen", "0", *shlex.split(self.args.client_args)]
        log = self.out / f"client-{self.client_launches}.log"
        self.client = Game(command, home, env, log)
        if not self.display.find_window(self.client.process.pid, self.args.client_timeout):
            raise RuntimeError("no client window appeared on the private display")
        return self.wait_for_menu(self.args.client_timeout)

    def listing(self):
        """Asks the client for the open menus and parses the answer."""
        self.client.send("listmenuitems")
        lines = []
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = self.client._take(0.5)
            if line is None:
                if not self.client.alive():
                    break
                continue
            if line.startswith(("menustack ", "menuitem ", "menurow ")):
                lines.append(line)
            elif line == "menuend":
                break
        self.last_listing = lines
        return parse_listing(lines)

    def wait_for_menu(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.client.alive():
            listing = self.listing()
            if listing["count"] > 0 and any(item["visible"] for item in listing["items"]):
                return listing
            time.sleep(2)
        return None

    def dvar(self, name):
        self.client.send(name)
        match = self.client.wait_for(re.compile(DVAR_RE.format(name=re.escape(name)), re.I), 5)
        return match["value"] if match else None

    def wait_listing(self, predicate, timeout, step=1.0):
        """Lists the menus until predicate(listing) holds; the last listing either way."""
        deadline = time.monotonic() + timeout
        listing = self.listing()
        while not predicate(listing) and time.monotonic() < deadline and self.client.alive():
            time.sleep(step)
            listing = self.listing()
        return listing

    def click_item(self, listing, label, **patterns):
        items = find_items(listing, **patterns)
        if not items:
            return None
        item = items[0]
        self.note(f"click {label}: {item['menu']} item {item['item']} \"{plain(item['text'])}\" at {item['click']}")
        self.display.click(item["click"])
        return item

    def back_to_main(self):
        """Escape until only the first menu is open (at most five times)."""
        for _ in range(5):
            listing = self.listing()
            if listing["count"] <= 1:
                return listing
            self.display.key("Escape")
            time.sleep(0.5)
        return self.listing()

    # the checks

    def open_key_menu(self):
        """Opens the key code menu the way a player would; its fields, accept item and how."""
        listing = self.back_to_main()
        opened_by = None
        if self.click_item(listing, "key code menu", action=r"cdkey"):
            opened_by = "click from " + listing["top"]
        else:
            # One level down: an options menu that holds the key code item.
            for item in find_items(listing, action=r"open\s+\S*option"):
                self.click_item(listing, "options menu", action=re.escape(item["action"]))
                inner = self.listing()
                if self.click_item(inner, "key code menu", action=r"cdkey"):
                    opened_by = f"click from {listing['top']} then {inner['top']}"
                    break
                self.display.key("Escape")
                listing = self.listing()
        if not opened_by:
            self.client.send("listmenus")
            names = []
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                match = self.client.wait_for(re.compile(r"^\d+\. (\S+)$"), 1)
                if match:
                    names.append(match[1])
            candidates = [name for name in names if re.search(r"cdkey|key_?code", name, re.I)]
            if candidates:
                self.client.send(f"openmenu {candidates[0]}")
                opened_by = f"console: openmenu {candidates[0]} (no menu item leads there)"
        listing = self.wait_listing(lambda l: len(find_items(l, dvar=r"cdkey[1-5]")) == 5, 5)
        fields = sorted(find_items(listing, dvar=r"cdkey[1-5]"), key=lambda item: item["dvar"].lower())
        accept = find_items(listing, action=r"verifyCDKey")
        return fields, accept, opened_by

    def key_code_menu(self):
        fields, accept, opened_by = self.open_key_menu()
        if not self.check("key code menu opens with five key fields", len(fields) == 5,
                          f"opened by {opened_by}; {len(fields)} fields"):
            return False
        if not self.check("key code menu has an accept item", bool(accept)):
            return False

        key_file = self.out / "client-home" / "codkey.txt"

        def type_key(parts):
            for field, part in zip(fields, parts):
                self.display.click(field["click"])
                self.display.key("BackSpace", 6)
                self.display.type_text(part)
            self.note("typed 20 characters into the five key fields")
            self.display.click(accept[0]["click"])
            time.sleep(1)

        wrong = wrong_checksum(self.args.key)
        type_key(split_key(wrong))
        refused_text = self.dvar("ui_cdkeyvalid")
        # An empty ui_cdkeyvalid means Verify never ran (input did not reach
        # the menu), which is not a refusal.
        self.check("a mistyped key is refused and saves nothing",
                   not key_file.exists() and bool(plain(refused_text or "")),
                   f"ui_cdkeyvalid \"{plain(refused_text or '')}\"")

        listing = self.listing()
        if len(find_items(listing, dvar=r"cdkey[1-5]")) != 5:
            self.note("the key code menu closed after the refused key; opening it again")
            fields, accept, _ = self.open_key_menu()
            if len(fields) != 5 or not accept:
                return self.check("the key code menu opens again", False)
        type_key(split_key(self.args.key))
        accepted_text = self.dvar("ui_cdkeyvalid")
        saved = key_file.exists()
        mode_ok = saved and stat.S_IMODE(key_file.stat().st_mode) == 0o600
        same = saved and re.sub(r"[^0-9A-Z]", "", key_file.read_text().upper()) == \
            re.sub(r"[^0-9A-Z]", "", self.args.key.upper())
        self.check("the test key is accepted and saved as codkey.txt", saved and same,
                   f"ui_cdkeyvalid \"{plain(accepted_text or '')}\"")
        self.check("codkey.txt is readable only by its owner (600)", mode_ok)
        self.check("the menu says something else for a good key than for a mistyped one",
                   accepted_text is not None and refused_text is not None and accepted_text != refused_text)
        self.back_to_main()
        return saved

    def open_join_game(self):
        listing = self.back_to_main()
        item = self.click_item(listing, "Join Game", action=r"pc_join_unranked|join_?game") or \
            self.click_item(listing, "Join Game", text=r"^join game$")
        listing = self.wait_listing(lambda l: find_items(l, action=r"RefreshServers"), 10)
        return self.check("Join Game opens the server list", bool(item and find_items(listing, action=r"RefreshServers")),
                          f"top menu {listing['top']}")

    def set_source(self, wanted, label):
        value = None
        for _ in range(5):
            value = self.dvar("ui_netSource")
            if value is not None and value.strip() == str(wanted):
                return True
            listing = self.listing()
            if not (self.click_item(listing, "source", dvar=r"ui_netSource")
                    or self.click_item(listing, "source", action=r"RefreshFilter")):
                break
        return self.check(f"the source can be set to {label}", False, f"ui_netSource {value}")

    def refresh_and_find(self, label):
        listing = self.listing()
        if not self.click_item(listing, "Refresh", action=r"RefreshServers"):
            return self.check(f"{label}: Refresh", False, "no refresh item")
        listing = self.wait_listing(lambda l: rows_naming(l, HOSTNAME), self.args.list_timeout, 2)
        rows = rows_naming(listing, HOSTNAME)
        self.check(f"{label}: the server is listed", bool(rows),
                   plain(rows[0]["text"]) if rows else f"{len(listing['rows'])} rows")
        return rows[0] if rows else None

    def favourite_and_join(self):
        if not self.open_join_game():
            return
        if not self.set_source(0, "Local"):
            return
        row = self.refresh_and_find("Local list")
        if not row:
            return
        self.display.click(row["click"])
        listing = self.listing()
        selected = [r for r in rows_naming(listing, HOSTNAME) if r["selected"]]
        self.check("clicking the server selects it", bool(selected))
        if not self.click_item(listing, "Add to Favourites", action=r"addFavorite"):
            self.check("Add to Favourites", False, "no item with uiScript addFavorite")
            return
        if not self.set_source(2, "Favourites"):
            return
        row = self.refresh_and_find("Favourites")
        if not row:
            return
        self.display.click(row["click"])
        listing = self.listing()
        if not self.click_item(listing, "Join", action=r"JoinServer"):
            self.check("Join from the list", False, "no item with uiScript JoinServer")
            return
        joined = self.server.wait_for(re.compile(r"Going from CS_FREE to CS_CONNECTED"), self.args.join_timeout)
        self.check("Join from the favourites list connects to the server", bool(joined))
        if joined:
            self.client.wait_quiet(3, 20)
            self.client.send("disconnect")
            self.wait_for_menu(60)

    def favourites_after_restart(self):
        status = self.client.finish(30)
        self.note(f"client exit {status} before the restart")
        if self.start_client() is None:
            self.check("the client starts again", False)
            return
        if not self.open_join_game() or not self.set_source(2, "Favourites"):
            return
        self.refresh_and_find("Favourites after a restart")

    def run(self):
        self.out.mkdir(parents=True, exist_ok=True)
        if (self.out / "client-home").exists() and any((self.out / "client-home").iterdir()):
            raise RuntimeError(f"{self.out}/client-home is not empty; use a new --out folder")
        self.display = Display(self.args.xvfb, self.args.xdotool, self.args.width, self.args.height,
                               self.out / "xvfb.log")
        self.note(f"using display {self.display.name}")
        try:
            self.start_server()
            if self.start_client() is None:
                self.check("the client reaches its main menu", False)
                return
            top = self.last_top()
            # A profile pop-up on top means the seeded profile was not taken,
            # and every later step would click under it.
            if not self.check("the client reaches its main menu", not top.startswith("profile_"), f"top menu {top}"):
                return
            size = "x".join(map(str, self.display.size))
            if not self.check("the game window has the keyboard focus", self.display.focus(),
                              f"window {self.display.window} ({size}) on display {self.display.name}"):
                return
            if self.key_code_menu():
                self.favourite_and_join()
            else:
                self.note("the key code was not saved, so the join is skipped (it would have no key)")
            if not self.args.no_restart:
                self.favourites_after_restart()
        finally:
            statuses = {}
            if self.client:
                statuses["client"] = self.client.finish(30)
            if self.server:
                statuses["server"] = self.server.finish(30)
            self.display.close()
            self.results["exit_codes"] = statuses
            for name, status in statuses.items():
                self.check(f"{name} exits with 0", status == 0, f"exit {status}")

    def last_top(self):
        for line in self.last_listing or []:
            match = STACK_RE.match(line)
            if match:
                return match["top"]
        return "-"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", required=True, type=Path, help="KisakCOD-mp")
    parser.add_argument("--server", required=True, type=Path, help="KisakCOD-dedi")
    parser.add_argument("--game-dir", required=True, type=Path, help="the CoD4 install (fs_basepath)")
    parser.add_argument("--out", required=True, type=Path, help="a new folder for logs, homes and results")
    parser.add_argument("--key", default=TEST_KEY, help="the key to type (default: the made-up test key)")
    parser.add_argument("--client-args", default="", help="more client arguments, as one string")
    parser.add_argument("--server-args", default="", help="more server arguments, as one string")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--xvfb", default="Xvfb")
    parser.add_argument("--xdotool", default="xdotool")
    parser.add_argument("--server-timeout", type=float, default=180)
    parser.add_argument("--client-timeout", type=float, default=240)
    parser.add_argument("--list-timeout", type=float, default=20)
    parser.add_argument("--join-timeout", type=float, default=180)
    parser.add_argument("--no-restart", action="store_true", help="skip the favourites-after-restart check")
    args = parser.parse_args(argv)

    for tool in (args.xvfb, args.xdotool):
        if not shutil.which(tool):
            print(f"{tool} is not installed", file=sys.stderr)
            return 2
    try:
        split_key(args.key)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    run = Run(args)
    try:
        run.run()
    except RuntimeError as error:
        run.check("set-up", False, str(error))
    (args.out / "results.json").write_text(json.dumps(run.results, indent=2) + "\n")
    failed = [entry["check"] for entry in run.results["checks"] if entry["result"] != "pass"]
    print(f"\n{len(run.results['checks']) - len(failed)} passed, {len(failed)} failed; "
          f"{args.out / 'results.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Opt-in OS-level test of the SDL window/input shell on a private Xvfb.

Requires Xvfb, xdotool, and host libX11. Keys/buttons use synthetic XTest input
sent through the X server by xdotool, not physical hardware input and not
SDL_PushEvent. Motion/resize/focus use X requests; close uses WM_DELETE_WINDOW. This
checks the software-surface shell and engine input queue boundary, not gameplay
or the D3D renderer. No X access-control settings are changed.

With --engine-input, target KisakCOD-client-input to verify the production
disconnected console editor, key state across resize/focus changes, and clean
WM/Escape/SIGTERM/SIGINT teardown while a key is held. Repeated launches share
one X server; they test process/window cleanup, not same-process host reuse.
The host explicitly reports game-frame execution unavailable.

Add --commands for real command-buffer execution across ticks, rejected local
commands, loopback discovery and live network restart/cancellation. Replies are
data only; no remote commands or authentication attempts are permitted.

Pass --runner 'qemu-i386' when the client cannot execute directly. Configure any
client libraries through that runner's environment (e.g. QEMU_SET_ENV), while
--xdotool-library-path applies only to the host xdotool process.
"""

import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import re
import select
import shlex
import signal
import subprocess
import tempfile
import time
import uuid


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def wait_for(predicate, timeout, label, process=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process and process.poll() is not None:
            raise RuntimeError(f"{label}: client exited early ({process.returncode})")
        select.select([], [], [], 0.025)
    raise RuntimeError(f"{label}: timed out after {timeout}s")


def request_window_close(display, window):
    """Send the same X11 protocol message a window manager's close button uses."""
    class Data(ctypes.Union):
        _fields_ = [("b", ctypes.c_char * 20), ("s", ctypes.c_short * 10),
                    ("l", ctypes.c_long * 5)]

    class Message(ctypes.Structure):
        _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong),
                    ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                    ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                    ("format", ctypes.c_int), ("data", Data)]

    class Event(ctypes.Union):
        _fields_ = [("message", Message), ("pad", ctypes.c_long * 24)]

    library = ctypes.util.find_library("X11")
    if not library:
        raise RuntimeError("host libX11 is required for WM_DELETE_WINDOW")
    x11 = ctypes.CDLL(library)
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                              ctypes.c_long, ctypes.POINTER(Event)]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    connection = x11.XOpenDisplay(display.encode())
    if not connection:
        raise RuntimeError("could not open the private X display for close request")
    try:
        event = Event()
        event.message.type = 33  # ClientMessage
        event.message.display = connection
        event.message.window = window
        event.message.message_type = x11.XInternAtom(connection, b"WM_PROTOCOLS", 0)
        event.message.format = 32
        event.message.data.l[0] = x11.XInternAtom(connection, b"WM_DELETE_WINDOW", 0)
        if not x11.XSendEvent(connection, window, 0, 0, ctypes.byref(event)):
            raise RuntimeError("XSendEvent rejected WM_DELETE_WINDOW")
        x11.XSync(connection, 0)
    finally:
        x11.XCloseDisplay(connection)


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--runner", default="")
    parser.add_argument("--xvfb", default="Xvfb")
    parser.add_argument("--xdotool", default="xdotool")
    parser.add_argument("--xdotool-library-path")
    parser.add_argument("--relative", action="store_true", help="also verify OS relative-mouse capture lifecycle")
    parser.add_argument("--engine-input", action="store_true",
                        help="verify real disconnected client editor/state and repeated close/signal lifecycles")
    parser.add_argument("--commands", action="store_true",
                        help="also verify real console executor, local UDP discovery and network restart")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    if args.timeout <= 0 or not args.binary.is_file():
        parser.error("provide an existing shell binary and a positive timeout")
    if args.commands and not args.engine_input:
        parser.error("--commands requires --engine-input")
    logs = args.log_dir or Path(tempfile.mkdtemp(prefix="kisak-window-x11-"))
    logs.mkdir(parents=True, exist_ok=True)
    processes, streams = [], []
    read_fd, write_fd = os.pipe()
    try:
        xvfb_log = (logs / "xvfb.log").open("w")
        streams.append(xvfb_log)
        xvfb = subprocess.Popen([args.xvfb, "-displayfd", str(write_fd), "-screen", "0",
                                 "800x600x24", "-nolisten", "tcp"],
                                pass_fds=(write_fd,), stdout=xvfb_log, stderr=subprocess.STDOUT)
        processes.append(xvfb)
        os.close(write_fd)
        write_fd = None
        if not select.select([read_fd], [], [], args.timeout)[0]:
            raise RuntimeError("private Xvfb did not publish its display number")
        number = os.read(read_fd, 64).decode().strip()
        if not number.isdecimal():
            raise RuntimeError(f"invalid Xvfb display response: {number!r}")
        display = ":" + number
        env = dict(os.environ, DISPLAY=display, SDL_VIDEODRIVER="x11")
        tool_env = dict(env)
        if args.xdotool_library_path:
            tool_env["LD_LIBRARY_PATH"] = args.xdotool_library_path

        def xdo(*command, check=True):
            result = subprocess.run([args.xdotool, *map(str, command)], env=tool_env,
                                    capture_output=True, text=True, timeout=args.timeout)
            if check and result.returncode:
                raise RuntimeError(f"xdotool {' '.join(map(str, command))}: {result.stderr.strip()}")
            return result

        def launch(name):
            title = "KisakWindowTest-" + name + "-" + uuid.uuid4().hex
            path = logs / (name + ".log")
            stream = path.open("w")
            streams.append(stream)
            process = subprocess.Popen(shlex.split(args.runner) + [str(args.binary.resolve()),
                                       "--run-ms", "60000", "--trace", "--title", title] +
                                       (["--relative"] if args.relative and name != "focus-peer" else []),
                                       env=env, stdout=stream, stderr=subprocess.STDOUT)
            processes.append(process)
            wait_for(lambda: "window=created driver=x11" in read(path) and
                     re.search(r"window=ready .*driver=x11", read(path)), args.timeout,
                     name + " X11 software-surface readiness", process)
            ids = wait_for(lambda: xdo("search", "--onlyvisible", "--name", "^" + title + "$",
                                      check=False).stdout.split(), args.timeout, name + " X11 window", process)
            if len(ids) != 1:
                raise RuntimeError(f"{name}: expected exactly one matching window, found {ids}")
            if args.engine_input:
                wait_for(lambda: "input=engine connection=disconnected frame=unavailable" in read(path) and
                         "editor text= cursor=0 held=0\n" in read(path),
                         args.timeout, name + " fresh production input state", process)
            return process, int(ids[0]), path

        def require_closed(process, path, label, window_id):
            if process.wait(timeout=args.timeout) != 0:
                raise RuntimeError(label + ": process did not close cleanly")
            summary = read(path)
            if summary.count("window=closing\n") != 1 or not re.search(r"window=closed .* held=0 .* game=false", summary):
                raise RuntimeError(label + ": missing unique close and empty held-state evidence")
            if args.engine_input and not re.search(r"window=closed .*relative_active=0 input=engine frame=unavailable game=false", summary):
                raise RuntimeError(label + ": production input/capture teardown missing")
            # A new process is used for each cycle. This tests OS window/process
            # cleanup, not same-process WindowHost allocator/resource reuse.
            title = re.search(r"window=created .*title=(\S+)", summary)
            if title and xdo("search", "--name", "^" + re.escape(title.group(1)) + "$", check=False).stdout.strip():
                raise RuntimeError(label + f": X11 window remains after process exit ({window_id})")
            return summary

        client, window, trace = launch("main")
        peer, peer_window, peer_trace = launch("focus-peer")
        print(f"Private X11 display {display}; main window {window}; focus peer {peer_window}", flush=True)
        xdo("windowmove", peer_window, 650, 450)
        xdo("windowmove", window, 10, 10)
        xdo("windowraise", window)
        xdo("windowfocus", "--sync", window)
        if args.relative:
            wait_for(lambda: "relative requested=1 active=1 error=\n" in read(trace),
                     args.timeout, "relative capture enabled", client)
        xdo("windowsize", "--sync", window, 640, 360)
        wait_for(lambda: "resize 640 360\n" in read(trace), args.timeout, "OS resize", client)
        print("PASS: real X11 window and resize", flush=True)

        offset = len(read(trace))
        xdo("key", "a")
        wait_for(lambda: all(line in read(trace)[offset:] for line in
                            ("key=97 down=1\n", "key=97 down=0\n", "character=97\n")),
                 args.timeout, "XTest key/text through engine queue", client)
        print("PASS: XTest keyboard and SDL text through engine queue", flush=True)
        if args.engine_input:
            wait_for(lambda: "editor text=a cursor=1 held=0\n" in read(trace)[offset:],
                     args.timeout, "production editor inserted a", client)
            offset = len(read(trace))
            xdo("key", "b")
            wait_for(lambda: "editor text=ab cursor=2 held=0\n" in read(trace)[offset:],
                     args.timeout, "production editor inserted b", client)
            offset = len(read(trace))
            xdo("key", "BackSpace")
            wait_for(lambda: "editor text=a cursor=1 held=0\n" in read(trace)[offset:],
                     args.timeout, "production editor handled BackSpace", client)
            print("PASS: real client console editor insertion and BackSpace", flush=True)
        command_checks = None
        if args.commands:
            from client_command_x11_checks import CommandChecks
            command_checks = CommandChecks(xdo, wait_for, read, trace, client, args.timeout)
            command_checks.run()

        offset = len(read(trace))
        if args.relative:
            xdo("mousemove_relative", "--", 15, 9)
        else:
            xdo("mousemove", "--window", window, 100, 80)
        xdo("click", 1)
        xdo("click", 4)
        wait_for(lambda: all(line in read(trace)[offset:] for line in
                            ("key=200 down=1\n", "key=200 down=0\n",
                             "key=206 down=1\n", "key=206 down=0\n")),
                 args.timeout, "OS pointer/button/wheel dispatch", client)
        if args.relative:
            wait_for(lambda: any(int(dx) or int(dy) for dx, dy in
                                 re.findall(r"mouse x=-?\d+ y=-?\d+ dx=(-?\d+) dy=(-?\d+)", read(trace)[offset:])),
                     args.timeout, "relative pointer movement", client)
        else:
            wait_for(lambda: "mouse x=100 y=80 " in read(trace)[offset:], args.timeout, "absolute pointer movement", client)
        print("PASS: X11 pointer motion and XTest button/wheel", flush=True)

        offset = len(read(trace))
        xdo("keydown", "w")
        wait_for(lambda: "key=119 down=1\n" in read(trace)[offset:], args.timeout, "held key", client)
        if args.engine_input:
            wait_for(lambda: re.search(r"editor text=.* cursor=\d+ held=1\n", read(trace)[offset:]),
                     args.timeout, "real client tracks held key", client)
            offset = len(read(trace))
            xdo("windowsize", "--sync", window, 620, 340)
            wait_for(lambda: "resize 620 340\n" in read(trace)[offset:] and
                     re.search(r"editor text=.* cursor=\d+ held=1\n", read(trace)[offset:]),
                     args.timeout, "resize while production input remains held", client)
            print("PASS: OS resize while real client key state remains held", flush=True)
        offset = len(read(trace))
        xdo("windowfocus", "--sync", peer_window)
        reset_marker = "input=reset held=0\n" if args.engine_input else "input=reset\n"
        wait_for(lambda: "focus lost\n" in read(trace)[offset:] and reset_marker in read(trace)[offset:],
                 args.timeout, "focus loss resets held input", client)
        if args.relative:
            wait_for(lambda: "relative requested=1 active=0 error=\n" in read(trace)[offset:],
                     args.timeout, "blur releases relative capture", client)
        xdo("keyup", "w")
        offset = len(read(trace))
        xdo("windowfocus", "--sync", window)
        wait_for(lambda: "focus gained\n" in read(trace)[offset:], args.timeout, "focus regained", client)
        if args.relative:
            wait_for(lambda: "relative requested=1 active=1 error=\n" in read(trace)[offset:],
                     args.timeout, "focus restores requested capture", client)
        xdo("key", "w")
        wait_for(lambda: "key=119 down=1\n" in read(trace)[offset:] and "key=119 down=0\n" in read(trace)[offset:],
                 args.timeout, "fresh press after focus loss", client)
        print("PASS: real focus transfer clears held state and accepts a fresh press", flush=True)

        if args.engine_input:
            if command_checks:
                command_checks.prepare_shutdown()
            offset = len(read(trace))
            xdo("keydown", "z")
            wait_for(lambda: "key=122 down=1\n" in read(trace)[offset:] and
                     re.search(r"editor text=.* cursor=\d+ held=1\n", read(trace)[offset:]),
                     args.timeout, "held key before WM close", client)
        offset = len(read(trace))
        request_window_close(display, window)
        summary = require_closed(client, trace, "main", window)
        if command_checks:
            command_checks.verify_shutdown()
        if args.engine_input:
            xdo("keyup", "z")
        if args.relative:
            if "relative requested=1 active=0 error=\n" not in summary[offset:]:
                raise RuntimeError("close did not release relative capture")
            print("PASS: relative capture active, released on blur, restored on focus, released on close", flush=True)
        request_window_close(display, peer_window)
        require_closed(peer, peer_trace, "focus peer", peer_window)
        print("PASS: WM_DELETE_WINDOW through X11, clean process exit, held=0", flush=True)
        if args.engine_input:
            for label, number in (("escape", None), ("sigterm", signal.SIGTERM), ("sigint", signal.SIGINT)):
                cycle, cycle_window, cycle_trace = launch("cycle-" + label)
                xdo("windowfocus", "--sync", cycle_window)
                if args.relative:
                    wait_for(lambda: "relative requested=1 active=1 error=\n" in read(cycle_trace),
                             args.timeout, label + " relative capture", cycle)
                offset = len(read(cycle_trace))
                xdo("keydown", "w")
                wait_for(lambda: "key=119 down=1\n" in read(cycle_trace)[offset:] and
                         re.search(r"editor text=.* cursor=\d+ held=1\n", read(cycle_trace)[offset:]),
                         args.timeout, label + " held production key", cycle)
                if number is None:
                    xdo("key", "Escape")
                else:
                    cycle.send_signal(number)
                cycle_log = require_closed(cycle, cycle_trace, label, cycle_window)
                xdo("keyup", "w")  # Release X server state before creating the next client.
                if number is not None and f"signal={int(number)}\n" not in cycle_log:
                    raise RuntimeError(label + ": no explicit handled-signal evidence")
                print(f"PASS: repeated process/window lifecycle: {label} closes with real held=0 and capture released", flush=True)
        print("Verified software window/input shell only; no game renderer or gameplay.", flush=True)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", flush=True)
        return 1
    finally:
        for process in reversed(processes):
            stop(process)
        for stream in streams:
            stream.close()
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)
        print(f"Evidence: {logs}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())

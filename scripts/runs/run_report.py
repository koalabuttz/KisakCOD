#!/usr/bin/env python3
"""Turn a run's results file into its report, with nothing retyped.

A run on a test machine (docs/runs.md) ends with one ``results.json``. The
machine cannot write the project's shared folder, so the file crosses as pasted
text. This script checks that it crossed whole and writes what used to be typed
by hand twice: the Markdown report with every table and verbatim line, and a
summary short enough for a thread message.

    run_report.py results.json                     print the report
    run_report.py results.json --out report.md     write it
    run_report.py results.json --summary           print the short summary
    run_report.py results.json --check             only check the checksum
    run_report.py results.json --compact           print the short form to paste

The short form (``--compact``, and ``results-compact.json`` beside every
results file) is what a machine pastes when the whole file is too long to
relay: per row the status, the power, the counts that are not zero, what the
scene read and the frame log line as the game printed it, with a checksum of
its own and the checksum of the file it was cut from. Every table can be made
from it; the other verbatim lines stay in the full file on the machine.

The checksum is the SHA-256 of the results without their ``sha256`` entry,
written as JSON with sorted keys, no spaces and ASCII escapes (``canonical``
below). A file whose lines were shortened or reworded on the way fails it, and
the report says so at its top instead of passing for the machine's words.

Exit status: 0 on success, 1 when the checksum does not match (the report is
still written, marked), 2 on a usage error or an unreadable file.

Standard library only.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

RESULT_FORMAT = "kisak-run-result/1"
SUMMARY_LIMIT = 2000
# The line the framelog console command prints (src/qcommon/com_frame_log.h).
FRAME_LOG = re.compile(
    r"frame log(?: (?P<label>\S+))?: (?P<frames>\d+) frames in (?P<seconds>[\d.]+) s: (?P<fps>[\d.]+) fps; "
    r"ms mean (?P<mean>[\d.]+), median (?P<median>[\d.]+), p95 (?P<p95>[\d.]+), p99 (?P<p99>[\d.]+), "
    r"max (?P<max>[\d.]+); (?P<over50>\d+) over 50 ms(?: \((?P<why>[^)]*)\))?")

FRAME_COLUMNS = (("fps", "fps", 1), ("mean", "mean ms", 3), ("median", "median", 3), ("p95", "p95", 3),
                 ("p99", "p99", 3), ("max", "max ms", 3))


def canonical(results):
    """The bytes the checksum is taken over."""
    body = {key: value for key, value in results.items() if key != "sha256"}
    return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def checksum(results):
    return hashlib.sha256(canonical(results)).hexdigest()


def seal(results):
    """Returns the results with their checksum set."""
    sealed = dict(results)
    sealed["sha256"] = checksum(results)
    return sealed


def verify(results):
    """None when the checksum holds, else what is wrong with it."""
    stated = results.get("sha256")
    if not stated:
        return "the results carry no checksum"
    actual = checksum(results)
    if stated != actual:
        return f"the checksum does not match (stated {stated[:12]}..., content {actual[:12]}...)"
    return None


def load(path):
    text = Path(path).read_text(encoding="utf-8")
    # A paste can arrive inside a Markdown code fence, or with words around it.
    start, end = text.find("{"), text.rfind("}")
    if start < 0 or end < start:
        raise ValueError("no JSON object found")
    results = json.loads(text[start:end + 1])
    if not isinstance(results, dict) or results.get("format") != RESULT_FORMAT:
        raise ValueError(f"not a {RESULT_FORMAT} file")
    return results


def _number(value, digits):
    return "" if value is None else f"{value:.{digits}f}"


def _power(row):
    power = row.get("power") or {}
    if not power:
        return ""
    state = "mains" if power.get("ac") else "battery"
    start, end = power.get("battery_start"), power.get("battery_end")
    if start is not None and end is not None:
        state += f", {start} to {end}%"
    if power.get("profile"):
        state += f", {power['profile']}"
    if power.get("changed"):
        state = "**" + state + ", changed during the capture**"
    if power.get("slept"):
        state = "**" + state + f", slept {power['slept']:g} s during the row**"
    return state


def frame_summary(match):
    """The figures of one frame log line, from its FRAME_LOG match."""
    summary = {key: float(match.group(key)) for key in ("seconds", "fps", "mean", "median", "p95", "p99", "max")}
    summary.update(label=match.group("label") or "", frames=int(match.group("frames")),
                   over50=int(match.group("over50")), line=match.group(0))
    return summary


def _frames(row):
    """The row's frame-log summaries (usually one). The short form carries
    only each line; its figures are read from it again."""
    logs = []
    for log in row.get("framelog") or []:
        if "fps" not in log and log.get("line"):
            match = FRAME_LOG.search(log["line"])
            log = {**frame_summary(match), **log} if match else log
        logs.append(log)
    return logs


def compact(results):
    """The short form of a results file, sealed with a checksum of its own."""
    short = {key: value for key, value in results.items() if key not in ("rows", "sha256")}
    short["compact"] = True
    short["full_sha256"] = results.get("sha256", "")
    short["rows"] = []
    for row in results.get("rows") or []:
        kept = {key: row[key] for key in ("name", "variant", "status", "exit", "seconds", "power", "guard", "values",
                                          "note", "binary") if row.get(key) not in (None, "", {})}
        kept["framelog"] = [{"label": log.get("label", ""), "line": log.get("line", "")} for log in _frames(row)]
        counts = {name: count for name, count in (row.get("counts") or {}).items() if count}
        if counts:
            kept["counts"] = counts
        failed = [step for step in row.get("steps") or [] if not step[1]]
        if failed:
            kept["steps"] = failed
        load_note = row.get("load") or {}
        if load_note.get("waited") or load_note.get("over"):
            kept["load"] = {key: load_note[key] for key in ("waited", "over") if key in load_note}
        short["rows"].append(kept)
    return seal(short)


def frame_table(rows):
    lines = ["| # | Row | " + " | ".join(title for _, title, _ in FRAME_COLUMNS)
             + " | over 50 ms | frames | power | status |",
             "| --- | --- | " + " | ".join("---:" for _ in FRAME_COLUMNS) + " | ---: | ---: | --- | --- |"]
    for index, row in enumerate(rows, 1):
        logs = _frames(row) or [None]
        for log in logs:
            name = row["name"]
            if log and log.get("label") and log["label"] != row["name"]:
                name += f" ({log['label']})"
            cells = [_number(log.get(key) if log else None, digits) for key, _, digits in FRAME_COLUMNS]
            lines.append(f"| {index} | {name} | " + " | ".join(cells)
                         + f" | {'' if not log else log.get('over50', '')} | {'' if not log else log.get('frames', '')}"
                         + f" | {_power(row)} | {row.get('status', '')} |")
    return lines


def averages(rows):
    """Per variant, the mean of each figure over the rows that count: status ok,
    with a frame log. Returns [(variant, count, {key: mean}, [fps values])]."""
    order, grouped = [], {}
    for row in rows:
        if row.get("status") != "ok":
            continue
        for log in _frames(row):
            variant = row.get("variant", row["name"])
            if variant not in grouped:
                grouped[variant] = []
                order.append(variant)
            grouped[variant].append(log)
    table = []
    for variant in order:
        logs = grouped[variant]
        means = {key: sum(log[key] for log in logs) / len(logs) for key, _, _ in FRAME_COLUMNS
                 if all(log.get(key) is not None for log in logs)}
        table.append((variant, len(logs), means, [log["fps"] for log in logs if log.get("fps") is not None]))
    return table


def average_table(rows):
    table = averages(rows)
    if not table:
        return []
    lines = ["| Row | launches | " + " | ".join(title for _, title, _ in FRAME_COLUMNS) + " | fps of each |",
             "| --- | ---: | " + " | ".join("---:" for _ in FRAME_COLUMNS) + " | --- |"]
    for variant, count, means, each in table:
        cells = [_number(means.get(key), digits) for key, _, digits in FRAME_COLUMNS]
        lines.append(f"| {variant} | {count} | " + " | ".join(cells) + " | "
                     + ", ".join(f"{value:.1f}" for value in each) + " |")
    return lines


def _counts(row):
    counts = row.get("counts") or {}
    return ", ".join(f"{name} {counts[name]}" for name in sorted(counts))


def render(results, problem=None):
    """The Markdown report."""
    rows = results.get("rows") or []
    out = [f"# {results.get('title') or results.get('id', 'Run')}", ""]
    if problem:
        out += [f"**Not the machine's own words: {problem}. Ask for the results file again before quoting "
                "from this report.**", ""]
    facts = [f"Run `{results.get('id', '')}` on {results.get('machine', 'an unnamed machine')}",
             f"{results.get('started', '?')} to {results.get('finished', '?')} UTC"]
    if results.get("asker"):
        facts.append(f"asked for by {results['asker']}")
    out += [", ".join(facts) + ". Written by `scripts/runs/run_report.py` from the machine's results file "
            f"(checksum {'not verified' if problem else 'verified'}). Local results, not CI evidence.", ""]
    if results.get("compact"):
        out += ["From the short form of the results: the frame log lines are the machine's, the other verbatim "
                f"lines are in the full file on the machine (checksum {results.get('full_sha256', '')[:12]}...).", ""]
    out += [f"**Status:** {results.get('status', 'unknown')}.", ""]
    if results.get("build"):
        out += ["## Build", ""]
        for key in sorted(results["build"]):
            out.append(f"- {key}: {results['build'][key]}")
        out.append("")

    if any(_frames(row) for row in rows):
        out += ["## Frame rate, in run order", ""] + frame_table(rows) + [""]
        table = average_table(rows)
        if table:
            out += ["## Per-row averages", "", "Rows with status ok only.", ""] + table + [""]

    out += ["## Rows", ""]
    for row in rows:
        out.append(f"### {row['name']}")
        out.append("")
        facts = [f"status {row.get('status', '?')}", f"exit {row.get('exit')}", f"{row.get('seconds', 0):.0f} s"]
        if _power(row):
            facts.append(_power(row))
        load_note = row.get("load") or {}
        if load_note.get("waited"):
            facts.append(f"waited {load_note['waited']:.0f} s for other load")
        if load_note.get("top"):
            facts.append("busiest other processes before the launch: "
                         + ", ".join(f"{name} {share:.0f}%" for name, share in load_note["top"])
                         + (" (still over the limit when the wait ran out)" if load_note.get("over") else ""))
        if load_note.get("at_capture"):
            facts.append("busiest other processes at the capture: "
                         + ", ".join(f"{name} {share:.0f}%" for name, share in load_note["at_capture"]))
        values = row.get("values") or {}
        if values:
            facts.append("values: " + ", ".join(f"{name} {values[name]}" for name in sorted(values)))
        if row.get("note"):
            facts.append(f"note: {row['note']}")
        if (row.get("binary") or {}).get("sha256"):
            facts.append(f"binary sha256 {row['binary']['sha256']}..., built {row['binary'].get('modified', '?')}")
        if row.get("guard"):
            facts.append(f"guard {row['guard']}")
        if _counts(row):
            facts.append(f"counts: {_counts(row)}")
        out += ["- " + fact for fact in facts]
        failed = [step for step in row.get("steps") or [] if not step[1]]
        for name, _, detail in failed:
            out.append(f"- step failed: {name}" + (f" ({detail})" if detail else ""))
        out.append("")
        verbatim = [log["line"] for log in _frames(row) if log.get("line")]
        for pattern in sorted(row.get("lines") or {}):
            verbatim += row["lines"][pattern]
        if verbatim:
            out += ["```"] + verbatim + ["```", ""]
        for name in sorted(row.get("outputs") or {}):
            out += [f"`{name}`:", "", "```"] + row["outputs"][name] + ["```", ""]
        if row.get("tail"):
            out += ["Last lines of the log:", "", "```"] + row["tail"] + ["```", ""]
    return "\n".join(out).rstrip() + "\n"


def summarize(results, problem=None, limit=SUMMARY_LIMIT):
    """A summary for a thread message: status, the averages, what failed."""
    rows = results.get("rows") or []
    parts = [f"{results.get('id', 'run')}: {results.get('status', 'unknown')}."]
    if problem:
        parts.append(f"CHECKSUM PROBLEM: {problem}.")
    done = sum(1 for row in rows if row.get("status") == "ok")
    parts.append(f"{done} of {len(rows)} rows ok.")
    table = averages(rows)
    if table:
        parts.append("fps (mean ms, p99 ms) per row, ok launches only: " + "; ".join(
            f"{variant} {means.get('fps', 0):.1f} ({means.get('mean', 0):.3f}, {means.get('p99', 0):.3f}) "
            f"from {count}" + (f" [{', '.join(f'{value:.1f}' for value in each)}]" if count > 1 else "")
            for variant, count, means, each in table) + ".")
    for row in rows:
        if row.get("status") != "ok":
            parts.append(f"{row['name']}: {row.get('status')}.")
    for row in rows:
        counts = row.get("counts") or {}
        bad = {name: counts[name] for name in ("errors", "asserts") if counts.get(name)}
        if bad:
            parts.append(f"{row['name']}: " + ", ".join(f"{name} {bad[name]}" for name in sorted(bad)) + ".")
        if row.get("guard") and row["guard"] != "ok":
            parts.append(f"{row['name']}: guard {row['guard']}.")
    text = " ".join(parts)
    cut = " ... (cut; see the report)"
    if len(text) > limit:
        text = text[:max(0, limit - len(cut))].rstrip() + cut
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", help="results.json, or a text file with it pasted inside")
    parser.add_argument("--out", help="write the report here instead of printing it")
    parser.add_argument("--summary", action="store_true", help="print the short summary instead")
    parser.add_argument("--check", action="store_true", help="only check the checksum")
    parser.add_argument("--compact", action="store_true", help="print the short form, to paste")
    parser.add_argument("--summary-limit", type=int, default=SUMMARY_LIMIT)
    args = parser.parse_args(argv)

    try:
        results = load(args.results)
    except (OSError, ValueError) as error:
        print(f"run_report.py: {args.results}: {error}", file=sys.stderr)
        return 2
    problem = verify(results)
    if args.check:
        print("checksum ok" if not problem else problem)
        return 1 if problem else 0
    if args.compact:
        if problem:
            print(f"run_report.py: {args.results}: {problem}", file=sys.stderr)
            return 1
        print(json.dumps(compact(results), sort_keys=True, separators=(",", ":"), ensure_ascii=True))
    elif args.summary:
        print(summarize(results, problem, args.summary_limit))
    else:
        report = render(results, problem)
        if args.out:
            Path(args.out).write_text(report, encoding="utf-8")
            print(f"wrote {args.out}" + (f" ({problem})" if problem else ""))
        else:
            sys.stdout.write(report)
    return 1 if problem else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Seal a Vita run's results with run_report.py's checksum, check them, and write the short form.

    python -I vita_seal.py <body.json> <results.json> <results-compact.json>

<body.json> is the results object without its checksum (as vita-run.ps1 wrote it). The sealed object goes to
<results.json>, its short form to <results-compact.json> (one line), and the script then reads <results.json>
back and verifies it. Prints "checksum ok" (or what is wrong), the checksum, and the short form's path.
run_report.py (scripts/runs/run_report.py of the repository) has to lie beside this file, as on a machine
the runner was copied to, or one folder up, as in the repository.
Standard library only. Exit status: 0 when the file verifies, 1 when it does not, 2 on a usage error.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE if (HERE / "run_report.py").is_file() else HERE.parent))
import run_report  # noqa: E402


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    body_path, results_path, compact_path = (Path(a) for a in argv)
    body = json.loads(body_path.read_text(encoding="utf-8-sig"))
    body.pop("sha256", None)
    if body.get("format") != run_report.RESULT_FORMAT:
        print(f"vita_seal.py: not a {run_report.RESULT_FORMAT} object", file=sys.stderr)
        return 2
    sealed = run_report.seal(body)
    results_path.write_text(json.dumps(sealed, sort_keys=True, indent=1, ensure_ascii=True) + "\n", encoding="ascii")
    short = run_report.compact(sealed)
    compact_path.write_text(json.dumps(short, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                            encoding="ascii")
    problem = run_report.verify(run_report.load(results_path))
    print("checksum ok" if not problem else problem)
    print("sha256 " + sealed["sha256"])
    print("compact " + str(compact_path))
    return 1 if problem else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

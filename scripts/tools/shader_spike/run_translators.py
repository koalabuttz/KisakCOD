#!/usr/bin/env python3
"""Run D3D9 shader translators over test shaders and validate their SPIR-V.

Part of the renderer plan's shader spike (S0). Each translator is a command
line program given by path; this script never builds or downloads anything.

  --vkd3d PATH        vkd3d-compiler (vkd3d-shader), d3dbc -> spirv-binary
  --mojoshader PATH   a driver that runs MOJOSHADER_parse("spirv") and writes
                      the SPIR-V words: mojo_spv <in> <out>
  --dxbc-spirv PATH   dxbc-spirv's dxbc_compiler, <in> --spv <out>

Shaders come from the op-by-op cases in cases.py (default) or
from a folder of bytecode files (--corpus, e.g. shader_corpus.py --dump-dir).
For a corpus the report holds counts only, so it can leave the machine.

SPIR-V is checked with spirv-val for Vulkan 1.3. dxbc-spirv's stand-alone
output keeps D3D9 state in scalar-layout uniform blocks, so its modules are
validated with --scalar-block-layout (Vulkan 1.2 scalarBlockLayout, which
DXVK 3.x also requires).
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import d3d9_asm  # noqa: E402
import cases as shader_cases  # noqa: E402

TRANSLATORS = {
    "vkd3d-shader": lambda exe, src, out: [exe, "-x", "d3dbc", "-b", "spirv-binary", "-o", out, src],
    "mojoshader": lambda exe, src, out: [exe, src, out],
    "dxbc-spirv": lambda exe, src, out: [exe, src, "--spv", out],
}
VALIDATOR_ARGS = {
    "dxbc-spirv": ["--scalar-block-layout"],
}


def first_line(text: str) -> str:
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line[:200]
    return ""


def run_one(name, exe, src, workdir, spirv_val, timeout):
    out = os.path.join(workdir, "%s.spv" % name)
    if os.path.exists(out):
        os.unlink(out)
    cmd = TRANSLATORS[name](exe, src, out)
    start = time.perf_counter()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    elapsed = time.perf_counter() - start
    result = {"ms": round(elapsed * 1000, 2)}
    if proc.returncode < 0:
        result.update(status="crash", detail="signal %d" % -proc.returncode)
        return result
    if proc.returncode != 0 or not os.path.exists(out) or os.path.getsize(out) == 0:
        result.update(status="rejected", detail=first_line(proc.stderr + proc.stdout))
        return result
    result["spv_bytes"] = os.path.getsize(out)
    val = subprocess.run([spirv_val, "--target-env", "vulkan1.3"] + VALIDATOR_ARGS.get(name, []) + [out],
                         capture_output=True, text=True)
    if val.returncode != 0:
        result.update(status="invalid_spirv", detail=first_line(val.stderr + val.stdout))
    else:
        result["status"] = "ok"
    return result


def collect_inputs(args, workdir):
    if args.corpus:
        files = sorted(f for f in os.listdir(args.corpus)
                       if os.path.isfile(os.path.join(args.corpus, f)))
        return [(f, os.path.join(args.corpus, f)) for f in files], True
    inputs = []
    case_dir = os.path.join(workdir, "cases")
    os.makedirs(case_dir)
    for name, source in shader_cases.all_cases().items():
        path = os.path.join(case_dir, name + ".bin")
        with open(path, "wb") as f:
            f.write(d3d9_asm.assemble(source))
        inputs.append((name, path))
    return inputs, False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--vkd3d")
    parser.add_argument("--mojoshader")
    parser.add_argument("--dxbc-spirv", dest="dxbc_spirv")
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val") or "spirv-val")
    parser.add_argument("--corpus", help="folder of D3D9 bytecode files instead of the built-in cases")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--out", help="JSON report path (default: stdout)")
    args = parser.parse_args(argv)
    exes = {"vkd3d-shader": args.vkd3d, "mojoshader": args.mojoshader, "dxbc-spirv": args.dxbc_spirv}
    exes = {k: v for k, v in exes.items() if v}
    if not exes:
        parser.error("give at least one translator")

    with tempfile.TemporaryDirectory(prefix="shader-spike-") as workdir:
        inputs, counts_only = collect_inputs(args, workdir)
        per_case = {}
        summary = {name: collections.Counter() for name in exes}
        reasons = {name: collections.Counter() for name in exes}
        timing = {name: [] for name in exes}
        sizes = {name: 0 for name in exes}
        for case, path in inputs:
            row = {}
            for name, exe in exes.items():
                res = run_one(name, exe, path, workdir, args.spirv_val, args.timeout)
                row[name] = res
                summary[name][res["status"]] += 1
                if res["status"] != "ok":
                    reasons[name][res.get("detail", res["status"])] += 1
                if "ms" in res:
                    timing[name].append(res["ms"])
                sizes[name] += res.get("spv_bytes", 0)
            if not counts_only:
                per_case[case] = row

    report = {"shaders": len(inputs), "translators": {}}
    for name in exes:
        t = sorted(timing[name])
        report["translators"][name] = {
            "status": dict(summary[name]),
            "failure_reasons": dict(reasons[name].most_common()),
            "ms_median": t[len(t) // 2] if t else None,
            "ms_max": t[-1] if t else None,
            "spv_bytes_total": sizes[name],
        }
    if not counts_only:
        report["cases"] = per_case
    text = json.dumps(report, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

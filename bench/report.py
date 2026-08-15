#!/usr/bin/env python3
"""re-derive the numbers quoted in README.md and ANALYSIS.md.

runs the pinned corpus through each mode, checks every output renders identically
with gifdiff when it is on PATH, and prints the table. this is what makes the
claims checkable by somebody who is not us.

usage: report.py [--binary build/optigif] [--jobs N]
"""
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(HERE, "corpus")
WORK = "/tmp/optigif_report"

MODES = [
    ("copy", ["--copy"]),
    ("recompress", []),
    ("-O", ["-O"]),
    ("-O --strip", ["-O", "--strip"]),
    ("-O -s --strip", ["-O", "-s", "--strip"]),
]


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    binary = os.path.join(ROOT, arg("--binary", "build/optigif"))
    threads = arg("--jobs", "0")
    if not os.path.exists(binary):
        sys.exit(f"no binary at {binary}, build it first")
    with open(os.path.join(HERE, "corpus.manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    files = [os.path.join(CORPUS, e["file"]) for e in manifest]
    for path in files:
        if not os.path.exists(path):
            sys.exit("corpus incomplete, run bench/fetch_corpus.py")
    os.makedirs(WORK, exist_ok=True)
    gifdiff = shutil.which("gifdiff")

    original = sum(e["bytes"] for e in manifest)
    load = os.getloadavg()[0] if hasattr(os, "getloadavg") else 0.0
    print(f"corpus: {len(files)} files, {original} bytes")
    # sizes are deterministic, wall clock is not: say what the machine was doing
    print(f"load average at start: {load:.2f}\n")
    print(f"{'mode':16s} {'total':>10} {'vs original':>12} {'time':>8}  lossless")
    for label, flags in MODES:
        total, seconds, bad = 0, 0.0, []
        for path in files:
            out = os.path.join(WORK, os.path.basename(path))
            start = time.perf_counter()
            run = subprocess.run([binary, "-q", *flags, "-j", threads, path, out],
                                 capture_output=True)
            seconds += time.perf_counter() - start
            if run.returncode != 0:
                bad.append(os.path.basename(path) + " (failed)")
                continue
            # a recompressor only keeps what is smaller, so score it that way
            total += min(os.path.getsize(out), os.path.getsize(path))
            if gifdiff and subprocess.run([gifdiff, "-w", "-q", path, out],
                                          capture_output=True).returncode != 0:
                bad.append(os.path.basename(path))
        verdict = "yes" if gifdiff and not bad else ("SEE BELOW" if bad else "not checked")
        print(f"{label:16s} {total:10d} {100 * (total / original - 1):11.2f}% "
              f"{seconds:7.1f}s  {verdict}")
        for name in bad:
            print(f"    differs: {name}")
    print("\nthreads used for the search: " + ("all cores" if threads == "0" else threads))
    if hasattr(os, "getloadavg"):
        print(f"load average at end: {os.getloadavg()[0]:.2f}; "
              "timings only mean something on an otherwise idle machine")
    if not gifdiff:
        print("gifdiff not on PATH, so nothing was verified; build it from gifsicle")


main()

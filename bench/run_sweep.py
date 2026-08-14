#!/usr/bin/env python3
"""run stage_bench over a corpus and collect sizes/timings as jsonl.

usage: run_sweep.py <mode> <out.jsonl> [--max-pixels N] [--jobs N] [--timeout S]
                    [--max-tokens N]
"""
import concurrent.futures as cf
import json, os, subprocess, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "stage_bench")
CORPUS = os.path.join(ROOT, "bench", "corpus")
TMP = "/tmp/gb/work"

def arg(name, default):
    return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default

def main():
    mode = sys.argv[1]
    outpath = sys.argv[2]
    max_pixels = arg("--max-pixels", 0)
    jobs = arg("--jobs", 1)
    timeout = arg("--timeout", 0)
    max_tokens = arg("--max-tokens", 10000)

    stats = {}
    with open("/tmp/gb/stats.jsonl") as f:
        for line in f:
            r = json.loads(line)
            stats[r["file"]] = r

    files = sorted(glob.glob(os.path.join(CORPUS, "*.gif")))
    if max_pixels:
        files = [f for f in files
                 if stats.get(os.path.basename(f), {}).get("pixels", 1 << 62) <= max_pixels]
    os.makedirs(TMP, exist_ok=True)

    def run(path):
        name = os.path.basename(path)
        out = os.path.join(TMP, f"{mode}_{max_tokens}_{name}")
        cmd = [BIN, mode, path, out, str(max_tokens)]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=timeout or None)
        except subprocess.TimeoutExpired:
            return {"file": name, "mode": mode, "maxTokens": max_tokens,
                    "status": "timeout", "in": stats[name]["bytes"]}
        if p.returncode != 0:
            return {"file": name, "mode": mode, "maxTokens": max_tokens,
                    "status": "error", "err": p.stderr.strip()[:200],
                    "in": stats[name]["bytes"]}
        r = json.loads(p.stdout)
        r["status"] = "ok"
        return r

    results = []
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for r in ex.map(run, files):
            results.append(r)
            print(json.dumps(r), flush=True)
    with open(outpath, "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")

main()

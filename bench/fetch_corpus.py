#!/usr/bin/env python3
"""fetch the benchmark corpus listed in bench/corpus.manifest.json.

the manifest pins a name, a size and a sha256 for every file, so the corpus a
reader downloads is the corpus the numbers in README.md and ANALYSIS.md were
measured on. a file that does not match its checksum is an error, never a warning:
wikimedia can and does replace files behind the same url.

usage: fetch_corpus.py [--verify-only] [--jobs N]
"""
import concurrent.futures as cf
import hashlib
import json
import os
import sys
import time
import urllib.request

UA = "gifoutcpp-corpus/1.0 (https://github.com/Snesnopic/gifoutcpp)"
HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "corpus.manifest.json")
CORPUS = os.path.join(HERE, "corpus")


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check(entry):
    path = os.path.join(CORPUS, entry["file"])
    if not os.path.exists(path):
        return "missing"
    if os.path.getsize(path) != entry["bytes"]:
        return "wrong size"
    if digest(path) != entry["sha256"]:
        return "wrong checksum"
    return None


def fetch(entry):
    path = os.path.join(CORPUS, entry["file"])
    if check(entry) is None:
        return entry["file"], None
    for attempt in range(5):
        try:
            req = urllib.request.Request(entry["url"], headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=180) as r, open(path, "wb") as f:
                f.write(r.read())
            break
        except Exception as exc:  # noqa: BLE001 - report whatever the network did
            if attempt == 4:
                return entry["file"], f"download failed: {exc}"
            time.sleep(3 * (attempt + 1))
    return entry["file"], check(entry)


def main():
    verify_only = "--verify-only" in sys.argv
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 4

    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)
    os.makedirs(CORPUS, exist_ok=True)

    if verify_only:
        problems = [(e["file"], why) for e in manifest if (why := check(e))]
    else:
        with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
            problems = [(name, why) for name, why in pool.map(fetch, manifest) if why]

    total = sum(e["bytes"] for e in manifest)
    print(f"{len(manifest) - len(problems)}/{len(manifest)} files ok, "
          f"{total / 1e6:.1f} MB total")
    for name, why in problems:
        print(f"  {name}: {why}")
    return 1 if problems else 0


sys.exit(main())

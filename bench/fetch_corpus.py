#!/usr/bin/env python3
"""collect a stratified sample of real GIFs from wikimedia commons."""
import json, random, sys, time, urllib.parse, urllib.request, os, hashlib

UA = "gifoutcpp-research/0.1 (snesnopic@gmail.com)"
API = "https://commons.wikimedia.org/w/api.php"
OUT = os.path.expanduser("~/CLionProjects/gifoutcpp/bench/corpus")

def api(params):
    params = dict(params, format="json")
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)

def collect(offsets):
    out = {}
    for off in offsets:
        try:
            d = api({"action": "query", "generator": "search",
                     "gsrsearch": "filemime:gif", "gsrnamespace": 6,
                     "gsrlimit": 50, "gsroffset": off,
                     "prop": "imageinfo", "iiprop": "url|size|mime|metadata"})
        except Exception as e:
            print("warn:", e, file=sys.stderr); continue
        for p in d.get("query", {}).get("pages", {}).values():
            ii = p.get("imageinfo", [{}])[0]
            if ii.get("mime") != "image/gif":
                continue
            frames = None
            for m in ii.get("metadata") or []:
                if m.get("name") == "GIFFileComment":
                    pass
                if m.get("name") == "frameCount":
                    frames = m.get("value")
            out[p["title"]] = (ii["size"], ii["url"], frames)
        time.sleep(0.3)
    return out

BUCKETS = [("xs", 4_000, 20_000, 10),
           ("s", 20_000, 100_000, 10),
           ("m", 100_000, 500_000, 10),
           ("l", 500_000, 2_000_000, 8),
           ("xl", 2_000_000, 8_000_000, 5)]

def main():
    random.seed(20260813)
    offsets = [0] + [random.randrange(0, 9000) for _ in range(30)]
    pool = collect(offsets)
    print(f"pool: {len(pool)} gifs", file=sys.stderr)
    os.makedirs(OUT, exist_ok=True)
    manifest = []
    for name, lo, hi, n in BUCKETS:
        cands = [(t, s, u, f) for t, (s, u, f) in pool.items() if lo <= s < hi]
        random.shuffle(cands)
        picked = cands[:n]
        print(f"{name}: {len(picked)}/{len(cands)} available", file=sys.stderr)
        for i, (title, size, url, frames) in enumerate(picked):
            safe = f"{name}{i:02d}_" + "".join(
                c if c.isalnum() or c in "._-" else "_"
                for c in urllib.parse.unquote(title[5:]))[:80]
            if not safe.lower().endswith(".gif"):
                safe += ".gif"
            dest = os.path.join(OUT, safe)
            if os.path.exists(dest) and os.path.getsize(dest) == size:
                manifest.append({"file": safe, "size": size, "src": url, "frames": frames})
                continue
            ok = False
            for attempt in range(6):
                try:
                    req = urllib.request.Request(url, headers={"User-Agent": UA})
                    with urllib.request.urlopen(req, timeout=180) as r, open(dest, "wb") as f:
                        f.write(r.read())
                    ok = True
                    break
                except Exception as e:
                    print("retry", safe, e, file=sys.stderr)
                    time.sleep(5 * (attempt + 1))
            if not ok:
                print("dl fail", safe, file=sys.stderr)
                continue
            manifest.append({"file": safe, "size": os.path.getsize(dest),
                             "src": url, "frames": frames})
            time.sleep(1.5)
    with open(os.path.join(OUT, "MANIFEST.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"downloaded {len(manifest)} files, "
          f"{sum(m['size'] for m in manifest)/1e6:.1f} MB", file=sys.stderr)

main()

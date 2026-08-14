#!/usr/bin/env python3
"""dump the top-level block structure of a gif (what each byte range is spent on)."""
import sys, collections

def blocks(d):
    i = 6
    flags = d[10]
    if flags & 0x80:
        i = 13 + 3 * (2 << (flags & 7))
    else:
        i = 13
    yield ("header+gct", 0, i)
    while i < len(d):
        b = d[i]
        if b == 0x3B:
            yield ("trailer", i, i + 1); i += 1
        elif b == 0x21:
            label = d[i + 1]
            j = i + 2
            while j < len(d) and d[j]:
                j += d[j] + 1
            j += 1
            names = {0xF9: "gce", 0xFE: "comment", 0x01: "plaintext", 0xFF: "appext"}
            name = names.get(label, f"ext{label:02X}")
            if name == "appext":
                name += ":" + d[i + 3:i + 3 + 11].decode("latin1", "replace").strip()
            yield (name, i, j); i = j
        elif b == 0x2C:
            lf = d[i + 9]
            j = i + 10
            if lf & 0x80:
                j += 3 * (2 << (lf & 7))
            j += 1  # min code size
            while j < len(d) and d[j]:
                j += d[j] + 1
            j += 1
            yield ("image", i, j); i = j
        else:
            yield ("junk", i, len(d)); return

def main():
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        agg = collections.Counter()
        for name, a, b in blocks(d):
            agg[name] += b - a
        print(path.split("/")[-1], len(d))
        for k, v in agg.most_common():
            print(f"   {k:24s} {v:9d}")

main()

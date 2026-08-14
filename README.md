# gifoutcpp

Lossless GIF recompression in C++20. One parse, one in-memory model, one writer:
the structural optimizer and the LZW optimizer work on the same data instead of
handing files back and forth.

The project exists because the two established tools each solve one half of the
problem and cannot see the other half. [gifsicle](https://github.com/kohler/gifsicle)
optimizes the *structure* of an animation (frame bounding boxes, disposal,
transparency, palettes) but encodes LZW greedily. [flexiGIF](https://create.stephan-brumme.com/flexigif-lossless-gif-lzw-optimization/)
finds near-optimal LZW dictionary restart points but copies the container
verbatim, so it can never touch a palette or a frame boundary. Measured on 51 real
GIFs, the two gains compose: `gifsicle -O3` alone is −9.46 %, `gifsicle -O3` followed by the LZW
optimizer is −12.12 %.

`ANALYSIS.md` documents the study behind those choices: what each codebase does,
what it does well, and what is worth porting, with the measurements that justify
each verdict.

## Two levels of lossless

GIF admits two different invariants, and a tool has to say which one it promises.

| level | what stays identical | example transformation |
|---|---|---|
| **L1 — structure** | frame count, per-frame geometry, indices, palette | re-encoding the LZW bitstream |
| **L2 — rendering** | the animation as played: pixels over time, total duration | cropping a frame to its bounding box, merging a redundant frame, pruning unused palette entries |

L2 contains L1. Consumers that verify by comparing frame structure (chisel's
`raw_equal`, for one) need L1; consumers that only care about what the viewer sees
can allow L2 and get considerably more. The level is an explicit option, never an
implicit default.

## Status

Early, but the whole pipeline is in place: read, restructure, search the LZW
restart points, write.

What is verified today, on a corpus of 51 real GIFs from Wikimedia Commons
(31.6 MB, 136.5 M pixels, 1 to 475 frames). The corpus is pinned by name, size and
SHA-256 in `bench/corpus.manifest.json`; `bench/fetch_corpus.py` downloads exactly
that and refuses anything whose checksum moved, and `bench/report.py` re-derives the
table below and checks every output with `gifdiff`. Sizes are deterministic;
timings were taken on an otherwise idle M2 Pro and mean nothing on a loaded machine:

* **Decoder agrees with gifsicle on 51/51 files** — frame geometry, delays,
  disposal, transparency, local colormap sizes and every decoded pixel index
  (`bench/decode_check`).
* **Read/write round trip is byte-identical on 50/51 files.** The one exception is
  a file carrying 3086 bytes of zero padding after the GIF trailer, which is
  dropped on purpose; gifsicle drops it too.
* **All 51 round-trip outputs are L1-equivalent** to their input.
* **The greedy encoder reproduces gifsicle's LZW output byte for byte on 51/51
  files** (`bench/encode_check`), so the ported run-length heuristic is exact.
* Recompressing the corpus gives **−2.62 %** at ~80 Mpixel/s end to end, with all
  51 outputs L1-equivalent and `gifdiff`-identical. That matches what gifsicle's
  own encoder achieves, which is the point: it is the baseline the optimal LZW
  search has to beat.
* `-s` searches the dictionary restart points instead of guessing them. On the
  29-file subset the established tools can finish, it gives **−11.29 %** against
  **−8.75 %** for the greedy encoder, is **never larger than flexiGIF at the same
  alignment** and smaller on 9 of them, at **1.8x** its speed.
* `-O -s` together give **−12.03 % in 16 s** on that subset, against **−11.43 %**
  for gifsicle piped into flexiGIF, which spends about 737 s in flexiGIF alone.
* On the **whole** corpus, which the established pipeline cannot finish at all,
  `-O -s --strip` gives **−11.68 %** in **1.08 minutes on ten threads**,
  byte-identical whatever the thread count, all 51 rendering-identical.
* With `-O` the corpus goes to **−8.00 % in 7.1 s**, against **−8.04 % in 4.8 s** for
  `gifsicle -O3`; adding `--strip` reaches **−8.06 %**, slightly ahead of it.
  **All 51 outputs are rendering-identical** (`gifdiff -w`), and 360 mutated
  inputs go through `-O` under ASan/UBSan without a crash.

For reference, the reader accepts the whole corpus, including the four files
flexiGIF's parser rejects outright.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Two targets, mirroring the layout of `flacoutcpp`:

* `gifout_core` — the library: model, reader, writer. No CLI, no I/O policy.
* `gifoutcpp` — a thin CLI on top of the core.

The research harness under `bench/` is off by default because it links gifsicle
and flexiGIF out of a [chisel](https://github.com/Snesnopic/chisel) checkout:

```
cmake -S . -B build -DGIFOUT_BUILD_BENCH=ON -DCHISEL_ROOT=../chisel
```

## Using it as a library

```cpp
#include <gifoutcpp/gifoutcpp.hpp>

gifout::Options options;
options.restructure = true;      // crop frames, pick disposal, rebuild palettes
options.search_restarts = true;  // search the lzw restart points
options.search.threads = 0;      // 0 means as many as the machine has

const auto r = gifout::recompress_file("in.gif", "out.gif", options);
if (r.ok && r.smaller()) { /* keep it */ }
```

`recompress` does the same in memory, and `recompress_stream` takes a `Stream` the
caller already has, so a consumer can inspect or edit frames between the stages.
Everything reachable from `gifoutcpp/gifoutcpp.hpp` is the supported surface;
anything under `src/` is not.

```
cmake --install build --prefix /somewhere
find_package(gifoutcpp REQUIRED)
target_link_libraries(app PRIVATE gifoutcpp::core)
```

CI installs the library and builds an external consumer against it on every push, so
the packaging is checked rather than assumed.

## Usage

```
gifoutcpp [options] <input.gif> [output.gif]

  -O, --optimize    rebuild frames and palettes, keeping only the rendered result
  -s, --search      search for the best dictionary restart points, much slower
      --alignment N how far apart restart points may sit, in pixels (default 160)
      --max-tokens N how far a block is explored (default 10000)
      --strip       drop comments and application metadata, keep the loop block
      --deinterlace drop interlacing, which costs size and only helps slow links
  -i, --info        report structure and diagnostics, write nothing
  -c, --copy        copy the lzw data instead of re-encoding it
      --careful     take the code size from the palette, not from the pixels
      --eager-clear restart the dictionary as soon as it fills
      --both-clears try both restart policies and keep the smaller
      --reblock     re-chunk copied lzw data at 255 bytes
  -q, --quiet       only report errors
  -h, --help        this message
  -v, --version     version number
```

`--alignment` is the knob that matters for `-s`: the cost is linear in its inverse,
and 10 rather than 160 costs 15 to 33 times the time for 0.15 % of size. That is why
the default is 160 and not flexiGIF's 10.

`--both-clears` is what gifsicle only turns on at `gifsicle -O3`. Measured over the corpus it
buys 0.004 % for 1.5x the time, so it is an option rather than the default.

## Design notes

**Tolerant reading.** Real GIFs are broken in ways strict parsers reject: trailing
junk, truncated streams, out-of-range code sizes, palette size bits set with no
palette present. The reader repairs and reports through a diagnostics list rather
than throwing, because a file that a viewer displays fine has to be readable.

**Byte-exact by default.** The model keeps the sub-block split of the LZW data, the
position of the graphic control extension among the other extensions, and even
bits that carry no meaning (a transparent index stored with the transparency flag
off). None of it matters once the file is re-encoded, but preserving it makes an
untouched round trip provably a no-op, which is the strongest available test of
reader and writer.

**`-O` can never make a file larger.** Restructuring loses on some inputs, so the
plain recompression is computed as well and the smaller of the two is written.

**Four transparency variants, and the encoder picks.** Turning every unchanged pixel
transparent makes photographs 18 % *larger*: it breaks runs of one colour into
fragments. The frame is therefore built four ways — untouched, gifsicle's rule that
only replaces a run whose colour differs from the last index emitted, that rule plus
lone matching pixels, and the blunt "every run of two or more" — and each is encoded
so the smallest one wins. This is why the greedy encoder had to exist before the
optimizer did.

**Metadata removal is a policy, not an optimization.** `--strip` drops comments and
application blocks (keeping the loop block, which is animation rather than metadata).
One file in the corpus carries 3966 bytes of XMP; nothing about the picture changes
either way, so the tool does not decide this on its own.

**The transparent index is per frame, not a reserved slot.** It only has to be an
index that frame never paints with, and a frame rarely uses the whole table. Setting
one aside globally instead is what makes a 256-colour animation overflow by exactly
one entry and fall back to a local palette per frame, which on one 44-frame file cost
33 KB on its own. A single spare slot serves every frame that needs one, because no
pixel ever maps to it; adding one per frame instead grew a 32-colour palette to 256.

**The background is transparent only if the first frame says so.** A GIF shows its
background colour under uncovered area only when the first frame declares no
transparent index; otherwise that area is transparent. Cropping a frame can expose
area that was covered before, so the emitted stream has to keep the declaration even
when no pixel uses it. It is carried on an index the frame never uses, which costs
nothing; only when every index is taken does the palette grow by one entry.

**The search and the decoder count codes the same way.** A decoder defines a
dictionary entry for every code it receives, including the last one of a block. An
encoder that ends a block mid-match has nothing to insert, but it must still advance
its own count, or the two disagree about when the code width grows and the stream
desynchronises a few thousand pixels later. This cost two corrupt files before it
was found, and the same latent bug was in the greedy encoder.

**A variant is a bitmask, not a second copy.** The four candidate images a frame is
built as differ from each other only by which pixels go transparent, so three of them
are one bit per pixel rather than a full copy. That took the optimizer's peak from 17
to 9 bytes per pixel, 511 MB down to 270 MB on the longest animation in the corpus,
with byte-identical output.

**A declared size is not a promise.** A frame states its dimensions in four bytes, so
a 35-byte file can ask the decoder for 4.3 G pixels; before this was bounded it
really did allocate 2.4 GB. `ReadOptions::max_frame_pixels` (256 M by default) keeps
such a frame compressed and undecoded, and says so in the diagnostics. The fuzzer
found this, and fixing it also made the fuzzer 20 times faster.

**A damaged frame is never re-encoded.** If the decoder had to repair a frame's LZW
stream, its pixels no longer describe the same image the file carries, so the
original bytes are copied through instead. Recompressing them would silently change
the picture.

**The palette is ordered by what each frame paints.** A frame's code size follows its
highest index, so the order of the global colour table decides how many bits every
frame pays. Ordering it by the colours of the variant a frame is likely to keep, and
serving the frames with the fewest colours first, is what closed the last measurable
gap on the longest animation in the corpus.

**The search prunes only where it can prove it is safe.** Accumulated bits never go
down and the cost of any remaining tail is non-negative, so once a block alone is
worse than the best complete path already found, no longer block can win and the walk
stops. It is worth about 7 %, far less than the parallelism, and it cannot change the answer
by construction. A tighter bound was measured rather than attempted: instrumenting
the walk shows the winning end sits at 72.6 % of how far it goes, so a perfect oracle
would save at most a quarter of the walk, and less in wall clock because the pruned
tail is the part where the dictionary is already warm.

**Parallelism is an optimization, never a different answer.** The search splits a
frame into chunks of restart positions; the forward walks inside a chunk never read
the part of the cost table the chunk is still computing, so they are independent and
only the small fold at the end is sequential. Each worker gets its own dictionary, no
locks are involved, and `-j 1` runs a plain loop with no threads created at all. CI
checks that `-j 1` and `-j 0` produce identical bytes, and the tests run under both
the address and the thread sanitizer.

## Levels, and a note on names

`gifsicle -O1/-O2/-O3` in this document always means gifsicle's *structural*
optimization level, never a compiler flag: `-O1` crops each frame to what changes
and picks a disposal, `-O2` also turns pixels equal to the previous frame
transparent, `-O3` also tries a second transparency variant and an eager dictionary
clear, keeping whichever compresses smaller.

## License

MIT, see `LICENSE`. No code was copied: the algorithms here were re-implemented
from studying how the two established tools work, and `ANALYSIS.md` records that
study. Credit where it is due, though — the run-length heuristic that decides
dictionary restarts and the transparency substitution rule are Eddie Kohler's, from
[gifsicle](https://github.com/kohler/gifsicle) (GPL-2.0), and the idea of searching
for optimal restart points is Stephan Brumme's, from
[flexiGIF](https://create.stephan-brumme.com/flexigif-lossless-gif-lzw-optimization/).

The instrumented copy of flexiGIF's encoder under `bench/instrumented/` is derived
from third-party sources and is deliberately not committed; it is regenerated
locally by `bench/instrumented/instrument.py`.

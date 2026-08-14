# gifoutcpp

Lossless GIF recompression in C++20. One parse, one in-memory model, one writer: the
structural optimizer and the LZW optimizer work on the same data instead of handing
files back and forth.

The project exists because the two established tools each solve one half of the
problem and cannot see the other half. [gifsicle](https://github.com/kohler/gifsicle)
optimizes the *structure* of an animation (frame bounding boxes, disposal,
transparency, palettes) but encodes LZW greedily.
[flexiGIF](https://create.stephan-brumme.com/flexigif-lossless-gif-lzw-optimization/)
searches for near-optimal LZW dictionary restart points but copies the container
verbatim, so it can never touch a palette or a frame boundary. The two gains compose,
and doing both on one model does better than piping one into the other.

`ANALYSIS.md` documents the study behind every design choice: what each codebase does,
what it does well, and what was worth porting, with the measurements that justify each
verdict.

## What it is, and what it is not

It replaces the **lossless recompression stage**, and it is measurably better at that
than either tool it learned from.

It is **not** a replacement for gifsicle, which is an editor: gifsicle also resizes,
crops, rotates, quantizes colours (lossy), merges files, extracts and deletes frames,
rewrites delays and loop counts by hand, and ships a viewer and a comparison tool.
gifoutcpp does exactly one of those jobs. If you want `--colors 64 --resize 50%`, you
want gifsicle.

## Two levels of lossless

GIF admits two different invariants, and a tool has to say which one it promises.

| level | what stays identical | example transformation |
|---|---|---|
| **L1 — structure** | frame count, per-frame geometry, indices, palette | re-encoding the LZW bitstream |
| **L2 — rendering** | the animation as played: pixels over time, total duration | cropping a frame to its bounding box, merging a redundant frame, pruning unused palette entries, dropping interlacing |

L2 contains L1. Consumers that verify by comparing frame structure need L1; consumers
that only care about what the viewer sees can allow L2 and get considerably more.
`--level` picks, and L2 is the default.

## Results

Corpus: 51 real GIFs from Wikimedia Commons, 31.6 MB, 136.5 M pixels, 1 to 475 frames,
stratified 4 KB to 4 MB, animated and static, hand-drawn, photographic, scientific and
scanned. Pinned by name, size and SHA-256 in `bench/corpus.manifest.json`.
Machine: Apple M2 Pro, 10 cores, Release.

| mode | total | vs original | time |
|---|---|---|---|
| original | 31 625 992 B | — | — |
| `--copy` (container rewrite only) | 31 622 906 B | −0.01 % | 0.1 s |
| default (re-encode LZW) | 30 798 920 B | −2.62 % | 1.6 s |
| `gifsicle -O3`, for reference | 29 082 967 B | −8.04 % | 4.8 s |
| `-O --strip` | 29 024 849 B | **−8.22 %** | 6.8 s, 3.8 s at `-j 0` |
| `-O --strip --lookahead 4` | 28 989 881 B | **−8.34 %** | 29.7 s |
| `-O -s --strip -j 0` | 27 871 612 B | **−11.87 %** | 1.8 min |

On the 29-file subset where the established pipeline can finish at all, gifsicle piped
into flexiGIF gives −11.43 % and spends about 737 s inside flexiGIF; `-O -s` gives
**−12.03 % in 16 s**. On the whole corpus that pipeline does not finish, so −11.87 % is
a number that did not previously exist.

`bench/report.py` re-derives this table and checks every output with `gifdiff`. Sizes
are deterministic; timings mean nothing on a loaded machine.

## Correctness

* **The decoder agrees with gifsicle on 51/51 files** — frame geometry, delays,
  disposal, transparency, local colormap sizes and every decoded pixel index.
* **The greedy encoder reproduces gifsicle's LZW output byte for byte on 51/51 files**,
  so the ported run-length heuristic is exact rather than approximately right.
* **A read/write round trip is byte-identical on 50/51 files.** The exception carries
  3086 bytes of zero padding after the GIF trailer, dropped on purpose.
* **Every optimized output is rendering-identical** (`gifdiff -w`), at every setting.
* 911 unit checks, run also under AddressSanitizer, UndefinedBehaviorSanitizer and
  ThreadSanitizer; two libFuzzer targets; a fuzz run and an external-consumer build on
  every push, across 11 platforms.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Two targets: `gifout_core` (the library) and `gifoutcpp` (a thin CLI over it). The
research harness under `bench/` is off by default because it links gifsicle and
flexiGIF out of a [chisel](https://github.com/Snesnopic/chisel) checkout:
`-DGIFOUT_BUILD_BENCH=ON -DCHISEL_ROOT=../chisel`. The fuzz targets need a clang that
ships the runtime: `-DGIFOUT_BUILD_FUZZERS=ON`, see `fuzz/README.md`.

## Usage

```
gifoutcpp [options] <input.gif> [output.gif]

  -O, --optimize      rebuild frames and palettes, keeping only the rendered result
  -u, --unoptimize    expand every frame back to full screen, disposal applied
      --level L       l1 keeps the structure and only re-encodes, l2 (default) may
                      rebuild it as long as the animation plays the same
  -s, --search        search for the best dictionary restart points, much slower
      --alignment N   how far apart restart points may sit, in pixels (default 160)
      --max-tokens N  how far a block is explored (default 10000)
  -j, --threads N     threads for the search and for -O, 0 means as many as the
                      machine has
                      (default 1; the output is identical whatever you pick)
      --strip         drop comments and application metadata, keep the loop block
  -x, --exhaustive N  search the parse itself with a beam N wide
      --lookahead N   try N match lengths per token instead of only the longest
  -b, --best          try the settings that win on some files and lose on others,
                      and keep whichever came out smallest
      --keep-interlace  keep interlacing, which is otherwise dropped
      --careful       take the code size from the palette, not from the pixels
      --eager-clear   restart the dictionary as soon as it fills
      --both-clears   try both restart policies and keep the smaller
  -c, --copy          copy the lzw data instead of re-encoding it
      --reblock       re-chunk copied lzw data at 255 bytes
  -i, --info          report structure and diagnostics, write nothing
  -q, --quiet         only report errors
```

What each knob is worth, measured over the corpus, so the cost is a decision rather
than a guess:

| knob | worth | cost |
|---|---|---|
| `-O` | −5.4 pp | seconds |
| `-s` | −3.6 pp | minutes |
| dropping interlacing (on by default) | −0.15 % | free |
| `--lookahead 4` without `-s` | −0.11 % | 4x |
| `--lookahead 4` with `-s` | −0.008 % | 1.2x |
| `--alignment` 160 to 10 | −0.16 % | 16x |
| `--both-clears` | −0.004 % | 1.5x |
| `-b` | −0.0003 % | 4x |
| `--careful` | **+0.009 %** | free |

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
Everything reachable from `gifoutcpp/gifoutcpp.hpp` is the supported surface; anything
under `src/` is not.

```
cmake --install build --prefix /somewhere
find_package(gifoutcpp REQUIRED)
target_link_libraries(app PRIVATE gifoutcpp::core)
```

CI installs the library and builds an external consumer against it on every push, so
the packaging is checked rather than assumed.

## Design notes

Each of these was forced by a measurement, and most of them cost a wrong turn first.

**Tolerant reading.** Real GIFs are broken in ways strict parsers reject: trailing
junk, truncated streams, out-of-range code sizes, palette size bits set with no palette
present. The reader repairs and reports through a diagnostics list rather than
throwing. It accepts the whole corpus, including the four files flexiGIF rejects.

**A declared size is not a promise.** A frame states its dimensions in four bytes, so a
35-byte file can ask the decoder for 4.3 G pixels; before this was bounded it really
did allocate 2.4 GB. `ReadOptions::max_frame_pixels` keeps such a frame compressed and
undecoded. The fuzzer found this, and fixing it also made the fuzzer 20 times faster.

**Byte-exact by default.** The model keeps the sub-block split of the LZW data, the
position of the graphic control extension among the other extensions, and even bits
that carry no meaning, such as a transparent index stored with the transparency flag
off. None of it matters once the file is re-encoded, but preserving it makes an
untouched round trip provably a no-op, which is the strongest available test of reader
and writer.

**Four transparency variants, and the encoder picks.** Turning every unchanged pixel
transparent makes photographs 18 % *larger*: it breaks runs of one colour into
fragments. The frame is therefore built four ways — untouched, gifsicle's rule that
only replaces a run whose colour differs from the last index emitted, that rule plus
lone matching pixels, and the blunt "every run of two or more" — and each is encoded so
the smallest one wins. This is why the greedy encoder had to exist before the optimizer
did.

**The transparent index is per frame, not a reserved slot.** It only has to be an index
that frame never paints with. Setting one aside globally is what makes a 256-colour
animation overflow by exactly one entry and fall back to a local palette per frame,
which on one 44-frame file cost 33 KB. A single spare slot serves every frame that
needs one; adding one per frame instead grew a 32-colour palette to 256.

**The background is transparent only if the first frame says so.** A GIF shows its
background colour under uncovered area only when the first frame declares no
transparent index; otherwise that area is transparent. Cropping can expose area that
was covered before, so the emitted stream has to keep the declaration even when no
pixel uses it.

**The palette is ordered by what each frame paints.** A frame's code size follows its
highest index, so the order of the global colour table decides how many bits every
frame pays. Ordering it by the colours of the variant a frame is likely to keep, and
serving the frames with the fewest colours first, closed the last measurable gap
against gifsicle on the longest animation in the corpus.

**Interlacing is dropped by default.** It costs size on every file that has it and buys
only progressive display over a slow link. That is a rendering-level change, so it
never happens at `--level l1`.

**Metadata removal is a policy, not an optimization.** `--strip` drops comments and
application blocks, keeping the loop block, which is animation rather than metadata.
One file in the corpus carries 3966 bytes of XMP; nothing about the picture changes
either way, so the tool does not decide this on its own.

**The search and the decoder count codes the same way.** A decoder defines a dictionary
entry for every code it receives, including the last one of a block. An encoder that
ends a block mid-match has nothing to insert but must still advance its own count, or
the two disagree about when the code width grows and the stream desynchronises a few
thousand pixels later. This cost two corrupt files before it was found, and the same
latent bug was in the greedy encoder.

**A damaged frame is never re-encoded.** If the decoder had to repair a frame's LZW
stream, its pixels no longer describe the same image the file carries, so the original
bytes are copied through instead.

**`-O` can never make a file larger.** Restructuring loses on some inputs, so the plain
recompression is computed as well and the smaller of the two is written.

**There is no exhaustive mode, and there cannot be.** `-x` searches the parse itself,
keeping several ways of having reached each position with the dictionary each one
built. It is not exhaustive: an LZW encoder's state is the position *and* the
dictionary that got it there, two parses meeting at a position almost never carry the
same dictionary, so the paths never merge and the state count grows exponentially —
unlike a codec whose DP state is just a position. What `-x` offers is as wide a search
as you are willing to pay for, made monotone by construction: it runs every width up to
the one asked for and keeps the smallest, with the greedy encoder as a floor, so more
effort can never return a larger file.

**The search prunes only where it can prove it is safe.** Accumulated bits never go
down and the cost of any remaining tail is non-negative, so once a block alone is worse
than the best complete path already found, no longer block can win and the walk stops.
A tighter bound was measured rather than attempted: the winning end sits at 72.6 % of
how far the walk goes, so a perfect oracle would save at most a quarter of it.

**Parallelism is an optimization, never a different answer.** The search splits a frame
into chunks of restart positions; the forward walks inside a chunk never read the part
of the cost table the chunk is still computing, so they are independent and only the
small fold at the end is sequential. Each worker gets its own dictionary, no locks are
involved, and `-j 1` runs a plain loop with no threads created at all. CI checks that
`-j 1` and `-j 0` produce identical bytes.

`-j` also spreads `-O`, where most of the time goes into encoding the transparency
candidates in order to compare them. The unit of work is the pair (frame, candidate)
rather than the frame, because half the corpus is single frame files that would
otherwise stay entirely sequential. Threads only measure; the winner is picked
afterwards from the sizes and a fixed candidate order, so the answer cannot depend on
how the work was split. Measured on the corpus, `-O --strip` goes from **6.8 s to
3.8 s** on 10 cores, **1.8x**, up to **2.7x** on animated files, with byte-identical
output on all 51. What is left is the canvas simulation, which is sequential by nature:
the state a frame is drawn onto is the one the frame before it left behind.

**A variant is a bitmask, not a second copy.** The four candidate images a frame is
built as differ only by which pixels go transparent, so three of them are one bit per
pixel. That took the optimizer's peak from 17 to 9 bytes per pixel, 511 MB down to
270 MB on the longest animation, with byte-identical output.

## Known gaps

* **Endianness is unverified.** CI compiles on 11 platforms but runs the correctness
  gates only on Linux x64, and the reader and writer do binary I/O by hand.
* **The corpus comes from one source.** 51 files from Wikimedia Commons, mostly
  encyclopedic material. GIFs from Photoshop, screen recorders or the 1990s web have
  different encoder habits and might hide both bugs and gains.
* **`-x` is usable on small frames only**: 16 s for a 5 KB file. The dominant cost is
  copying a dictionary per branch of the search.
* **The restart search is O(n²/alignment)**; parallelism made it practical, not cheap.
* About 7 KB of residual on one 475-frame animation, now below the corpus noise.
* No release, no package, no man page; the library API is young and unstable.

## Reproducing the numbers

```
python3 bench/fetch_corpus.py          # downloads and verifies the pinned corpus
python3 bench/report.py                # re-derives the results table
```

`bench/` also holds the study tools: `stage_bench` runs each stage of the established
pipeline in isolation, `decode_check` and `encode_check` compare us against gifsicle
file by file, and `parse_probe` and `beam_probe` are the throwaway experiments that
measured what greedy LZW parsing leaves on the table before any of it was built.

## A note on names

`gifsicle -O1/-O2/-O3` in this document always means gifsicle's *structural*
optimization level, never a compiler flag: `-O1` crops each frame to what changes and
picks a disposal, `-O2` also turns pixels equal to the previous frame transparent, and
`-O3` also tries a second transparency variant and an eager dictionary clear, keeping
whichever compresses smaller.

## License

MIT, see `LICENSE`. No code was copied: the algorithms here were re-implemented from
studying how the two established tools work, and `ANALYSIS.md` records that study.
Credit where it is due, though — the run-length heuristic that decides dictionary
restarts and the transparency substitution rule are Eddie Kohler's, from
[gifsicle](https://github.com/kohler/gifsicle) (GPL-2.0), and the idea of searching for
optimal restart points is Stephan Brumme's, from
[flexiGIF](https://create.stephan-brumme.com/flexigif-lossless-gif-lzw-optimization/).

The instrumented copy of flexiGIF's encoder under `bench/instrumented/` is derived from
third-party sources and is deliberately not committed; it is regenerated locally by
`bench/instrumented/instrument.py`.

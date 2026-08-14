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
GIFs, the two gains compose: `-O3` alone is −9.46 %, `-O3` followed by the LZW
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

Early. The core reads, re-encodes and writes GIFs; the structural optimizer and the
optimal LZW search are not written yet.

What is verified today, on a corpus of 51 real GIFs from Wikimedia Commons
(31.6 MB, 136.5 M pixels, 1 to 475 frames):

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

## Usage

```
gifoutcpp [options] <input.gif> [output.gif]

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

`--both-clears` is what gifsicle only turns on at `-O3`. Measured over the corpus it
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

**A damaged frame is never re-encoded.** If the decoder had to repair a frame's LZW
stream, its pixels no longer describe the same image the file carries, so the
original bytes are copied through instead. Recompressing them would silently change
the picture.

**No mutable global state**, so the library can be used from several threads. When
parallelism arrives it will be optional, with single-threaded output identical bit
for bit.

## License

Not chosen yet. The ported parts of gifsicle are GPL-2.0, which will constrain it.

# Fuzzing

Two libFuzzer targets, both off by default because they need a clang that ships the
runtime (Apple's does not; `brew install llvm` does).

* `fuzz_reader` — arbitrary bytes through the reader and the whole pipeline. Checks
  that nothing crashes and that whatever we write, we can read back without repairs.
* `fuzz_roundtrip` — arbitrary bytes as pixel indices through the encoder and back.
  Checks the invariant the project rests on: the decoder returns the pixels the
  encoder was given, greedy or searched, one thread or four. This is the target that
  would have caught the block boundary desync.

```
CXX=$(brew --prefix llvm)/bin/clang++ cmake -S . -B build-fuzz -DOPTIGIF_BUILD_FUZZERS=ON \
    -DOPTIGIF_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-fuzz
mkdir -p /tmp/corpus
./build-fuzz/fuzz_reader -max_total_time=300 -max_len=65536 /tmp/corpus fuzz/seeds
```

`fuzz/seeds` holds a handful of tiny GIFs written by our own writer, covering an
animation, transparency, a local colormap, interlacing and a comment block. Generate
or refresh them with `build/optigif_tests --write-seeds fuzz/seeds`.

A note on limits: under ASan the default `-rss_limit_mb=2048` is reached by the
fuzzer's own bookkeeping on a long session, not by a single input. Raise it rather
than chasing a phantom leak, and use `-malloc_limit_mb` if you want to catch a single
oversized allocation.

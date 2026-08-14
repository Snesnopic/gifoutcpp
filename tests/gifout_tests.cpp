// self-contained tests: they build their own gifs in memory, so nothing here depends
// on a corpus, on gifsicle or on any file lying around.

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "gifoutcpp/gifoutcpp.hpp"
#include "gifoutcpp/gif_encoder.hpp"
#include "gifoutcpp/gif_lzw.hpp"
#include "gifoutcpp/gif_lzw_search.hpp"
#include "gifoutcpp/gif_optimizer.hpp"
#include "gifoutcpp/gif_reader.hpp"
#include "gifoutcpp/gif_writer.hpp"

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL %s\n", what.c_str());
    }
}

using namespace gifout;

Colormap gray_map(std::size_t n) {
    Colormap map;
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<uint8_t>(i * 255 / (n ? n : 1));
        map.colors.push_back({v, static_cast<uint8_t>(255 - v), static_cast<uint8_t>(i)});
    }
    return map;
}

Frame make_frame(uint16_t w, uint16_t h, std::function<uint8_t(uint16_t, uint16_t)> shade) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.resize(static_cast<std::size_t>(w) * h);
    for (uint16_t y = 0; y < h; ++y)
        for (uint16_t x = 0; x < w; ++x) f.pixels[static_cast<std::size_t>(y) * w + x] = shade(x, y);
    f.pixels_complete = true;
    return f;
}

Stream make_stream(std::vector<Frame> frames, std::size_t colors = 16) {
    Stream s;
    s.screen_width = frames.empty() ? 0 : frames.front().width;
    s.screen_height = frames.empty() ? 0 : frames.front().height;
    s.global = gray_map(colors);
    s.frames = std::move(frames);
    return s;
}

// ----- lzw -----

void test_lzw_round_trip() {
    std::mt19937 rng(1234);
    const std::vector<std::string> kinds{"flat", "stripes", "noise", "runs", "gradient"};
    for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
        for (unsigned bits : {2u, 4u, 8u}) {
            const unsigned colors = 1u << bits;
            const uint16_t w = 61, h = 47;
            auto frame = make_frame(w, h, [&](uint16_t x, uint16_t y) -> uint8_t {
                switch (kind) {
                    case 0: return 0;
                    case 1: return static_cast<uint8_t>((x / 3) % colors);
                    case 2: return static_cast<uint8_t>(rng() % colors);
                    case 3: return static_cast<uint8_t>(((x + y) / 17) % colors);
                    default: return static_cast<uint8_t>((x * y) % colors);
                }
            });
            for (bool interlaced : {false, true}) {
                const auto encoded =
                    encode_lzw(frame.pixels, w, h, interlaced, colors);
                std::vector<Diagnostic> diags;
                const auto decoded = decode_lzw(encoded.lzw, encoded.min_code_size, w, h,
                                                interlaced, diags);
                const std::string label = kinds[kind] + "/" + std::to_string(bits) + "bit" +
                                          (interlaced ? "/interlaced" : "");
                check(decoded.complete, "decode completes: " + label);
                check(decoded.pixels == frame.pixels, "round trip pixels: " + label);
                check(diags.empty(), "no diagnostics: " + label);
            }
        }
    }
}

void test_search_matches_and_never_loses() {
    std::mt19937 rng(99);
    for (int trial = 0; trial < 6; ++trial) {
        const uint16_t w = 200, h = 120;
        auto frame = make_frame(w, h, [&](uint16_t x, uint16_t y) -> uint8_t {
            if (trial % 2 == 0) return static_cast<uint8_t>(((x / (trial + 1)) ^ y) % 16);
            return static_cast<uint8_t>(rng() % 16);
        });
        const auto greedy = encode_lzw(frame.pixels, w, h, false, 16);
        SearchOptions so;
        so.alignment = 32;
        const auto found = encode_lzw_search(frame.pixels, w, h, false, 16, {}, so);
        check(found.searched, "search ran");
        std::vector<Diagnostic> diags;
        const auto decoded =
            decode_lzw(found.encoded.lzw, found.encoded.min_code_size, w, h, false, diags);
        check(decoded.pixels == frame.pixels, "search output decodes to the same pixels");
        check(decoded.complete && diags.empty(), "search output is a clean stream");
        check(found.encoded.lzw.size() <= greedy.lzw.size() + 8,
              "search is not meaningfully worse than greedy");

        // the same answer regardless of how the work was split
        SearchOptions parallel = so;
        parallel.threads = 4;
        const auto threaded = encode_lzw_search(frame.pixels, w, h, false, 16, {}, parallel);
        check(threaded.encoded.lzw == found.encoded.lzw, "threads do not change the output");
    }
}

// ----- container -----

void test_container_round_trip() {
    auto a = make_frame(40, 30, [](uint16_t x, uint16_t y) { return static_cast<uint8_t>((x + y) % 16); });
    a.delay = 7;
    a.has_gce = true;
    a.disposal = Disposal::Background;
    auto b = make_frame(40, 30, [](uint16_t x, uint16_t y) { return static_cast<uint8_t>((x * y) % 16); });
    b.transparent = 3;
    b.has_gce = true;
    b.local = gray_map(4);

    Stream s = make_stream({a, b});
    s.loopcount = 5;
    Extension comment;
    comment.label = 0xFE;
    comment.blocks.push_back({'h', 'e', 'l', 'l', 'o'});
    s.frames[0].extensions.push_back(comment);

    for (auto& f : s.frames)
        encode_frame(f, effective_palette_size(f, s));

    const auto bytes = write_gif(s);
    auto read = read_gif(bytes);
    check(read.ok, "written stream reads back");
    check(read.diagnostics.empty(), "no diagnostics on our own output");
    check(read.stream.frames.size() == 2, "frame count survives");
    check(read.stream.screen_width == 40 && read.stream.screen_height == 30, "screen survives");
    if (read.stream.frames.size() == 2) {
        check(read.stream.frames[0].delay == 7, "delay survives");
        check(read.stream.frames[0].disposal == Disposal::Background, "disposal survives");
        check(read.stream.frames[1].transparent == 3, "transparent index survives");
        check(read.stream.frames[1].local && read.stream.frames[1].local->size() == 4,
              "local colormap survives");
        check(read.stream.frames[0].pixels == s.frames[0].pixels, "frame 0 pixels survive");
        check(read.stream.frames[1].pixels == s.frames[1].pixels, "frame 1 pixels survive");
        check(!read.stream.frames[0].extensions.empty() &&
                  read.stream.frames[0].extensions.front().label == 0xFE,
              "comment extension survives");
    }

    // writing what we just read must reproduce the bytes exactly
    const auto again = write_gif(read.stream);
    check(again == bytes, "read then write is a no-op");
}

void test_reader_survives_damage() {
    auto f = make_frame(32, 32, [](uint16_t x, uint16_t y) { return static_cast<uint8_t>((x ^ y) % 16); });
    Stream s = make_stream({f});
    encode_frame(s.frames[0], 16);
    const auto bytes = write_gif(s);

    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<long>(cut));
        auto read = read_gif(truncated);  // must not throw, crash or hang
        check(read.stream.frames.size() <= 1, "truncated input yields at most one frame");
    }

    std::mt19937 rng(7);
    for (int i = 0; i < 300; ++i) {
        std::vector<uint8_t> damaged = bytes;
        for (int k = 0; k < 6; ++k) damaged[rng() % damaged.size()] = static_cast<uint8_t>(rng());
        auto read = read_gif(damaged);
        for (const auto& frame : read.stream.frames)
            check(frame.pixels.size() == frame.pixel_count() || frame.pixels.empty(),
                  "damaged frame keeps a consistent pixel buffer");
    }
}

// ----- optimizer -----

std::vector<std::vector<Color>> render(const Stream& s) {
    const std::size_t area = static_cast<std::size_t>(s.screen_width) * s.screen_height;
    std::vector<Color> canvas(area, Color{});
    std::vector<uint8_t> painted(area, 0);
    std::vector<std::vector<Color>> out;
    for (const auto& f : s.frames) {
        std::vector<Color> before = canvas;
        std::vector<uint8_t> before_painted = painted;
        const Colormap* map = f.local ? &*f.local : (s.global ? &*s.global : nullptr);
        for (uint16_t y = 0; y < f.height; ++y) {
            for (uint16_t x = 0; x < f.width; ++x) {
                const uint8_t index = f.pixels[static_cast<std::size_t>(y) * f.width + x];
                if (f.transparent >= 0 && index == f.transparent) continue;
                const std::size_t at = static_cast<std::size_t>(f.top + y) * s.screen_width + f.left + x;
                canvas[at] = map && index < map->colors.size() ? map->colors[index] : Color{};
                painted[at] = 1;
            }
        }
        std::vector<Color> shown = canvas;
        for (std::size_t i = 0; i < area; ++i)
            if (!painted[i]) shown[i] = Color{0, 0, 0};
        out.push_back(shown);
        if (f.disposal == Disposal::Background) {
            for (uint16_t y = 0; y < f.height; ++y)
                for (uint16_t x = 0; x < f.width; ++x) {
                    const std::size_t at =
                        static_cast<std::size_t>(f.top + y) * s.screen_width + f.left + x;
                    canvas[at] = Color{};
                    painted[at] = 0;
                }
        } else if (f.disposal == Disposal::Previous) {
            canvas = before;
            painted = before_painted;
        }
    }
    return out;
}

void test_optimizer_preserves_the_animation() {
    // a square sliding across a still background: cropping, transparency and frame
    // dropping all have something to do
    std::vector<Frame> frames;
    for (int i = 0; i < 6; ++i) {
        auto f = make_frame(64, 64, [&](uint16_t x, uint16_t y) -> uint8_t {
            const int step = i < 4 ? i : 3;  // frames 4 and 5 repeat frame 3
            const bool inside = x >= step * 8 && x < step * 8 + 12 && y >= 20 && y < 32;
            return inside ? 9 : static_cast<uint8_t>((x / 16 + y / 16) % 4);
        });
        f.delay = 10;
        f.has_gce = true;
        frames.push_back(std::move(f));
    }
    Stream original = make_stream(frames);
    Stream optimized = original;
    const auto stats = optimize(optimized);
    check(stats.skipped.empty(), "optimizer ran: " + stats.skipped);
    check(stats.frames_dropped == 2, "the two repeated frames are dropped");
    check(optimized.frames.size() == 4, "four frames remain");

    unsigned total_before = 0, total_after = 0;
    for (const auto& f : original.frames) total_before += f.delay;
    for (const auto& f : optimized.frames) total_after += f.delay;
    check(total_before == total_after, "total duration is preserved");

    const auto before = render(original);
    const auto after = render(optimized);
    check(before.size() == 6 && after.size() == 4, "rendered frame counts");
    // every frame of the original must appear, in order, in the optimized rendering
    std::size_t at = 0;
    bool matched = true;
    for (const auto& want : before) {
        if (at < after.size() && after[at] == want) continue;
        if (at + 1 < after.size() && after[at + 1] == want) {
            ++at;
            continue;
        }
        matched = false;
        break;
    }
    check(matched, "the optimized animation shows the same pictures in the same order");

    for (auto& f : optimized.frames)
        check(f.pixels.size() == f.pixel_count(), "optimized frame has its pixels");
}

void test_unoptimize_expands_frames() {
    std::vector<Frame> frames;
    auto base = make_frame(48, 48, [](uint16_t x, uint16_t y) { return static_cast<uint8_t>((x / 8 + y / 8) % 4); });
    base.delay = 5;
    base.has_gce = true;
    frames.push_back(base);
    auto patch = make_frame(8, 8, [](uint16_t, uint16_t) { return static_cast<uint8_t>(11); });
    patch.left = 16;
    patch.top = 16;
    patch.delay = 5;
    patch.has_gce = true;
    frames.push_back(patch);

    Stream s = make_stream(frames);
    const auto before = render(s);
    const auto stats = unoptimize(s);
    check(stats.skipped.empty(), "unoptimize ran");
    for (const auto& f : s.frames)
        check(f.width == 48 && f.height == 48 && f.left == 0 && f.top == 0,
              "every frame covers the screen after unoptimize");
    check(render(s) == before, "unoptimize does not change what is shown");
}

// the facade is what a consumer sees, so it gets its own coverage
void test_public_api() {
    std::vector<Frame> frames;
    for (int i = 0; i < 3; ++i) {
        auto f = make_frame(48, 48, [&](uint16_t x, uint16_t y) -> uint8_t {
            const bool box = x >= i * 8u && x < i * 8u + 10 && y >= 10 && y < 20;
            return box ? 7 : static_cast<uint8_t>((x / 12 + y / 12) % 3);
        });
        f.delay = 8;
        f.has_gce = true;
        frames.push_back(std::move(f));
    }
    Stream s = make_stream(frames);
    for (auto& f : s.frames) encode_frame(f, effective_palette_size(f, s));
    Extension comment;
    comment.label = 0xFE;
    comment.blocks.push_back({'m', 'e', 't', 'a'});
    s.frames[0].extensions.push_back(comment);
    const auto original = write_gif(s);

    std::vector<uint8_t> out;
    Options options;
    const auto plain = recompress(original, out, options);
    check(plain.ok && !out.empty(), "recompress works with default options");
    check(plain.input_bytes == original.size(), "input size reported");
    check(plain.output_bytes == out.size(), "output size reported");
    check(plain.frames_in == 3 && plain.frames_out == 3, "frame counts reported");
    check(!plain.restructured, "nothing restructured unless asked");

    options.restructure = true;
    options.search_restarts = true;
    const auto full = recompress(original, out, options);
    check(full.ok, "restructure and search run");
    check(full.output_bytes <= plain.output_bytes, "the fuller pipeline is not larger");
    auto reread = read_gif(out);
    check(reread.ok && reread.diagnostics.empty(), "the result reads back cleanly");

    options.strip_metadata = true;
    const auto stripped = recompress(original, out, options);
    check(stripped.metadata_removed > 0, "metadata removal is reported");
    for (const auto& f : read_gif(out).stream.frames)
        for (const auto& e : f.extensions) check(e.label != 0xFE, "comments really go away");

    Options refuse;
    refuse.level = Lossless::Structure;
    refuse.restructure = true;
    const auto refused = recompress(original, out, refuse);
    check(!refused.ok && !refused.restructure_note.empty(), "l1 plus restructure is refused");

    const std::vector<uint8_t> garbage(64, 0x42);
    const auto bad = recompress(garbage, out, {});
    check(!bad.ok, "garbage is rejected, not crashed on");
    check(!version().empty(), "version is reported");
}

void test_level_l1_refuses_to_restructure() {
    auto f = make_frame(16, 16, [](uint16_t x, uint16_t) { return static_cast<uint8_t>(x % 4); });
    Stream s = make_stream({f});
    OptimizeOptions options;
    options.level = Lossless::Structure;
    const auto stats = optimize(s, options);
    check(!stats.skipped.empty(), "l1 refuses to restructure");
    check(s.frames.size() == 1 && s.frames[0].width == 16, "l1 leaves the stream alone");
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"lzw round trip", test_lzw_round_trip},
        {"search", test_search_matches_and_never_loses},
        {"container round trip", test_container_round_trip},
        {"reader survives damage", test_reader_survives_damage},
        {"optimizer preserves the animation", test_optimizer_preserves_the_animation},
        {"unoptimize", test_unoptimize_expands_frames},
        {"level l1", test_level_l1_refuses_to_restructure},
        {"public api", test_public_api},
    };
    for (const auto& [name, fn] : tests) {
        const int before = failures;
        fn();
        std::printf("%-38s %s\n", name, failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

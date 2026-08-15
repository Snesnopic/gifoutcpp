// feeds arbitrary bytes to the reader and then through the whole pipeline. what is
// being checked is that nothing crashes, reads out of bounds or hangs: a gif that
// comes off the internet is under nobody's control.

#include <cstdint>
#include <vector>

#include "optigif/optigif.hpp"

namespace {

// the search is quadratic in the frame size, so a fuzz case must stay small or the
// run degenerates into one enormous input per minute
constexpr std::size_t kMaxPixelsForSearch = 40000;

std::size_t pixel_count(const optigif::Stream& stream) {
    std::size_t total = 0;
    for (const auto& f : stream.frames) total += f.pixel_count();
    return total;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (1u << 20)) return 0;
    std::span<const uint8_t> input(data, size);

    auto read = optigif::read_gif(input);
    if (!read.ok) return 0;

    const std::size_t pixels = pixel_count(read.stream);
    if (pixels > kMaxPixelsForSearch) return 0;

    for (const bool restructure : {false, true}) {
        optigif::Options options;
        options.restructure = restructure;
        options.search_restarts = restructure;
        options.strip_metadata = restructure;
        options.search.alignment = 64;
        options.search.max_tokens = 2000;

        std::vector<uint8_t> out;
        const auto result = optigif::recompress(input, out, options);
        if (!result.ok) continue;

        // whatever we produce has to be readable by us, with no repairs needed
        auto again = optigif::read_gif(out);
        if (!again.ok) __builtin_trap();
        for (const auto& d : again.diagnostics)
            if (d.severity == optigif::Severity::Error) __builtin_trap();
    }
    return 0;
}

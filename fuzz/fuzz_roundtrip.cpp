// treats the input as raw pixel indices and checks the one invariant the whole
// project rests on: whatever the encoder emits, the decoder gets the pixels back.
// this is the target that would have caught the block boundary desync.

#include <cstdint>
#include <vector>

#include "gifoutcpp/gifoutcpp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4 || size > 65536) return 0;

    // first two bytes choose the geometry, the rest are pixels
    const unsigned palette_bits = 1u + (data[0] & 7u);  // 1..8
    const unsigned palette = 1u << palette_bits;
    std::size_t width = 1 + data[1];
    const std::vector<uint8_t> body(data + 2, data + size);
    if (body.empty()) return 0;
    if (width > body.size()) width = body.size();
    const std::size_t height = body.size() / width;
    if (height == 0 || height > 65535) return 0;

    std::vector<uint8_t> pixels(body.begin(), body.begin() + static_cast<long>(width * height));
    for (auto& p : pixels) p = static_cast<uint8_t>(p % palette);

    for (const bool interlaced : {false, true}) {
        std::vector<gifout::Diagnostic> diagnostics;

        const auto greedy = gifout::encode_lzw(pixels, static_cast<uint16_t>(width),
                                               static_cast<uint16_t>(height), interlaced, palette);
        auto back = gifout::decode_lzw(greedy.lzw, greedy.min_code_size,
                                       static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                       interlaced, diagnostics);
        if (back.pixels != pixels || !back.complete || !diagnostics.empty()) __builtin_trap();

        gifout::SearchOptions search;
        search.alignment = 32;
        search.max_tokens = 1500;
        const auto found = gifout::encode_lzw_search(pixels, static_cast<uint16_t>(width),
                                                     static_cast<uint16_t>(height), interlaced,
                                                     palette, {}, search);
        if (!found.searched) continue;
        diagnostics.clear();
        back = gifout::decode_lzw(found.encoded.lzw, found.encoded.min_code_size,
                                  static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                  interlaced, diagnostics);
        if (back.pixels != pixels || !back.complete || !diagnostics.empty()) __builtin_trap();

        // and the same answer however the work was divided
        search.threads = 4;
        const auto threaded = gifout::encode_lzw_search(pixels, static_cast<uint16_t>(width),
                                                        static_cast<uint16_t>(height), interlaced,
                                                        palette, {}, search);
        if (threaded.encoded.lzw != found.encoded.lzw) __builtin_trap();
    }
    return 0;
}

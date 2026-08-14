#ifndef GIFOUTCPP_GIF_ENCODER_HPP
#define GIFOUTCPP_GIF_ENCODER_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

struct EncodeOptions {
    // derive the code size from the palette instead of the highest index actually
    // used; larger output, but survives decoders that dislike a tight code size
    bool careful_min_code_size = false;
    // clear the dictionary as soon as it fills instead of asking the run heuristic
    bool eager_clear = false;
    // encode both ways and keep the smaller one
    bool try_both_clear_policies = false;
    // override the code size entirely, 0 means compute it
    uint8_t min_code_size = 0;

    // taking a shorter match than the longest one changes what enters the dictionary
    // and can pay off later, which greedy never sees. 0 or 1 means pure greedy; the
    // result is only kept when it really came out smaller, so this can never lose.
    unsigned lookahead = 0;
    unsigned lookahead_probe = 4;
};

struct EncodeResult {
    std::vector<uint8_t> lzw;
    uint8_t min_code_size = 0;
    bool cleared = false;  // true when a dictionary restart was emitted

    [[nodiscard]] std::size_t encoded_size() const { return lzw.size(); }
};

// pixels are in natural row order; interlaced tells the encoder to visit them in
// the four gif passes, exactly as the decoder reads them back
EncodeResult encode_lzw(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                        bool interlaced, unsigned palette_size, const EncodeOptions& options = {});

// re-encodes a frame in place from its pixels, dropping the source block split
void encode_frame(Frame& frame, unsigned palette_size, const EncodeOptions& options = {});

// palette a frame's indices resolve against, needed to size the code table
unsigned effective_palette_size(const Frame& frame, const Stream& stream);

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_ENCODER_HPP

#ifndef GIFOUTCPP_GIF_LZW_BEAM_HPP
#define GIFOUTCPP_GIF_LZW_BEAM_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "gifoutcpp/gif_encoder.hpp"

namespace gifout {

// searches the parse itself rather than the restart points: it keeps several ways of
// having reached each position, each carrying its own dictionary, instead of
// committing to the longest match every time.
//
// this is not exhaustive and cannot be. the state of an lzw encoder is the position
// *and the dictionary it built to get there*, and two parses that meet at the same
// position almost never carry the same dictionary, so the paths never merge and the
// state count grows exponentially. what this offers is as wide a search as the caller
// is willing to pay for, and it is monotone by construction: it runs the widths in
// turn and keeps the smallest result, so asking for more effort can never hand back a
// larger file.
struct BeamOptions {
    // widest beam to try. 0 disables the search entirely
    std::size_t width = 64;
    // how many match lengths to consider per token, counting down from the longest
    std::size_t candidates = 3;
    // refuse frames larger than this: the search holds a dictionary per live state
    std::size_t max_pixels = 1u << 16;
};

struct BeamResult {
    EncodeResult encoded;
    std::size_t width_used = 0;  // the width that produced the kept result
    bool searched = false;       // false when the frame was too large or the beam was off
};

BeamResult encode_lzw_beam(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                           bool interlaced, unsigned palette_size,
                           const EncodeOptions& encode_options = {},
                           const BeamOptions& beam_options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_LZW_BEAM_HPP

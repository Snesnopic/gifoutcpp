#ifndef GIFOUTCPP_GIF_LZW_SEARCH_HPP
#define GIFOUTCPP_GIF_LZW_SEARCH_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "gif_encoder.hpp"
#include "gif_types.hpp"

namespace gifout {

// gif lets an encoder restart the dictionary whenever it likes, and where it does
// changes the size. the greedy encoder guesses with a run length heuristic; this
// one searches the restart points with a shortest path over the frame instead.
struct SearchOptions {
    // restart points are considered every this many pixels; the cost is linear in
    // 1/alignment and 10 costs 15 to 33 times more than 160 for 0.15 percent
    unsigned alignment = 160;
    // how far a block is explored before giving up on it
    unsigned max_tokens = 10000;
    // 1 keeps everything on the calling thread, 0 asks the machine how many it has;
    // the result is identical either way, only the wall clock changes
    unsigned threads = 1;
};

struct SearchResult {
    EncodeResult encoded;
    std::size_t blocks = 0;         // number of dictionary restarts plus one
    unsigned long long bits = 0;    // what the search predicted, for cross-checking
    bool searched = false;          // false when the frame was too small to bother
};

SearchResult encode_lzw_search(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                               bool interlaced, unsigned palette_size,
                               const EncodeOptions& encode_options = {},
                               const SearchOptions& search_options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_LZW_SEARCH_HPP

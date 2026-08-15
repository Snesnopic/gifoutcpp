/**
 * @file gif_lzw_search.hpp
 * @brief Searching where to restart the LZW dictionary, as a shortest path.
 *
 * GIF lets an encoder clear its dictionary whenever it likes, and where it does changes
 * the size. The greedy encoder guesses with a run length heuristic; this searches the
 * restart points instead, exactly, over positions spaced SearchOptions::alignment apart.
 */
#ifndef OPTIGIF_GIF_LZW_SEARCH_HPP
#define OPTIGIF_GIF_LZW_SEARCH_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "optigif/gif_encoder.hpp"

namespace optigif {

/** @brief How finely and how far to search, and how many threads to use. */
struct SearchOptions {
    /// Restart points are considered every this many pixels. The cost is linear in its
    /// inverse: 10 costs 15 to 33 times more than 160 and buys about 0.15 percent.
    unsigned alignment = 160u;

    /// How far a block is explored before the search gives up on it.
    unsigned max_tokens = 10000u;

    /// 1 keeps everything on the calling thread, 0 asks the machine how many it has.
    /// The result is identical either way; only the wall clock changes.
    unsigned threads = 1u;
};

/** @brief The encoded stream the search settled on, with what it did to get there. */
struct SearchResult {
    EncodeResult encoded;         ///< The stream, ready to store in a frame.
    std::size_t blocks = 0;       ///< Dictionary restarts plus one.
    unsigned long long bits = 0u;  ///< What the search predicted, for cross-checking.
    bool searched = false;        ///< False when the frame was too small to bother with.
};

/**
 * @brief Encodes a frame, searching for the best dictionary restart points.
 * @param pixels          width*height indices in natural row order.
 * @param width           Frame width in pixels.
 * @param height          Frame height in pixels.
 * @param interlaced      Emit the rows in the four GIF passes.
 * @param palette_size    Entries in the table these indices refer to.
 * @param encode_options  Code size policy, shared with the greedy encoder.
 * @param search_options  Granularity, exploration limit and thread count.
 * @return The encoded stream; SearchResult::searched is false when nothing was tried.
 */
SearchResult encode_lzw_search(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                               bool interlaced, unsigned palette_size,
                               const EncodeOptions& encode_options = {},
                               const SearchOptions& search_options = {});

}  // namespace optigif

#endif  // OPTIGIF_GIF_LZW_SEARCH_HPP

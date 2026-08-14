/**
 * @file gif_lzw_beam.hpp
 * @brief Searching the LZW parse itself, as widely as the caller will pay for.
 */
#ifndef GIFOUTCPP_GIF_LZW_BEAM_HPP
#define GIFOUTCPP_GIF_LZW_BEAM_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "gifoutcpp/gif_encoder.hpp"

namespace gifout {

/**
 * @brief How wide to search the parse, and when to refuse.
 *
 * This search is not exhaustive and cannot be. The state of an LZW encoder is the
 * position *and the dictionary it built to get there*, and two parses meeting at the
 * same position almost never carry the same dictionary, so the paths never merge and
 * the state count grows exponentially. What it offers instead is as wide a search as
 * the caller will pay for, monotone by construction: every width up to @ref width is
 * run and the smallest result kept, so more effort can never return a larger file.
 */
struct BeamOptions {
    std::size_t width = 64;       ///< Widest beam to try; 0 disables the search entirely.
    std::size_t candidates = 3;   ///< Match lengths considered per token, counting down from the longest.
    std::size_t max_pixels = 1u << 16;  ///< Refuse larger frames: every live state holds a dictionary.
};

/** @brief The smallest stream the search found, and which width found it. */
struct BeamResult {
    EncodeResult encoded;        ///< The stream; never larger than the greedy encoder's.
    std::size_t width_used = 0;  ///< The width that produced the kept result.
    bool searched = false;       ///< False when the frame was too large or the beam was off.
};

/**
 * @brief Encodes a frame by searching the parse, keeping the smallest result found.
 * @param pixels         width*height indices in natural row order.
 * @param width          Frame width in pixels.
 * @param height         Frame height in pixels.
 * @param interlaced     Emit the rows in the four GIF passes.
 * @param palette_size   Entries in the table these indices refer to.
 * @param encode_options Code size policy, shared with the greedy encoder.
 * @param beam_options   How wide to search, and the size beyond which to refuse.
 * @return The best stream found, with the greedy encoder as a floor.
 */
BeamResult encode_lzw_beam(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                           bool interlaced, unsigned palette_size,
                           const EncodeOptions& encode_options = {},
                           const BeamOptions& beam_options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_LZW_BEAM_HPP

/**
 * @file gif_encoder.hpp
 * @brief The greedy LZW encoder, and the cost function the optimizer decides with.
 *
 * This is the fast path and the yardstick: it reproduces gifsicle's output byte for
 * byte on the whole benchmark corpus, so anything that claims to beat it is measured
 * against something known rather than something plausible.
 */
#ifndef GIFOUTCPP_GIF_ENCODER_HPP
#define GIFOUTCPP_GIF_ENCODER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

/** @brief How to encode: code size policy, dictionary restart policy, effort. */
struct EncodeOptions {
    /// Take the code size from the palette rather than from the highest index actually
    /// used. Larger output, but survives decoders that dislike a tight code size.
    bool careful_min_code_size = false;

    /// Clear the dictionary as soon as it fills, instead of asking the run heuristic.
    bool eager_clear = false;

    /// Encode both ways and keep the smaller one.
    bool try_both_clear_policies = false;

    /// Override the code size entirely; 0 means compute it.
    uint8_t min_code_size = 0;

    /// Taking a shorter match than the longest one changes what enters the dictionary
    /// and can pay off later, which greedy never sees. 0 or 1 means pure greedy; the
    /// result is only kept when it really came out smaller, so this can never lose.
    unsigned lookahead = 0;

    /// How many tokens the lookahead simulates before scoring a choice.
    unsigned lookahead_probe = 4;
};

/** @brief An encoded LZW stream. */
struct EncodeResult {
    std::vector<uint8_t> lzw;   ///< The codes, without sub-block framing.
    uint8_t min_code_size = 0;  ///< Initial code width the decoder needs.
    bool cleared = false;       ///< A dictionary restart was emitted.

    /** @return Size in bytes of the encoded stream. */
    [[nodiscard]] std::size_t encoded_size() const { return lzw.size(); }
};

/**
 * @brief Encodes pixels into an LZW stream.
 * @param pixels       width*height indices in natural row order.
 * @param width        Frame width in pixels.
 * @param height       Frame height in pixels.
 * @param interlaced   Emit the rows in the four GIF passes.
 * @param palette_size Entries in the table these indices refer to.
 * @param options      Code size and restart policy.
 * @return The encoded stream, empty when the frame has no pixels.
 */
EncodeResult encode_lzw(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                        bool interlaced, unsigned palette_size, const EncodeOptions& options = {});

/**
 * @brief Re-encodes a frame in place from its pixels, dropping the source block split.
 * @param frame        Frame to rewrite; its compressed data is replaced.
 * @param palette_size Entries in the table the frame's indices refer to.
 * @param options      Code size and restart policy.
 */
void encode_frame(Frame& frame, unsigned palette_size, const EncodeOptions& options = {});

/**
 * @brief The palette a frame's indices resolve against.
 * @param frame  Frame to look at.
 * @param stream Stream it belongs to, for the global table.
 * @return Entry count of the local table when there is one, otherwise the global one.
 */
unsigned effective_palette_size(const Frame& frame, const Stream& stream);

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_ENCODER_HPP

/**
 * @file gif_lzw.hpp
 * @brief The LZW decoder.
 */
#ifndef GIFOUTCPP_GIF_LZW_HPP
#define GIFOUTCPP_GIF_LZW_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

/** @brief Pixels recovered from an LZW stream, and whether they came out whole. */
struct DecodeResult {
    std::vector<uint8_t> pixels;  ///< width*height indices in natural row order.
    bool complete = false;        ///< Exactly width*height pixels, with no repair needed.
};

/**
 * @brief Decodes an LZW stream into palette indices.
 *
 * Repairs instead of throwing: a truncated or corrupt stream yields the pixels decoded
 * so far, zero filled to the declared size, plus a diagnostic. De-interlaces on the
 * way, so callers never deal with the four GIF passes.
 *
 * @param lzw           Concatenated sub-block payloads, without their length bytes.
 * @param min_code_size Initial code width; values outside 2 to 8 are clamped.
 * @param width         Frame width in pixels.
 * @param height        Frame height in pixels.
 * @param interlaced    The rows arrive in the four GIF passes.
 * @param diagnostics   Anything repaired is appended here.
 * @param offset_hint   Byte offset reported in those diagnostics.
 * @return The pixels, always of the declared size.
 */
DecodeResult decode_lzw(std::span<const uint8_t> lzw, uint8_t min_code_size, uint16_t width,
                        uint16_t height, bool interlaced, std::vector<Diagnostic>& diagnostics,
                        std::size_t offset_hint = 0);

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_LZW_HPP

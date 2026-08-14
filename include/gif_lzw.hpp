#ifndef GIFOUTCPP_GIF_LZW_HPP
#define GIFOUTCPP_GIF_LZW_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "gif_types.hpp"

namespace gifout {

// decodes to indices in natural row order, repairing instead of throwing: real files are broken
struct DecodeResult {
    std::vector<uint8_t> pixels;
    bool complete = false;  // produced exactly width*height pixels with no repair
};

DecodeResult decode_lzw(std::span<const uint8_t> lzw, uint8_t min_code_size,
                        uint16_t width, uint16_t height, bool interlaced,
                        std::vector<Diagnostic>& diagnostics, std::size_t offset_hint = 0);

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_LZW_HPP

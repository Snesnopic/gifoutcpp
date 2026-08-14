#ifndef GIFOUTCPP_GIF_WRITER_HPP
#define GIFOUTCPP_GIF_WRITER_HPP

#include <cstdint>
#include <filesystem>
#include <vector>

#include "gif_types.hpp"

namespace gifout {

struct WriteOptions {
    // re-chunk at 255 bytes instead of keeping the source split, which breaks byte exactness
    bool reblock_lzw = false;
};

std::vector<uint8_t> write_gif(const Stream& stream, const WriteOptions& options = {});
bool write_gif_file(const Stream& stream, const std::filesystem::path& path,
                    const WriteOptions& options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_WRITER_HPP

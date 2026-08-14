/**
 * @file gif_writer.hpp
 * @brief Serialising the model back to GIF bytes.
 */
#ifndef GIFOUTCPP_GIF_WRITER_HPP
#define GIFOUTCPP_GIF_WRITER_HPP

#include <cstdint>
#include <filesystem>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

/** @brief Knobs for @ref write_gif and @ref write_gif_file. */
struct WriteOptions {
    /// Re-chunk LZW data at 255 bytes instead of keeping the split the source used.
    /// Keeping it is what makes an untouched read/write round trip byte exact.
    bool reblock_lzw = false;
};

/**
 * @brief Serialises a stream into a buffer.
 * @param stream  The model to write.
 * @param options Whether to preserve the source sub-block split.
 * @return The complete file, header to trailer.
 */
std::vector<uint8_t> write_gif(const Stream& stream, const WriteOptions& options = {});

/**
 * @brief Serialises a stream to disk.
 * @param stream  The model to write.
 * @param path    Destination file, truncated if it exists.
 * @param options Whether to preserve the source sub-block split.
 * @return True when every byte reached the file.
 */
bool write_gif_file(const Stream& stream, const std::filesystem::path& path,
                    const WriteOptions& options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_WRITER_HPP

#ifndef GIFOUTCPP_GIF_READER_HPP
#define GIFOUTCPP_GIF_READER_HPP

#include <filesystem>
#include <span>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

struct ReadResult {
    Stream stream;
    std::vector<Diagnostic> diagnostics;
    bool ok = false;  // false only when not even a header could be read

    [[nodiscard]] bool has_errors() const;
};

struct ReadOptions {
    // off keeps only the compressed bytes, which is all a verbatim rewrite needs
    bool decode_pixels = true;
    // cap on diagnostics so a corrupt file cannot flood the caller
    std::size_t max_diagnostics = 64;
    // a frame declares its size in four bytes, so without a budget a 35 byte file can
    // ask for gigabytes. frames past this are kept compressed and left undecoded.
    std::size_t max_frame_pixels = std::size_t{1} << 28;
};

ReadResult read_gif(std::span<const uint8_t> data, const ReadOptions& options = {});
ReadResult read_gif_file(const std::filesystem::path& path, const ReadOptions& options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_READER_HPP

#ifndef GIFOUTCPP_GIFOUTCPP_HPP
#define GIFOUTCPP_GIFOUTCPP_HPP

// the whole library in one header: everything below is the supported surface, and
// the individual headers it pulls in are supported too. anything under src/ is not.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "gifoutcpp/gif_encoder.hpp"
#include "gifoutcpp/gif_lzw.hpp"
#include "gifoutcpp/gif_lzw_search.hpp"
#include "gifoutcpp/gif_optimizer.hpp"
#include "gifoutcpp/gif_reader.hpp"
#include "gifoutcpp/gif_types.hpp"
#include "gifoutcpp/gif_writer.hpp"

#define GIFOUTCPP_VERSION_MAJOR 0
#define GIFOUTCPP_VERSION_MINOR 2
#define GIFOUTCPP_VERSION_PATCH 0

namespace gifout {

// what a consumer asks for, in the order the pipeline applies it
struct Options {
    // l1 keeps the structure and only re-encodes; l2 may rebuild anything as long as
    // the animation plays the same. asking for l1 with restructure on is an error.
    Lossless level = Lossless::Rendering;

    bool strip_metadata = false;  // drop comments and application blocks, keep the loop
    bool unoptimize = false;      // expand every frame back to full screen first
    bool restructure = false;     // crop frames, pick disposal and transparency, rebuild palettes
    bool deinterlace = false;     // implies restructure
    bool search_restarts = false; // search the lzw restart points instead of guessing

    bool copy_lzw = false;   // do not re-encode at all, only rewrite the container
    bool reblock_lzw = false;  // re-chunk copied lzw at 255 bytes

    EncodeOptions encode;
    SearchOptions search;  // alignment, max_tokens and threads live here
};

struct Result {
    bool ok = false;
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
    std::size_t frames_in = 0;
    std::size_t frames_out = 0;
    std::size_t metadata_removed = 0;
    bool restructured = false;  // false when restructuring was asked for but not used
    // why restructuring did not happen or did not pay, empty when it did
    std::string restructure_note;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool smaller() const { return ok && output_bytes < input_bytes; }
};

// recompresses in memory. output is left untouched when the input cannot be read.
Result recompress(std::span<const uint8_t> input, std::vector<uint8_t>& output,
                  const Options& options = {});

// same, reading and writing files. the output file is only written when ok.
Result recompress_file(const std::filesystem::path& input, const std::filesystem::path& output,
                       const Options& options = {});

// applies the pipeline to a stream that is already in memory, leaving i/o to the caller
Result recompress_stream(Stream& stream, std::vector<uint8_t>& output,
                         const Options& options = {});

[[nodiscard]] std::string version();

}  // namespace gifout

#endif  // GIFOUTCPP_GIFOUTCPP_HPP

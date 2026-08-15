/**
 * @file optigif.hpp
 * @brief The whole library in one header, and the pipeline that runs its stages.
 *
 * Everything reachable from here is the supported surface, including the headers this
 * one pulls in. Anything under src/ is not.
 */
#ifndef OPTIGIF_OPTIGIF_HPP
#define OPTIGIF_OPTIGIF_HPP

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "optigif/gif_encoder.hpp"
#include "optigif/gif_lzw.hpp"
#include "optigif/gif_lzw_beam.hpp"
#include "optigif/gif_lzw_search.hpp"
#include "optigif/gif_optimizer.hpp"
#include "optigif/gif_reader.hpp"
#include "optigif/gif_types.hpp"
#include "optigif/gif_writer.hpp"

namespace optigif {

/** @brief What to do to the file, in the order the pipeline applies it. */
struct Options {
    /// L1 keeps the structure and only re-encodes; L2 may rebuild anything as long as
    /// the animation plays the same. Asking for L1 with @ref restructure on is refused.
    Lossless level = Lossless::Rendering;

    bool strip_metadata = false;   ///< Drop comments and application blocks, keep the loop block.
    bool unoptimize = false;       ///< Expand every frame back to full screen first.
    bool restructure = false;      ///< Crop frames, pick disposal and transparency, rebuild palettes.
    bool search_restarts = false;  ///< Search the LZW restart points instead of guessing them.

    /// Interlacing costs size and only buys progressive display over a slow link, so a
    /// re-encoded frame drops it. That is a rendering level change, never applied at L1.
    bool keep_interlace = false;

    /// Try the settings that win on some files and lose on others, keeping whichever
    /// came out smallest. Costs a full pass per candidate.
    bool try_everything = false;

    bool copy_lzw = false;     ///< Do not re-encode at all, only rewrite the container.
    bool reblock_lzw = false;  ///< Re-chunk copied LZW data at 255 bytes.

    EncodeOptions encode;  ///< Code size and restart policy for the encoder.
    SearchOptions search;  ///< Alignment, exploration limit and thread count.

    /// Search the parse itself, as widely as the caller will pay for. Off by default
    /// because it is orders of magnitude slower than everything else here.
    BeamOptions beam;
    bool search_parse = false;  ///< Enable the parse search described by @ref beam.
};

/** @brief What the pipeline produced, and what it decided along the way. */
struct Result {
    bool ok = false;                  ///< False when the input could not be read at all.
    std::size_t input_bytes = 0;      ///< Size of the input.
    std::size_t output_bytes = 0;     ///< Size of what was produced.
    std::size_t frames_in = 0;        ///< Frames on the way in.
    std::size_t frames_out = 0;       ///< Frames on the way out; fewer when some were merged.
    std::size_t metadata_removed = 0; ///< Bytes of comments and application blocks dropped.
    bool restructured = false;        ///< False when restructuring was asked for but not used.
    std::string variant;              ///< Which candidate won, when Options::try_everything was on.
    std::string restructure_note;     ///< Why restructuring did not happen or did not pay.
    std::vector<Diagnostic> diagnostics;  ///< Everything the reader repaired or noticed.

    /** @return True when the output is worth keeping over the input. */
    [[nodiscard]] bool smaller() const { return ok && output_bytes < input_bytes; }
};

/**
 * @brief Recompresses a GIF held in memory.
 * @param input   The whole file.
 * @param output  Receives the result; left untouched when the input cannot be read.
 * @param options What to do to the file.
 * @return What was produced and decided; check Result::ok.
 */
Result recompress(std::span<const uint8_t> input, std::vector<uint8_t>& output,
                  const Options& options = {});

/**
 * @brief Recompresses a GIF from disk to disk.
 * @param input   File to read.
 * @param output  File to write; only written when the result is ok.
 * @param options What to do to the file.
 * @return What was produced and decided; check Result::ok.
 */
Result recompress_file(const std::filesystem::path& input, const std::filesystem::path& output,
                       const Options& options = {});

/**
 * @brief Runs the pipeline over a stream the caller already has, leaving I/O to them.
 *
 * Useful for inspecting or editing frames between the stages.
 *
 * @param stream  The model, rewritten in place.
 * @param output  Receives the serialised result.
 * @param options What to do to the stream.
 * @return What was produced and decided.
 */
Result recompress_stream(Stream& stream, std::vector<uint8_t>& output,
                         const Options& options = {});

/**
 * @return The library version, as "major.minor.patch", from the build that produced it.
 */
[[nodiscard]] std::string_view version();

}  // namespace optigif

#endif  // OPTIGIF_OPTIGIF_HPP

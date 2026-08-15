/**
 * @file gif_reader.hpp
 * @brief Parsing a GIF into the model, repairing rather than refusing.
 *
 * Real GIFs are broken in ways a strict parser rejects: trailing junk, truncated
 * streams, out of range code sizes, palette size bits set with no palette present. The
 * reader repairs what it can and reports through a list of diagnostics, because a file
 * a viewer displays has to be readable here too.
 */
#ifndef OPTIGIF_GIF_READER_HPP
#define OPTIGIF_GIF_READER_HPP

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "optigif/gif_types.hpp"

namespace optigif {

/** @brief What came out of a parse: the stream, and everything odd that was noticed. */
struct ReadResult {
    Stream stream;                        ///< The parsed file; empty when @ref ok is false.
    std::vector<Diagnostic> diagnostics;  ///< Anything repaired, dropped or worth saying.
    bool ok = false;                      ///< False only when not even one frame could be read.

    /** @return True when at least one diagnostic has Severity::Error. */
    [[nodiscard]] bool has_errors() const;
};

/** @brief Knobs for @ref read_gif and @ref read_gif_file. */
struct ReadOptions {
    /// Decode every frame's LZW into indices. Off keeps only the compressed bytes,
    /// which is all a verbatim rewrite needs.
    bool decode_pixels = true;

    /// Cap on diagnostics, so a corrupt file cannot flood the caller.
    std::size_t max_diagnostics = 64;

    /// A frame declares its size in four bytes, so without a budget a 35 byte file can
    /// ask for gigabytes. Frames past this are kept compressed and left undecoded.
    std::size_t max_frame_pixels = std::size_t{1} << 28;
};

/**
 * @brief Parses a GIF held in memory.
 * @param data    The whole file.
 * @param options What to decode and how much to tolerate.
 * @return The stream and its diagnostics; check ReadResult::ok before using it.
 */
ReadResult read_gif(std::span<const uint8_t> data, const ReadOptions& options = {});

/**
 * @brief Parses a GIF from disk.
 * @param path    File to read.
 * @param options What to decode and how much to tolerate.
 * @return The stream and its diagnostics; an unreadable path yields ok == false.
 */
ReadResult read_gif_file(const std::filesystem::path& path, const ReadOptions& options = {});

}  // namespace optigif

#endif  // OPTIGIF_GIF_READER_HPP

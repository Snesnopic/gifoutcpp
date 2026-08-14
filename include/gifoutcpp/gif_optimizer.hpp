/**
 * @file gif_optimizer.hpp
 * @brief Rebuilding the structure of an animation: frames, disposal, palettes.
 *
 * This is where the largest gain lives. The optimizer simulates the canvas the original
 * animation paints, then rebuilds each frame around the smallest rectangle that
 * changes, choosing a disposal, a transparency scheme and a palette. Every candidate is
 * measured with the encoder rather than picked by a rule of thumb.
 */
#ifndef GIFOUTCPP_GIF_OPTIMIZER_HPP
#define GIFOUTCPP_GIF_OPTIMIZER_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "gifoutcpp/gif_types.hpp"

namespace gifout {

/**
 * @brief What a transformation is allowed to change.
 *
 * The two levels are nested: Rendering contains Structure. A consumer that verifies by
 * comparing frame structure needs Structure; one that only cares what the viewer sees
 * can allow Rendering and get considerably more.
 */
enum class Lossless : uint8_t {
    Structure,  ///< L1: frame count, geometry, indices and palette all survive.
    Rendering,  ///< L2: only the animation as played survives.
};

/** @brief Which structural transformations to apply. */
struct OptimizeOptions {
    Lossless level = Lossless::Rendering;  ///< Restructuring needs Rendering; Structure refuses.
    bool crop_frames = true;               ///< Shrink each frame to the area that changes.
    bool use_transparency = true;          ///< Let unchanged pixels show the canvas through.
    bool drop_redundant_frames = true;     ///< Drop frames that paint nothing, merging their delay.
    bool prune_palettes = true;            ///< Rebuild palettes around the colours really used.
    bool deinterlace = false;              ///< Store rows in natural order rather than interlaced.
};

/** @brief What the optimizer did, or why it did nothing. */
struct OptimizeStats {
    std::size_t frames_before = 0;   ///< Frames on the way in.
    std::size_t frames_dropped = 0;  ///< Frames merged away because they painted nothing.
    std::size_t pixels_before = 0;   ///< Total frame area before cropping.
    std::size_t pixels_after = 0;    ///< Total frame area after cropping.
    std::size_t colors_before = 0;   ///< Distinct colours across the whole animation.
    std::size_t colors_after = 0;    ///< Distinct colours actually kept.
    bool transparency_used = false;  ///< At least one frame shows the canvas through.
    bool local_colormaps = false;    ///< The colours did not fit one shared table.
    std::string skipped;             ///< Non-empty when the stream was left untouched, with the reason.
};

/**
 * @brief Rebuilds the structure of a stream in place.
 *
 * Frames keep their pixels but their LZW data is dropped, so the caller has to
 * re-encode afterwards. The stream is left untouched when the transformation cannot be
 * applied, with the reason in OptimizeStats::skipped.
 *
 * @param stream  The model to rewrite.
 * @param options Which transformations to apply, and at which lossless level.
 * @return What was done, or why nothing was.
 */
OptimizeStats optimize(Stream& stream, const OptimizeOptions& options = {});

/**
 * @brief Expands every frame back to the full screen with disposal applied.
 *
 * The inverse of the cropping optimize() does, and the way to re-optimize a stream
 * somebody else optimized badly.
 *
 * @param stream The model to rewrite.
 * @return What was done, or why nothing was.
 */
OptimizeStats unoptimize(Stream& stream);

/**
 * @brief Drops comments, plain text blocks and application extensions.
 *
 * The loop block is kept, because it is animation rather than metadata. This is a
 * policy and not an optimization: the pixels do not change either way, which is why the
 * tool never decides it on its own.
 *
 * @param stream The model to strip.
 * @return How many bytes the removed blocks occupied.
 */
std::size_t strip_metadata(Stream& stream);

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_OPTIMIZER_HPP

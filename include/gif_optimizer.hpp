#ifndef GIFOUTCPP_GIF_OPTIMIZER_HPP
#define GIFOUTCPP_GIF_OPTIMIZER_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "gif_types.hpp"

namespace gifout {

// what a transformation is allowed to change; see README for the two levels
enum class Lossless : uint8_t {
    Structure,  // l1: frame count, geometry, indices and palette all survive
    Rendering,  // l2: only the animation as played survives
};

struct OptimizeOptions {
    Lossless level = Lossless::Rendering;
    // shrink each frame to the area that actually changes, picking disposal to match
    bool crop_frames = true;
    // let unchanged pixels show the canvas through instead of repainting them
    bool use_transparency = true;
    // drop frames that paint nothing new, adding their delay to the previous one
    bool drop_redundant_frames = true;
    // rebuild palettes around the colors that are really used
    bool prune_palettes = true;
    // interlacing costs size and only buys progressive loading over a slow link
    bool deinterlace = false;
};

struct OptimizeStats {
    std::size_t frames_before = 0;
    std::size_t frames_dropped = 0;
    std::size_t pixels_before = 0;
    std::size_t pixels_after = 0;
    std::size_t colors_before = 0;
    std::size_t colors_after = 0;
    bool transparency_used = false;
    bool local_colormaps = false;
    std::string skipped;  // non-empty when the stream was left untouched, with the reason
};

// rewrites the stream in place; frames keep their pixels, their lzw data is dropped
// and has to be re-encoded afterwards
OptimizeStats optimize(Stream& stream, const OptimizeOptions& options = {});

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_OPTIMIZER_HPP

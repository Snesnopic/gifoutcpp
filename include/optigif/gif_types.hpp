/**
 * @file gif_types.hpp
 * @brief The in-memory model of a GIF file: stream, frames, colormaps, extensions.
 *
 * Everything the reader produces and the writer consumes lives here. The model keeps
 * both representations of a frame at once, the compressed LZW bytes and the decoded
 * indices, so a stage can rewrite one without discarding the other.
 */
#ifndef OPTIGIF_GIF_TYPES_HPP
#define OPTIGIF_GIF_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace optigif {

/** @brief One entry of a colour table. */
struct Color {
    uint8_t r = 0u;  ///< Red channel.
    uint8_t g = 0u;  ///< Green channel.
    uint8_t b = 0u;  ///< Blue channel.

    /** @brief Two colours are the same when all three channels are. */
    friend bool operator==(const Color&, const Color&) = default;
};

/**
 * @brief A GIF colour table, global or local.
 *
 * The entry count is kept exactly as it was read. On disk a table always holds a power
 * of two entries, and preserving that is what lets an untouched round trip reproduce
 * the original bytes.
 */
struct Colormap {
    std::vector<Color> colors;  ///< The entries, in palette order.
    bool sorted = false;        ///< The "sorted" flag of the packed field, preserved verbatim.

    /** @return Number of entries in the table. */
    [[nodiscard]] std::size_t size() const noexcept { return colors.size(); }

    /** @return True when the table holds no entries. */
    [[nodiscard]] bool empty() const noexcept { return colors.empty(); }
};

/** @brief What a viewer does with a frame's area before drawing the next one. */
enum class Disposal : uint8_t {
    None = 0,        ///< Leave the frame on screen.
    Asis = 1,        ///< Also leave it; semantically identical to None in every viewer.
    Background = 2,  ///< Erase the frame's rectangle back to the background.
    Previous = 3,    ///< Restore what was on screen before the frame was drawn.
};

/**
 * @brief An extension block kept byte for byte.
 *
 * Sub-block boundaries are part of the file's identity, so they are preserved rather
 * than normalised. Graphic control extensions are not stored here: they are parsed
 * into the frame's own fields and regenerated on write.
 */
struct Extension {
    uint8_t label = 0;                          ///< 0x01 plain text, 0xFE comment, 0xFF application.
    std::vector<std::vector<uint8_t>> blocks;   ///< The sub-blocks, in order, without their length bytes.

    /**
     * @brief Tests whether this is an application extension with a given identifier.
     * @param name Application identifier to match, for example "NETSCAPE2.0".
     * @return True when the label is 0xFF and the first sub-block starts with @p name.
     */
    [[nodiscard]] bool is_application(std::string_view name) const;
};

/**
 * @brief One image in the stream, with its timing, its palette and its pixels.
 *
 * A frame carries its compressed bytes and, when the reader was asked to decode, the
 * indices they expand to. Pixels are always stored in natural row order; the
 * interlaced flag only says how they are laid out on disk.
 */
struct Frame {
    uint16_t left = 0;    ///< Distance from the left edge of the logical screen.
    uint16_t top = 0;     ///< Distance from the top edge of the logical screen.
    uint16_t width = 0;   ///< Frame width in pixels.
    uint16_t height = 0;  ///< Frame height in pixels.
    bool interlaced = false;           ///< The rows are stored in the four GIF passes.
    std::optional<Colormap> local;     ///< Local colour table, if the frame has one.

    bool has_gce = false;              ///< The source carried a graphic control extension.
    Disposal disposal = Disposal::None;///< What happens to this frame's area afterwards.
    bool user_input = false;           ///< The "user input" flag, preserved.
    int transparent = -1;              ///< Transparent palette index, or -1 for none.
    uint16_t delay = 0;                ///< How long the frame stays up, in hundredths of a second.

    // real files carry these bits with the flag that gives them meaning turned off, so
    // they are kept for a verbatim rewrite and ignored everywhere else
    uint8_t raw_transparent_index = 0;  ///< The transparent index byte, flag or no flag.
    uint8_t raw_local_size_bits = 0;    ///< The palette size bits, local table or not.

    std::vector<Extension> extensions;  ///< Blocks preceding this frame, in order.
    std::size_t gce_position = 0;       ///< Where the graphic control extension sits among them.

    uint8_t lzw_min_code_size = 0;      ///< Initial LZW code width of the compressed data.
    std::vector<uint8_t> lzw;           ///< Compressed sub-block payloads, concatenated.
    std::vector<uint8_t> block_sizes;   ///< Original sub-block split; empty means free to re-block.
    std::vector<uint8_t> pixels;        ///< width*height palette indices, natural row order.
    bool pixels_complete = false;       ///< False when the decoder had to repair the stream.

    /** @return Number of pixels the frame declares, which is width times height. */
    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * height;
    }
};

/** @brief A whole GIF file: the logical screen, its frames and what surrounds them. */
struct Stream {
    std::string version = "89a";     ///< "87a" or "89a", as read.
    uint16_t screen_width = 0;       ///< Logical screen width.
    uint16_t screen_height = 0;      ///< Logical screen height.
    uint8_t color_resolution = 7;    ///< Bits 4 to 6 of the packed field, preserved verbatim.
    uint8_t background = 0;          ///< Index into the global table for uncovered area.
    uint8_t pixel_aspect_ratio = 0;  ///< The aspect ratio byte, preserved verbatim.
    std::optional<Colormap> global;  ///< Global colour table, if the file has one.

    std::vector<Frame> frames;                   ///< The images, in playback order.
    std::vector<Extension> trailing_extensions;  ///< Blocks after the last frame.

    long loopcount = -1;  ///< Read from the netscape block; -1 when the file has none.
};

/** @brief How much a diagnostic matters. */
enum class Severity : uint8_t {
    Info,     ///< Something worth mentioning that changed nothing.
    Warning,  ///< Something was repaired or dropped.
    Error,    ///< Something could not be read at all.
};

/** @brief One thing the reader noticed, with where in the file it happened. */
struct Diagnostic {
    Severity severity = Severity::Info;  ///< How much it matters.
    std::size_t offset = 0;              ///< Byte offset in the source file.
    std::string message;                 ///< What happened, in plain words.
};

}  // namespace optigif

#endif  // OPTIGIF_GIF_TYPES_HPP

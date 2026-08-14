#ifndef GIFOUTCPP_GIF_TYPES_HPP
#define GIFOUTCPP_GIF_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gifout {

struct Color {
    uint8_t r = 0, g = 0, b = 0;
    friend bool operator==(const Color&, const Color&) = default;
};

// palette as read: entry count stays a power of two so a round trip is byte exact
struct Colormap {
    std::vector<Color> colors;
    bool sorted = false;

    [[nodiscard]] std::size_t size() const noexcept { return colors.size(); }
    [[nodiscard]] bool empty() const noexcept { return colors.empty(); }
};

enum class Disposal : uint8_t {
    None = 0,
    Asis = 1,
    Background = 2,
    Previous = 3,
};

// extension kept verbatim, sub-block boundaries included, they are part of the file identity
struct Extension {
    uint8_t label = 0;  // 0x01 plain text, 0xFE comment, 0xFF application
    std::vector<std::vector<uint8_t>> blocks;

    [[nodiscard]] bool is_application(std::string_view name) const;
};

struct Frame {
    uint16_t left = 0, top = 0, width = 0, height = 0;
    bool interlaced = false;
    std::optional<Colormap> local;

    // graphic control extension; absent in files that never had one
    bool has_gce = false;
    Disposal disposal = Disposal::None;
    bool user_input = false;
    int transparent = -1;  // palette index, -1 = none
    uint16_t delay = 0;    // hundredths of a second

    // bits real files carry with the flag that gives them meaning turned off; kept
    // so a verbatim rewrite reproduces the source bytes, ignored otherwise
    uint8_t raw_transparent_index = 0;
    uint8_t raw_local_size_bits = 0;

    std::vector<Extension> extensions;  // blocks preceding this frame, in order
    std::size_t gce_position = 0;       // where the gce sits among them, order is part of the bytes

    uint8_t lzw_min_code_size = 0;
    std::vector<uint8_t> lzw;          // sub-block payloads concatenated
    std::vector<uint8_t> block_sizes;  // original split, empty = free to re-block
    std::vector<uint8_t> pixels;       // width*height indices, natural row order

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * height;
    }
};

struct Stream {
    std::string version = "89a";  // "87a" or "89a"
    uint16_t screen_width = 0, screen_height = 0;
    uint8_t color_resolution = 7;  // bits 4-6 of the packed field, preserved verbatim
    uint8_t background = 0;
    uint8_t pixel_aspect_ratio = 0;
    std::optional<Colormap> global;

    std::vector<Frame> frames;
    std::vector<Extension> trailing_extensions;  // blocks after the last frame

    long loopcount = -1;  // read from the netscape block, which also stays in extensions
};

enum class Severity : uint8_t { Info, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Info;
    std::size_t offset = 0;  // byte offset in the source file
    std::string message;
};

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_TYPES_HPP

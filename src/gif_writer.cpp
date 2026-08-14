#include "gifoutcpp/gif_writer.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>

namespace gifout {
namespace {

void put_byte(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void put_word(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

// gif stores palettes at a power of two size, so a short map is padded with black
uint8_t colormap_size_bits(std::size_t entries) {
    // the packed field stores log2(slots) - 1, and a table always rounds up to a power
    // of two, so this is the width of the largest index minus one
    if (entries <= 2) return 0;
    const int bits = std::bit_width(entries - 1) - 1;
    return static_cast<uint8_t>(std::clamp(bits, 0, 7));
}

void put_colormap(std::vector<uint8_t>& out, const Colormap& map) {
    const std::size_t slots = 2u << colormap_size_bits(map.size());
    for (std::size_t i = 0; i < slots; ++i) {
        const Color c = i < map.colors.size() ? map.colors[i] : Color{};
        out.push_back(c.r);
        out.push_back(c.g);
        out.push_back(c.b);
    }
}

void put_extension(std::vector<uint8_t>& out, const Extension& ext) {
    put_byte(out, 0x21);
    put_byte(out, ext.label);
    for (const auto& block : ext.blocks) {
        if (block.empty()) continue;
        for (std::size_t i = 0; i < block.size(); i += 255) {
            const std::size_t n = std::min<std::size_t>(255, block.size() - i);
            put_byte(out, static_cast<uint8_t>(n));
            const auto at = static_cast<std::ptrdiff_t>(i);
            out.insert(out.end(), block.begin() + at, block.begin() + at + static_cast<std::ptrdiff_t>(n));
        }
    }
    put_byte(out, 0x00);
}

void put_gce(std::vector<uint8_t>& out, const Frame& frame) {
    put_byte(out, 0x21);
    put_byte(out, 0xF9);
    put_byte(out, 4);
    const uint8_t packed = static_cast<uint8_t>((static_cast<uint8_t>(frame.disposal) & 7) << 2) |
                           static_cast<uint8_t>(frame.user_input ? 0x02 : 0) |
                           static_cast<uint8_t>(frame.transparent >= 0 ? 0x01 : 0);
    put_byte(out, packed);
    put_word(out, frame.delay);
    put_byte(out, frame.transparent >= 0 ? static_cast<uint8_t>(frame.transparent)
                                         : frame.raw_transparent_index);
    put_byte(out, 0x00);
}

void put_lzw(std::vector<uint8_t>& out, const Frame& frame, bool reblock) {
    put_byte(out, frame.lzw_min_code_size);
    std::size_t pos = 0;
    if (!reblock && !frame.block_sizes.empty()) {
        for (const uint8_t n : frame.block_sizes) {
            if (n == 0 || pos + n > frame.lzw.size()) break;
            put_byte(out, n);
            const auto at = static_cast<std::ptrdiff_t>(pos);
            out.insert(out.end(), frame.lzw.begin() + at, frame.lzw.begin() + at + n);
            pos += n;
        }
    }
    while (pos < frame.lzw.size()) {
        const std::size_t n = std::min<std::size_t>(255, frame.lzw.size() - pos);
        put_byte(out, static_cast<uint8_t>(n));
        const auto at = static_cast<std::ptrdiff_t>(pos);
        out.insert(out.end(), frame.lzw.begin() + at, frame.lzw.begin() + at + static_cast<std::ptrdiff_t>(n));
        pos += n;
    }
    put_byte(out, 0x00);
}

}  // namespace

std::vector<uint8_t> write_gif(const Stream& stream, const WriteOptions& options) {
    std::vector<uint8_t> out;
    out.reserve(1024);

    out.insert(out.end(), {'G', 'I', 'F'});
    const std::string version = stream.version.size() == 3 ? stream.version : std::string("89a");
    out.insert(out.end(), version.begin(), version.end());

    put_word(out, stream.screen_width);
    put_word(out, stream.screen_height);
    uint8_t packed = static_cast<uint8_t>((stream.color_resolution & 7) << 4);
    if (stream.global) {
        packed |= 0x80;
        packed |= colormap_size_bits(stream.global->size());
        if (stream.global->sorted) packed |= 0x08;
    }
    put_byte(out, packed);
    put_byte(out, stream.background);
    put_byte(out, stream.pixel_aspect_ratio);
    if (stream.global) put_colormap(out, *stream.global);

    for (const auto& frame : stream.frames) {
        const std::size_t gce_at = frame.has_gce
                                       ? std::min(frame.gce_position, frame.extensions.size())
                                       : frame.extensions.size();
        for (std::size_t i = 0; i < frame.extensions.size(); ++i) {
            if (i == gce_at && frame.has_gce) put_gce(out, frame);
            put_extension(out, frame.extensions[i]);
        }
        if (frame.has_gce && gce_at == frame.extensions.size()) put_gce(out, frame);

        put_byte(out, 0x2C);
        put_word(out, frame.left);
        put_word(out, frame.top);
        put_word(out, frame.width);
        put_word(out, frame.height);
        uint8_t flags = 0;
        if (frame.local) {
            flags |= 0x80;
            flags |= colormap_size_bits(frame.local->size());
            if (frame.local->sorted) flags |= 0x20;
        } else {
            flags |= frame.raw_local_size_bits & 7;
        }
        if (frame.interlaced) flags |= 0x40;
        put_byte(out, flags);
        if (frame.local) put_colormap(out, *frame.local);

        put_lzw(out, frame, options.reblock_lzw);
    }

    for (const auto& ext : stream.trailing_extensions) put_extension(out, ext);
    put_byte(out, 0x3B);
    return out;
}

bool write_gif_file(const Stream& stream, const std::filesystem::path& path,
                    const WriteOptions& options) {
    const auto bytes = write_gif(stream, options);
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    const bool ok = std::fclose(f) == 0 && written == bytes.size();
    return ok;
}

}  // namespace gifout

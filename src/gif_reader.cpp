#include "gif_reader.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "gif_lzw.hpp"

namespace gifout {

bool Extension::is_application(std::string_view name) const {
    if (label != 0xFF || blocks.empty()) return false;
    const auto& id = blocks.front();
    return id.size() >= name.size() &&
           std::memcmp(id.data(), name.data(), name.size()) == 0;
}

bool ReadResult::has_errors() const {
    for (const auto& d : diagnostics)
        if (d.severity == Severity::Error) return true;
    return false;
}

namespace {

// cursor that never reads past the end, so a truncated file degrades instead of crashing
class Cursor {
public:
    explicit Cursor(std::span<const uint8_t> data) : data_(data) {}

    [[nodiscard]] std::size_t offset() const { return pos_; }
    [[nodiscard]] bool exhausted() const { return pos_ >= data_.size(); }
    [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }

    [[nodiscard]] int peek() const { return pos_ < data_.size() ? data_[pos_] : -1; }

    int byte() {
        if (pos_ >= data_.size()) return -1;
        return data_[pos_++];
    }

    int word() {
        const int lo = byte();
        const int hi = byte();
        if (lo < 0 || hi < 0) return -1;
        return lo | (hi << 8);
    }

    std::span<const uint8_t> take(std::size_t n) {
        n = std::min(n, remaining());
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }

private:
    std::span<const uint8_t> data_;
    std::size_t pos_ = 0;
};

class Reader {
public:
    Reader(std::span<const uint8_t> data, const ReadOptions& options)
        : cursor_(data), options_(options) {}

    ReadResult run() {
        if (!read_signature()) return std::move(result_);
        read_screen_descriptor();

        std::vector<Extension> pending;
        bool has_gce = false;
        std::size_t gce_position = 0;
        Frame gce_frame;

        while (!cursor_.exhausted()) {
            const std::size_t block_offset = cursor_.offset();
            const int marker = cursor_.byte();
            if (marker < 0) break;

            if (marker == 0x3B) {
                trailer_seen_ = true;
                break;
            }
            if (marker == 0x21) {
                Extension ext;
                if (!read_extension(ext)) break;
                if (ext.label == 0xF9) {
                    if (has_gce)
                        note(Severity::Warning, block_offset,
                             "second graphic control extension before an image, first dropped");
                    has_gce = apply_gce(ext, gce_frame, block_offset);
                    gce_position = pending.size();
                    continue;
                }
                if (ext.is_application("NETSCAPE2.0")) read_loopcount(ext);
                pending.push_back(std::move(ext));
                continue;
            }
            if (marker == 0x2C) {
                Frame frame;
                if (has_gce) {
                    frame.has_gce = true;
                    frame.disposal = gce_frame.disposal;
                    frame.user_input = gce_frame.user_input;
                    frame.transparent = gce_frame.transparent;
                    frame.raw_transparent_index = gce_frame.raw_transparent_index;
                    frame.delay = gce_frame.delay;
                    frame.gce_position = gce_position;
                }
                frame.extensions = std::move(pending);
                pending.clear();
                has_gce = false;
                gce_position = 0;
                if (!read_image(frame, block_offset)) break;
                result_.stream.frames.push_back(std::move(frame));
                continue;
            }

            note(Severity::Error, block_offset,
                 "unknown block marker 0x" + hex(static_cast<uint8_t>(marker)) + ", stopping");
            break;
        }

        if (has_gce)
            note(Severity::Warning, cursor_.offset(),
                 "graphic control extension with no image after it, dropped");
        result_.stream.trailing_extensions = std::move(pending);

        if (!trailer_seen_)
            note(Severity::Warning, cursor_.offset(), "file ends without a trailer");
        else if (!cursor_.exhausted())
            note(Severity::Info, cursor_.offset(),
                 std::to_string(cursor_.remaining()) + " bytes of junk after the trailer, dropped");

        if (result_.stream.frames.empty())
            note(Severity::Error, cursor_.offset(), "no frames found");
        result_.ok = !result_.stream.frames.empty();
        return std::move(result_);
    }

private:
    static std::string hex(uint8_t v) {
        static const char* digits = "0123456789ABCDEF";
        return std::string{digits[v >> 4], digits[v & 15]};
    }

    void note(Severity severity, std::size_t offset, std::string message) {
        if (result_.diagnostics.size() >= options_.max_diagnostics) return;
        if (result_.diagnostics.size() + 1 == options_.max_diagnostics) {
            result_.diagnostics.push_back({Severity::Info, offset, "further diagnostics suppressed"});
            return;
        }
        result_.diagnostics.push_back({severity, offset, std::move(message)});
    }

    bool read_signature() {
        auto magic = cursor_.take(6);
        if (magic.size() < 6 || std::memcmp(magic.data(), "GIF", 3) != 0) {
            note(Severity::Error, 0, "not a gif file");
            return false;
        }
        result_.stream.version.assign(reinterpret_cast<const char*>(magic.data()) + 3, 3);
        if (result_.stream.version != "87a" && result_.stream.version != "89a")
            note(Severity::Warning, 3, "unknown gif version '" + result_.stream.version + "'");
        return true;
    }

    void read_screen_descriptor() {
        auto& s = result_.stream;
        s.screen_width = static_cast<uint16_t>(std::max(0, cursor_.word()));
        s.screen_height = static_cast<uint16_t>(std::max(0, cursor_.word()));
        const int packed = cursor_.byte();
        s.background = static_cast<uint8_t>(std::max(0, cursor_.byte()));
        s.pixel_aspect_ratio = static_cast<uint8_t>(std::max(0, cursor_.byte()));
        if (packed < 0) {
            note(Severity::Error, cursor_.offset(), "truncated logical screen descriptor");
            return;
        }
        s.color_resolution = static_cast<uint8_t>((packed >> 4) & 7);
        if (packed & 0x80) {
            Colormap map;
            map.sorted = (packed & 0x08) != 0;
            read_colormap(map, 2u << (packed & 7));
            s.global = std::move(map);
        }
    }

    void read_colormap(Colormap& map, unsigned entries) {
        auto bytes = cursor_.take(static_cast<std::size_t>(entries) * 3);
        if (bytes.size() < static_cast<std::size_t>(entries) * 3)
            note(Severity::Warning, cursor_.offset(), "truncated colormap");
        map.colors.reserve(bytes.size() / 3);
        for (std::size_t i = 0; i + 2 < bytes.size(); i += 3)
            map.colors.push_back({bytes[i], bytes[i + 1], bytes[i + 2]});
    }

    bool read_extension(Extension& ext) {
        const int label = cursor_.byte();
        if (label < 0) {
            note(Severity::Warning, cursor_.offset(), "truncated extension");
            return false;
        }
        ext.label = static_cast<uint8_t>(label);
        while (true) {
            const int len = cursor_.byte();
            if (len < 0) {
                note(Severity::Warning, cursor_.offset(), "truncated extension block");
                return false;
            }
            if (len == 0) return true;
            auto payload = cursor_.take(static_cast<std::size_t>(len));
            if (payload.size() < static_cast<std::size_t>(len)) {
                note(Severity::Warning, cursor_.offset(), "truncated extension block");
                ext.blocks.emplace_back(payload.begin(), payload.end());
                return false;
            }
            ext.blocks.emplace_back(payload.begin(), payload.end());
        }
    }

    bool apply_gce(const Extension& ext, Frame& frame, std::size_t offset) {
        if (ext.blocks.empty() || ext.blocks.front().size() < 4) {
            note(Severity::Warning, offset, "malformed graphic control extension, ignored");
            return false;
        }
        const auto& b = ext.blocks.front();
        frame.disposal = static_cast<Disposal>((b[0] >> 2) & 7);
        frame.user_input = (b[0] & 0x02) != 0;
        frame.delay = static_cast<uint16_t>(b[1] | (b[2] << 8));
        frame.transparent = (b[0] & 0x01) ? b[3] : -1;
        frame.raw_transparent_index = b[3];
        if (((b[0] >> 2) & 7) > 3)
            note(Severity::Warning, offset, "reserved disposal method, kept as is");
        return true;
    }

    void read_loopcount(const Extension& ext) {
        for (std::size_t i = 1; i < ext.blocks.size(); ++i) {
            const auto& b = ext.blocks[i];
            if (b.size() >= 3 && b[0] == 1) {
                result_.stream.loopcount = b[1] | (b[2] << 8);
                return;
            }
        }
    }

    bool read_image(Frame& frame, std::size_t offset) {
        frame.left = static_cast<uint16_t>(std::max(0, cursor_.word()));
        frame.top = static_cast<uint16_t>(std::max(0, cursor_.word()));
        frame.width = static_cast<uint16_t>(std::max(0, cursor_.word()));
        frame.height = static_cast<uint16_t>(std::max(0, cursor_.word()));
        const int packed = cursor_.byte();
        if (packed < 0) {
            note(Severity::Error, offset, "truncated image descriptor");
            return false;
        }
        frame.interlaced = (packed & 0x40) != 0;
        frame.raw_local_size_bits = static_cast<uint8_t>(packed & 7);
        if (packed & 0x80) {
            Colormap map;
            map.sorted = (packed & 0x20) != 0;
            read_colormap(map, 2u << (packed & 7));
            frame.local = std::move(map);
        }

        const int min_code_size = cursor_.byte();
        if (min_code_size < 0) {
            note(Severity::Error, cursor_.offset(), "truncated image data");
            return false;
        }
        frame.lzw_min_code_size = static_cast<uint8_t>(min_code_size);

        while (true) {
            const int len = cursor_.byte();
            if (len < 0) {
                note(Severity::Warning, cursor_.offset(), "image data ends without a terminator");
                break;
            }
            if (len == 0) break;
            auto payload = cursor_.take(static_cast<std::size_t>(len));
            frame.lzw.insert(frame.lzw.end(), payload.begin(), payload.end());
            frame.block_sizes.push_back(static_cast<uint8_t>(payload.size()));
            if (payload.size() < static_cast<std::size_t>(len)) {
                note(Severity::Warning, cursor_.offset(), "truncated image data block");
                break;
            }
        }

        if (frame.width == 0 || frame.height == 0) {
            note(Severity::Warning, offset, "frame with zero width or height");
            return true;
        }
        if (options_.decode_pixels) {
            auto decoded = decode_lzw(frame.lzw, frame.lzw_min_code_size, frame.width,
                                      frame.height, frame.interlaced, result_.diagnostics, offset);
            frame.pixels = std::move(decoded.pixels);
        }
        return true;
    }

    Cursor cursor_;
    ReadOptions options_;
    ReadResult result_;
    bool trailer_seen_ = false;
};

}  // namespace

ReadResult read_gif(std::span<const uint8_t> data, const ReadOptions& options) {
    return Reader(data, options).run();
}

ReadResult read_gif_file(const std::filesystem::path& path, const ReadOptions& options) {
    ReadResult result;
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        result.diagnostics.push_back({Severity::Error, 0, "cannot open " + path.string()});
        return result;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<std::size_t>(size) : 0);
    const std::size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    data.resize(got);
    if (data.empty()) {
        result.diagnostics.push_back({Severity::Error, 0, "empty file " + path.string()});
        return result;
    }
    return read_gif(data, options);
}

}  // namespace gifout

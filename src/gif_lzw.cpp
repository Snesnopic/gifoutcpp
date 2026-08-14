#include "gifoutcpp/gif_lzw.hpp"

#include <array>
#include <string>

namespace gifout {
namespace {

constexpr int kMaxCodeBits = 12;
constexpr int kMaxCodes = 1 << kMaxCodeBits;

// walks the four interlace passes so the rest of the codebase never sees interlacing
class RowOrder {
public:
    RowOrder(uint16_t height, bool interlaced) : height_(height), interlaced_(interlaced) {}

    [[nodiscard]] uint16_t current() const { return row_; }
    [[nodiscard]] bool done() const { return row_ >= height_; }

    void advance() {
        if (!interlaced_) {
            ++row_;
            return;
        }
        static constexpr std::array<uint16_t, 4> kStart{0, 4, 2, 1};
        static constexpr std::array<uint16_t, 4> kStep{8, 8, 4, 2};
        row_ += kStep[pass_];
        while (row_ >= height_ && pass_ < 3) {
            ++pass_;
            row_ = kStart[pass_];
        }
    }

private:
    uint16_t height_;
    bool interlaced_;
    uint16_t row_ = 0;
    int pass_ = 0;
};

}  // namespace

DecodeResult decode_lzw(std::span<const uint8_t> lzw, uint8_t min_code_size,
                        uint16_t width, uint16_t height, bool interlaced,
                        std::vector<Diagnostic>& diagnostics, std::size_t offset_hint) {
    DecodeResult result;
    const std::size_t total = static_cast<std::size_t>(width) * height;
    result.pixels.assign(total, 0);

    auto warn = [&](std::string message) {
        diagnostics.push_back({Severity::Warning, offset_hint, std::move(message)});
    };

    if (min_code_size < 2 || min_code_size > 8) {
        // encoders do emit 1 for two colour images, clamp like gifsicle instead of rejecting
        warn("min code size " + std::to_string(min_code_size) + " out of range, clamped");
        min_code_size = min_code_size < 2 ? 2 : 8;
    }

    const int clear_code = 1 << min_code_size;
    const int eoi_code = clear_code + 1;

    std::array<uint16_t, kMaxCodes> prefix{};
    std::array<uint8_t, kMaxCodes> suffix{};
    std::array<uint16_t, kMaxCodes> length{};
    for (int i = 0; i < clear_code; ++i) {
        prefix[i] = kMaxCodes;  // sentinel: no prefix
        suffix[i] = static_cast<uint8_t>(i);
        length[i] = 1;
    }

    int next_code = eoi_code + 1;
    int code_bits = min_code_size + 1;
    int previous = -1;

    RowOrder rows(height, interlaced);
    std::size_t column = 0;
    std::size_t written = 0;
    std::vector<uint8_t> expand;
    expand.reserve(kMaxCodes);

    auto emit = [&](uint8_t index) {
        if (rows.done()) return;
        result.pixels[static_cast<std::size_t>(rows.current()) * width + column] = index;
        ++written;
        if (++column == width) {
            column = 0;
            rows.advance();
        }
    };

    std::size_t bit_pos = 0;
    const std::size_t bit_end = lzw.size() * 8;
    bool truncated = false;
    bool saw_eoi = false;

    while (!rows.done()) {
        if (bit_pos + static_cast<std::size_t>(code_bits) > bit_end) {
            truncated = true;
            break;
        }
        // gif packs codes little-endian across byte boundaries
        unsigned code = 0;
        for (int i = 0; i < code_bits; ++i) {
            const std::size_t p = bit_pos + i;
            code |= static_cast<unsigned>((lzw[p >> 3] >> (p & 7)) & 1) << i;
        }
        bit_pos += code_bits;

        if (static_cast<int>(code) == clear_code) {
            next_code = eoi_code + 1;
            code_bits = min_code_size + 1;
            previous = -1;
            continue;
        }
        if (static_cast<int>(code) == eoi_code) {
            saw_eoi = true;
            break;
        }

        int emit_code;
        if (static_cast<int>(code) < next_code && (static_cast<int>(code) < clear_code ||
                                                   length[code] != 0)) {
            emit_code = static_cast<int>(code);
        } else if (static_cast<int>(code) == next_code && previous >= 0) {
            // the classic kwkwk case: the code being defined right now
            emit_code = -1;
        } else {
            warn("lzw code out of range, stopping frame early");
            break;
        }

        expand.clear();
        if (emit_code >= 0) {
            int walk = emit_code;
            while (walk != kMaxCodes) {
                expand.push_back(suffix[walk]);
                walk = prefix[walk];
            }
        } else {
            int walk = previous;
            while (walk != kMaxCodes) {
                expand.push_back(suffix[walk]);
                walk = prefix[walk];
            }
            expand.insert(expand.begin(), expand.back());
        }
        for (auto it = expand.rbegin(); it != expand.rend(); ++it) emit(*it);

        if (previous >= 0 && next_code < kMaxCodes) {
            prefix[next_code] = static_cast<uint16_t>(previous);
            suffix[next_code] = expand.back();
            length[next_code] = static_cast<uint16_t>(length[previous] + 1);
            ++next_code;
            // deferred clear is legal: a full dictionary may run on without a clear code
            if (next_code == (1 << code_bits) && code_bits < kMaxCodeBits) ++code_bits;
        }
        previous = emit_code >= 0 ? emit_code : next_code - 1;
    }

    if (truncated) warn("lzw stream ends before the frame is complete");
    if (!truncated && !saw_eoi && !rows.done()) warn("lzw stream lacks an end-of-information code");
    result.complete = written == total && !truncated;
    if (written != total && !truncated)
        warn("decoded " + std::to_string(written) + " of " + std::to_string(total) + " pixels");
    return result;
}

}  // namespace gifout

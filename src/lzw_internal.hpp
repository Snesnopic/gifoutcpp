#ifndef GIFOUTCPP_LZW_INTERNAL_HPP
#define GIFOUTCPP_LZW_INTERNAL_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gifout::detail {

constexpr int kMaxCodeBits = 12;
constexpr int kMaxCode = 1 << kMaxCodeBits;

// gifsicle's deferred clear heuristic, ported verbatim: it tracks an exponentially
// weighted average of match lengths and restarts the dictionary once matches get
// short relative to how much of the frame is left
constexpr int kRunEwmaShift = 4;
constexpr int kRunEwmaScale = 19;
constexpr unsigned kRunInvThresh = (1u << kRunEwmaScale) / 3000;

// open addressing beats a trie here: the whole table is 96 kB and stays in cache,
// and a clear costs one counter bump instead of touching every entry
class Dictionary {
public:
    Dictionary() : key_(kSlots, 0), value_(kSlots, 0), stamp_(kSlots, 0) {}

    void clear() {
        if (++epoch_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            epoch_ = 1;
        }
    }

    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | suffix;
        for (uint32_t slot = hash(key);; slot = (slot + 1) & (kSlots - 1)) {
            if (stamp_[slot] != epoch_) return -1;
            if (key_[slot] == key) return value_[slot];
        }
    }

    void insert(int prefix, uint8_t suffix, int code) {
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | suffix;
        for (uint32_t slot = hash(key);; slot = (slot + 1) & (kSlots - 1)) {
            if (stamp_[slot] != epoch_ || key_[slot] == key) {
                stamp_[slot] = epoch_;
                key_[slot] = key;
                value_[slot] = static_cast<uint16_t>(code);
                return;
            }
        }
    }

private:
    static constexpr uint32_t kSlots = 1u << 13;  // twice the largest dictionary

    static uint32_t hash(uint32_t key) { return (key * 2654435761u >> 13) & (kSlots - 1); }

    std::vector<uint32_t> key_;
    std::vector<uint16_t> value_;
    std::vector<uint32_t> stamp_;
    uint32_t epoch_ = 1;
};

// bit sink with a rewind, which the clear heuristic needs when it decides after the
// fact that the restart belonged further back
class BitSink {
public:
    void put(unsigned code, int bits) {
        while (bits > 0) {
            if ((bit_pos_ & 7) == 0) bytes_.push_back(0);
            const int free_bits = 8 - (bit_pos_ & 7);
            const int take = std::min(bits, free_bits);
            bytes_.back() |= static_cast<uint8_t>((code & ((1u << take) - 1)) << (bit_pos_ & 7));
            code >>= take;
            bits -= take;
            bit_pos_ += static_cast<std::size_t>(take);
        }
    }

    [[nodiscard]] std::size_t bit_pos() const { return bit_pos_; }

    void rewind(std::size_t bit_pos) {
        bit_pos_ = bit_pos;
        bytes_.resize((bit_pos + 7) / 8);
        if (bit_pos & 7) bytes_.back() &= static_cast<uint8_t>((1u << (bit_pos & 7)) - 1);
    }

    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
    std::size_t bit_pos_ = 0;
};

// the pixels in the order the encoder walks them. laying them out once costs one byte
// per pixel and one pass; computing the mapping instead costs a division and a modulo
// per pixel, inside the loop the whole project spends its time in
class PixelOrder {
public:
    PixelOrder(std::span<const uint8_t> pixels, uint16_t width, uint16_t height, bool interlaced) {
        const std::size_t count = std::min<std::size_t>(
            pixels.size(), static_cast<std::size_t>(width) * height);
        if (!interlaced) {
            ordered_.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(count));
            return;
        }
        ordered_.reserve(count);
        static constexpr std::array<uint16_t, 4> start{0, 4, 2, 1};
        static constexpr std::array<uint16_t, 4> step{8, 8, 4, 2};
        for (int pass = 0; pass < 4; ++pass)
            for (uint16_t y = start[pass]; y < height; y = static_cast<uint16_t>(y + step[pass])) {
                const std::size_t row = static_cast<std::size_t>(y) * width;
                for (uint16_t x = 0; x < width && row + x < count; ++x)
                    ordered_.push_back(pixels[row + x]);
            }
    }

    [[nodiscard]] uint8_t at(std::size_t pos) const { return ordered_[pos]; }
    [[nodiscard]] std::size_t size() const { return ordered_.size(); }

private:
    std::vector<uint8_t> ordered_;
};

inline uint8_t min_code_bits_for(std::span<const uint8_t> pixels, unsigned palette_size, bool careful) {
    int colors_used;
    if (careful) {
        colors_used = static_cast<int>(palette_size ? palette_size : 256);
    } else {
        // the code size follows the highest index in the data, not the palette, so an
        // oversized palette costs nothing; gifsicle stops early once 8 bits are certain
        colors_used = 0;
        for (uint8_t p : pixels) {
            if (p > colors_used) colors_used = p;
            if (colors_used >= 128) break;
        }
        ++colors_used;
    }
    int bits = 2;
    for (int i = 4; i < colors_used; i *= 2) ++bits;
    return static_cast<uint8_t>(std::min(bits, 8));
}

}  // namespace gifout::detail

#endif  // GIFOUTCPP_LZW_INTERNAL_HPP

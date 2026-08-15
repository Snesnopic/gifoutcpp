/**
 * @file lzw_internal.hpp
 * @brief Internal: the pieces every LZW path shares, dictionary, bit sink, pixel order.
 *
 * Not part of the public surface. Included by the encoder, the restart search and the
 * parse search so that all three agree on how a dictionary behaves and how bits land.
 */
#ifndef OPTIGIF_LZW_INTERNAL_HPP
#define OPTIGIF_LZW_INTERNAL_HPP

#include <algorithm>
#include <bit>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace optigif::detail {

/** @brief Widest LZW code GIF allows. */
constexpr int kMaxCodeBits = 12;
/** @brief One past the largest code, so 4096. */
constexpr int kMaxCode = 1 << kMaxCodeBits;

// gifsicle's deferred clear heuristic, ported verbatim: it tracks an exponentially
// weighted average of match lengths and restarts the dictionary once matches get
// short relative to how much of the frame is left
/** @brief Smoothing shift of the run length average. */
constexpr int kRunEwmaShift = 4;
/** @brief Fixed point scale of the run length average. */
constexpr int kRunEwmaScale = 19;
/** @brief Threshold that decides a dictionary restart is due. */
constexpr unsigned kRunInvThresh = (1u << kRunEwmaScale) / 3000;

/**
 * @brief The LZW dictionary, as an open addressed table with epoch invalidation.
 *
 * Open addressing beats a trie here: the whole table stays in cache, and clearing costs
 * one counter bump instead of touching every entry.
 */
class Dictionary {
public:
    Dictionary() = default;

    /** @brief Forgets every entry, in constant time. */
    void clear() {
        // wraps after four billion clears, and then every stamp must go: rare, not never
        // cppcheck-suppress knownConditionTrueFalse
        if (++epoch_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            epoch_ = 1;
        }
    }

    /**
     * @brief Looks up the code for a prefix code followed by one byte.
     * @param prefix Code of the sequence so far.
     * @param suffix Byte that extends it.
     * @return The child code, or -1 when the pair is not in the table.
     */
    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | suffix;
        for (uint32_t slot = hash(key);; slot = (slot + 1) & (kSlots - 1)) {
            if (stamp_[slot] != epoch_) return -1;
            if (key_[slot] == key) return value_[slot];
        }
    }

    /**
     * @brief Defines a new dictionary entry.
     * @param prefix Code of the sequence so far.
     * @param suffix Byte that extends it.
     * @param code   Code to assign to the extended sequence.
     */
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

    // sized here rather than in a constructor body: the table is fixed and every lookup
    // assumes it is already there
    std::vector<uint32_t> key_ = std::vector<uint32_t>(kSlots, 0);
    std::vector<uint16_t> value_ = std::vector<uint16_t>(kSlots, 0);
    std::vector<uint32_t> stamp_ = std::vector<uint32_t>(kSlots, 0);
    uint32_t epoch_ = 1;
};

/**
 * @brief A little endian bit sink that can rewind.
 *
 * The clear heuristic decides after the fact that a restart belonged further back, so
 * the sink has to be able to give those bits up again.
 */
class BitSink {
public:
    /**
     * @brief Appends a code, least significant bit first.
     * @param code Value to write.
     * @param bits How many of its low bits to write.
     */
    void put(unsigned code, int bits) {
        while (bits > 0) {
            const int used = static_cast<int>(bit_pos_ & 7);
            if (used == 0) bytes_.push_back(0);
            const int free_bits = 8 - used;
            const int take = std::min(bits, free_bits);
            bytes_.back() |= static_cast<uint8_t>((code & ((1u << take) - 1)) << used);
            code >>= take;
            bits -= take;
            bit_pos_ += static_cast<std::size_t>(take);
        }
    }

    /** @return How many bits have been written so far. */
    [[nodiscard]] std::size_t bit_pos() const { return bit_pos_; }

    /**
     * @brief Throws away everything written past a bit position.
     * @param to Bit position to return to.
     */
    void rewind(std::size_t to) {
        bit_pos_ = to;
        bytes_.resize((to + 7) / 8);
        if (to & 7) bytes_.back() &= static_cast<uint8_t>((1u << (to & 7)) - 1);
    }

    /**
     * @brief Hands over the bytes written, leaving the sink empty.
     * @return Everything put into the sink so far.
     */
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
    std::size_t bit_pos_ = 0;
};

/**
 * @brief The pixels in the order the encoder walks them, interlacing already resolved.
 *
 * Laying them out once costs one byte per pixel and one pass. Computing the mapping
 * instead costs a division and a modulo per pixel, inside the loop this project spends
 * most of its time in.
 */
class PixelOrder {
public:
    /**
     * @brief Lays the frame out in encode order.
     * @param pixels     Indices in natural row order.
     * @param width      Frame width in pixels.
     * @param height     Frame height in pixels.
     * @param interlaced Walk the four GIF passes rather than straight down.
     */
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
            for (uint16_t y = start[static_cast<std::size_t>(pass)]; y < height;
                 y = static_cast<uint16_t>(y + step[static_cast<std::size_t>(pass)])) {
                const std::size_t row = static_cast<std::size_t>(y) * width;
                for (uint16_t x = 0; x < width && row + x < count; ++x)
                    ordered_.push_back(pixels[row + x]);
            }
    }

    /**
     * @brief Reads a pixel in encode order.
     * @param pos Linear position along the encode order.
     * @return The pixel the encoder reaches there.
     */
    [[nodiscard]] uint8_t at(std::size_t pos) const { return ordered_[pos]; }
    /** @return How many pixels there are to encode. */
    [[nodiscard]] std::size_t size() const { return ordered_.size(); }

private:
    std::vector<uint8_t> ordered_;
};

/**
 * @brief Smallest initial code width that can carry these pixels.
 * @param pixels       The indices to encode.
 * @param palette_size Entries in the table they refer to.
 * @param careful      Derive the width from the palette rather than from the data.
 * @return A width between 2 and 8.
 */
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
    // the width that holds the largest index, which is what bit_width answers; gif
    // forbids a code size below 2 and the dictionary caps it at 8
    const int bits = colors_used > 1 ? std::bit_width(static_cast<unsigned>(colors_used - 1)) : 1;
    return static_cast<uint8_t>(std::clamp(bits, 2, 8));
}

}  // namespace optigif::detail

#endif  // OPTIGIF_LZW_INTERNAL_HPP

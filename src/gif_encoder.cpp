#include "gif_encoder.hpp"

#include <algorithm>
#include <array>

namespace gifout {
namespace {

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

// maps a linear encode position to a pixel, honouring the interlace pass order
class PixelOrder {
public:
    PixelOrder(std::span<const uint8_t> pixels, uint16_t width, uint16_t height, bool interlaced)
        : pixels_(pixels), width_(width) {
        rows_.reserve(height);
        if (!interlaced) {
            for (uint16_t y = 0; y < height; ++y) rows_.push_back(y);
        } else {
            static constexpr std::array<uint16_t, 4> start{0, 4, 2, 1};
            static constexpr std::array<uint16_t, 4> step{8, 8, 4, 2};
            for (int pass = 0; pass < 4; ++pass)
                for (uint16_t y = start[pass]; y < height; y = static_cast<uint16_t>(y + step[pass]))
                    rows_.push_back(y);
        }
    }

    [[nodiscard]] uint8_t at(std::size_t pos) const {
        const std::size_t row = rows_[pos / width_];
        return pixels_[row * width_ + pos % width_];
    }

private:
    std::span<const uint8_t> pixels_;
    uint16_t width_;
    std::vector<uint16_t> rows_;
};

uint8_t min_code_bits_for(std::span<const uint8_t> pixels, unsigned palette_size, bool careful) {
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

EncodeResult encode_pass(const PixelOrder& order, std::size_t end_pos, uint8_t min_code_bits,
                         bool eager_clear) {
    EncodeResult result;
    result.min_code_size = min_code_bits;

    const int clear_code = 1 << min_code_bits;
    const int eoi_code = clear_code + 1;

    Dictionary dict;
    BitSink sink;

    unsigned run = 0;
    unsigned run_ewma = 0;
    std::size_t pos = 0;
    std::size_t clear_pos = 0, clear_bit_pos = 0;
    int work_node = -1;
    int next_code = 0;
    int cur_code_bits = min_code_bits + 1;
    int output_code = clear_code;

    while (true) {
        sink.put(static_cast<unsigned>(output_code), cur_code_bits);

        if (output_code == clear_code) {
            cur_code_bits = min_code_bits + 1;
            next_code = clear_code + 2;
            run_ewma = 1u << kRunEwmaScale;
            run = 0;
            dict.clear();
            clear_pos = clear_bit_pos = 0;
        } else if (output_code == eoi_code) {
            break;
        } else {
            if (next_code > (1 << cur_code_bits) && cur_code_bits < kMaxCodeBits) ++cur_code_bits;
            run = (run << kRunEwmaScale) + (1u << (kRunEwmaShift - 1));
            if (run < run_ewma)
                run_ewma -= (run_ewma - run) >> kRunEwmaShift;
            else
                run_ewma += (run - run_ewma) >> kRunEwmaShift;
            run = work_node >= 0 ? 1 : 0;
        }

        bool emitted = false;
        while (pos < end_pos) {
            uint8_t suffix = order.at(pos);
            if (suffix >= clear_code) suffix = 0;  // only reachable with a careful code size
            const int next_node = work_node < 0 ? suffix : dict.find(work_node, suffix);
            ++pos;
            if (next_node >= 0) {
                work_node = next_node;
                ++run;
                continue;
            }

            if (next_code < kMaxCode) {
                dict.insert(work_node, suffix, next_code);
                ++next_code;
            } else {
                next_code = kMaxCode + 1;
            }

            if (next_code > 4094) {
                bool do_clear = eager_clear;
                if (!do_clear) {
                    const std::size_t pixels_left = end_pos - pos;
                    if (pixels_left) {
                        if (run_ewma < ((36u << kRunEwmaScale) / min_code_bits) ||
                            pixels_left > UINT32_MAX / kRunInvThresh ||
                            run_ewma < pixels_left * kRunInvThresh)
                            do_clear = true;
                    }
                }
                if ((do_clear || run < 7) && clear_pos == 0) {
                    clear_pos = pos - (run + 1);
                    clear_bit_pos = sink.bit_pos();
                } else if (!do_clear && run > 50) {
                    clear_pos = clear_bit_pos = 0;
                }
                if (do_clear) {
                    output_code = clear_code;
                    pos = clear_pos;
                    sink.rewind(clear_bit_pos);
                    work_node = -1;
                    result.cleared = true;
                    emitted = true;
                    break;
                }
            }

            output_code = work_node;
            work_node = suffix;
            emitted = true;
            break;
        }

        if (!emitted) {
            output_code = work_node >= 0 ? work_node : eoi_code;
            work_node = -1;
        }
    }

    result.lzw = sink.take();
    return result;
}

}  // namespace

EncodeResult encode_lzw(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                        bool interlaced, unsigned palette_size, const EncodeOptions& options) {
    EncodeResult result;
    if (width == 0 || height == 0 || pixels.empty()) return result;

    const uint8_t min_code_bits =
        options.min_code_size ? options.min_code_size
                              : min_code_bits_for(pixels, palette_size, options.careful_min_code_size);

    const PixelOrder order(pixels, width, height, interlaced);
    const std::size_t end_pos = std::min<std::size_t>(
        pixels.size(), static_cast<std::size_t>(width) * height);

    result = encode_pass(order, end_pos, min_code_bits, options.eager_clear);
    if (options.try_both_clear_policies && !options.eager_clear && result.cleared) {
        EncodeResult eager = encode_pass(order, end_pos, min_code_bits, true);
        if (eager.lzw.size() < result.lzw.size()) result = std::move(eager);
    }
    return result;
}

void encode_frame(Frame& frame, unsigned palette_size, const EncodeOptions& options) {
    auto encoded = encode_lzw(frame.pixels, frame.width, frame.height, frame.interlaced,
                              palette_size, options);
    frame.lzw = std::move(encoded.lzw);
    frame.lzw_min_code_size = encoded.min_code_size;
    frame.block_sizes.clear();
}

unsigned effective_palette_size(const Frame& frame, const Stream& stream) {
    if (frame.local) return static_cast<unsigned>(frame.local->size());
    if (stream.global) return static_cast<unsigned>(stream.global->size());
    return 256;
}

}  // namespace gifout

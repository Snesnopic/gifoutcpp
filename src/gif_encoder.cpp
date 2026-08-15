#include "optigif/gif_encoder.hpp"

#include <algorithm>

#include "lzw_internal.hpp"

namespace optigif {
using namespace detail;
namespace {

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
            // the decoder defines an entry for this last code too, so the width it
            // expects for the end of stream code has to be computed the same way
            if (work_node >= 0 && next_code < kMaxCode) ++next_code;
            output_code = work_node >= 0 ? work_node : eoi_code;
            work_node = -1;
        }
    }

    result.lzw = sink.take();
    return result;
}

// a handful of entries defined during a simulated lookahead, consulted before the real
// dictionary and thrown away afterwards. the alternative, mutating and then undoing the
// real table, cannot be done safely with open addressing.
class Overlay {
public:
    void clear() { count_ = 0; }

    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        for (unsigned i = 0; i < count_; ++i)
            if (entries_[i].prefix == prefix && entries_[i].suffix == suffix)
                return entries_[i].code;
        return -1;
    }

    void insert(int prefix, uint8_t suffix, int code) {
        if (count_ < kMax) entries_[count_++] = {prefix, code, suffix};
    }

private:
    static constexpr unsigned kMax = 32;
    struct Entry {
        int prefix, code;
        uint8_t suffix;
    };
    std::array<Entry, kMax> entries_{};
    unsigned count_ = 0;
};

// the longest match from `pos`, looking through the overlay first
std::pair<int, std::size_t> longest_match(const Dictionary& dict, const Overlay& overlay,
                                          const PixelOrder& order, std::size_t pos,
                                          std::size_t end) {
    int code = order.at(pos);
    std::size_t length = 1;
    while (pos + length < end) {
        const uint8_t suffix = order.at(pos + length);
        int child = overlay.find(code, suffix);
        if (child < 0) child = dict.find(code, suffix);
        if (child < 0) break;
        code = child;
        ++length;
    }
    return {code, length};
}

int code_for(const Dictionary& dict, const Overlay& overlay, const PixelOrder& order,
             std::size_t pos, std::size_t length) {
    int code = order.at(pos);
    for (std::size_t i = 1; i < length; ++i) {
        const uint8_t suffix = order.at(pos + i);
        const int child = overlay.find(code, suffix);
        code = child >= 0 ? child : dict.find(code, suffix);
    }
    return code;
}

// one block, choosing each token by looking a few tokens ahead. the same routine both
// decides and emits, so the cost model cannot drift away from the bytes written
EncodeResult encode_with_lookahead(const PixelOrder& order, std::size_t end_pos,
                                   uint8_t min_code_bits, unsigned depth, unsigned probe) {
    EncodeResult result;
    result.min_code_size = min_code_bits;
    const int clear_code = 1 << min_code_bits;

    Dictionary dict;
    Overlay overlay;
    BitSink sink;
    int next_code = clear_code + 2;
    int code_bits = min_code_bits + 1;
    std::size_t pos = 0;

    sink.put(static_cast<unsigned>(clear_code), code_bits);
    while (pos < end_pos) {
        overlay.clear();
        const auto [greedy_code, greedy_len] = longest_match(dict, overlay, order, pos, end_pos);
        (void)greedy_code;
        std::size_t choice = greedy_len;

        if (depth > 1 && greedy_len > 1) {
            double best = 1e30;
            for (std::size_t len = greedy_len; len >= 1 && len + depth > greedy_len; --len) {
                overlay.clear();
                unsigned bits = 0;
                int width = code_bits;
                int codes = next_code;
                std::size_t at = pos;
                std::size_t taken = len;
                for (unsigned step = 0; step <= probe && at < end_pos; ++step) {
                    if (step > 0) {
                        const auto [c, l] = longest_match(dict, overlay, order, at, end_pos);
                        (void)c;
                        taken = l;
                    }
                    const int code = code_for(dict, overlay, order, at, taken);
                    bits += static_cast<unsigned>(width);
                    if (codes < kMaxCode) {
                        if (at + taken < end_pos)
                            overlay.insert(code, order.at(at + taken), codes);
                        ++codes;
                    }
                    if (codes > (1 << width) && width < kMaxCodeBits) ++width;
                    at += taken;
                }
                const double score = static_cast<double>(bits) / static_cast<double>(at - pos);
                if (score < best - 1e-12) {
                    best = score;
                    choice = len;
                }
            }
            overlay.clear();
        }

        const int code = code_for(dict, overlay, order, pos, choice);
        sink.put(static_cast<unsigned>(code), code_bits);
        if (next_code < kMaxCode) {
            if (pos + choice < end_pos) dict.insert(code, order.at(pos + choice), next_code);
            ++next_code;
        }
        if (next_code > (1 << code_bits) && code_bits < kMaxCodeBits) ++code_bits;
        pos += choice;
    }
    sink.put(static_cast<unsigned>(clear_code + 1), code_bits);
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
    if (options.lookahead > 1) {
        EncodeResult smart = encode_with_lookahead(order, end_pos, min_code_bits, options.lookahead,
                                                   options.lookahead_probe);
        // only if it really came out smaller: a lookahead is a heuristic and does lose
        if (!smart.lzw.empty() && smart.lzw.size() < result.lzw.size()) result = std::move(smart);
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

}  // namespace optigif

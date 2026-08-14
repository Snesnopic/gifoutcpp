#include "gifoutcpp/gif_encoder.hpp"

#include <algorithm>

#include "lzw_internal.hpp"

namespace gifout {
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

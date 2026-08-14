#include "gifoutcpp/gif_lzw_search.hpp"

#include <limits>

#include "gif_parallel.hpp"

#include "lzw_internal.hpp"

namespace gifout {
using namespace detail;
namespace {

constexpr unsigned long long kNoPath = std::numeric_limits<unsigned long long>::max();

// one block of lzw codes, from a cleared dictionary. the walk down the trie also
// returns the code it ended on, so the entry can be inserted without walking the
// same chain a second time, which is half the memory traffic of the obvious version
struct BlockEncoder {
    const PixelOrder& order;
    std::size_t end_pos;
    uint8_t min_code_bits;
    Dictionary& dict;

    int clear_code() const { return 1 << min_code_bits; }

    // walks from `from`, reporting the running cost at every aligned position through
    // `report(position, bits_including_terminator)`. report returns false to give up on
    // this start, which is safe exactly when the accumulated bits already beat anything
    // the rest of the walk could reach. stops at max_tokens too.
    template <typename Report>
    void walk(std::size_t from, unsigned max_tokens, unsigned alignment, Report report) const {
        dict.clear();
        int next_code = clear_code() + 2;
        int code_bits = min_code_bits + 1;
        unsigned long long bits = 0;
        unsigned tokens = 0;
        std::size_t pos = from;

        while (pos < end_pos) {
            if (max_tokens && tokens >= max_tokens) return;
            const std::size_t token_start = pos;
            int code = order.at(pos);
            ++pos;
            uint8_t suffix = 0;
            bool mismatch = false;
            while (pos < end_pos) {
                suffix = order.at(pos);
                const int child = dict.find(code, suffix);
                if (child < 0) {
                    mismatch = true;
                    break;
                }
                code = child;
                ++pos;
            }

            // a block may also end inside this token: every prefix of a match is itself
            // a code, so the last token just comes out shorter
            for (std::size_t q = (token_start / alignment + 1) * alignment; q < pos; q += alignment)
                if (!report(q, bits + 2ull * static_cast<unsigned>(code_bits))) return;

            bits += static_cast<unsigned>(code_bits);
            ++tokens;
            if (next_code < kMaxCode) {
                if (mismatch) dict.insert(code, suffix, next_code);
                ++next_code;
            }
            if (next_code > (1 << code_bits) && code_bits < kMaxCodeBits) ++code_bits;

            // a restart here would cost one more code, of the width in force right now
            if (pos == end_pos || pos % alignment == 0)
                if (!report(pos, bits + static_cast<unsigned>(code_bits))) return;
        }
    }

    // emits a block that the search already decided on, into `sink`
    void emit(std::size_t from, std::size_t to, bool final_block, BitSink& sink) const {
        dict.clear();
        int next_code = clear_code() + 2;
        int code_bits = min_code_bits + 1;
        std::size_t pos = from;

        while (pos < to) {
            int code = order.at(pos);
            ++pos;
            uint8_t suffix = 0;
            bool mismatch = false;
            while (pos < to) {
                suffix = order.at(pos);
                const int child = dict.find(code, suffix);
                if (child < 0) {
                    mismatch = true;
                    break;
                }
                code = child;
                ++pos;
            }

            sink.put(static_cast<unsigned>(code), code_bits);
            if (next_code < kMaxCode) {
                if (mismatch) dict.insert(code, suffix, next_code);
                ++next_code;
            }
            if (next_code > (1 << code_bits) && code_bits < kMaxCodeBits) ++code_bits;
        }
        sink.put(static_cast<unsigned>(final_block ? clear_code() + 1 : clear_code()), code_bits);
    }
};

}  // namespace

SearchResult encode_lzw_search(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                               bool interlaced, unsigned palette_size,
                               const EncodeOptions& encode_options,
                               const SearchOptions& search_options) {
    SearchResult result;
    if (width == 0 || height == 0 || pixels.empty()) return result;

    const uint8_t min_code_bits =
        encode_options.min_code_size
            ? encode_options.min_code_size
            : min_code_bits_for(pixels, palette_size, encode_options.careful_min_code_size);

    const PixelOrder order(pixels, width, height, interlaced);
    const std::size_t end_pos =
        std::min<std::size_t>(pixels.size(), static_cast<std::size_t>(width) * height);
    const unsigned alignment = std::max(1u, search_options.alignment);

    // slot k covers position k * alignment; the last slot is the end of the frame
    const std::size_t slots = end_pos / alignment + 1;
    std::vector<unsigned long long> best(slots + 1, kNoPath);
    std::vector<std::size_t> next_pos(slots + 1, end_pos);
    best[slots] = 0;

    auto slot_of = [&](std::size_t pos) { return pos >= end_pos ? slots : pos / alignment; };

    // the forward walks do not read the dp table for anything inside their own chunk, so
    // a chunk's walks are independent and the only sequential part is folding the few
    // candidates that land inside it. the answer is the same whatever the thread count.
    const unsigned threads = resolve_threads(search_options.threads);
    constexpr std::size_t kChunk = 64;
    struct Candidate {
        std::size_t pos;
        unsigned long long bits;
    };
    std::vector<std::vector<Candidate>> inside(kChunk);
    std::vector<unsigned long long> far_best(kChunk);
    std::vector<std::size_t> far_end(kChunk);
    std::vector<Dictionary> dicts(std::max(1u, threads));

    for (std::size_t high = slots; high > 0;) {
        const std::size_t low = high > kChunk ? high - kChunk : 0;
        const std::size_t width_of_chunk = high - low;

        parallel_for(width_of_chunk, threads, [&](std::size_t idx, unsigned worker) {
            const std::size_t k = low + idx;
            inside[idx].clear();
            far_best[idx] = kNoPath;
            far_end[idx] = end_pos;
            const std::size_t from = k * alignment;
            if (from >= end_pos) return;
            const BlockEncoder block{order, end_pos, min_code_bits,
                                     dicts[std::min<std::size_t>(worker, dicts.size() - 1)]};
            block.walk(from, search_options.max_tokens, alignment,
                       [&](std::size_t pos, unsigned long long bits) {
                           const std::size_t slot = slot_of(pos);
                           if (slot >= high) {
                               const unsigned long long tail = best[slot];
                               if (tail != kNoPath) {
                                   const unsigned long long total = bits + tail;
                                   if (total < far_best[idx]) {
                                       far_best[idx] = total;
                                       far_end[idx] = pos;
                                   }
                               }
                           } else {
                               inside[idx].push_back({pos, bits});
                           }
                           // the tail of any longer block costs at least nothing, so once
                           // the block alone is worse than the best complete path found,
                           // no later end can win and the walk is over
                           return bits < far_best[idx];
                       });
        });

        for (std::size_t idx = width_of_chunk; idx-- > 0;) {
            const std::size_t k = low + idx;
            if (k * alignment >= end_pos) continue;
            unsigned long long local_best = far_best[idx];
            std::size_t local_end = far_end[idx];
            for (const auto& c : inside[idx]) {
                const unsigned long long tail = best[slot_of(c.pos)];
                if (tail == kNoPath) continue;
                const unsigned long long total = c.bits + tail;
                if (total < local_best) {
                    local_best = total;
                    local_end = c.pos;
                }
            }
            best[k] = local_best;
            next_pos[k] = local_end;
        }
        high = low;
    }

    if (best[0] == kNoPath) return result;  // no path: max_tokens too small for alignment

    Dictionary dict;
    const BlockEncoder block{order, end_pos, min_code_bits, dict};
    BitSink sink;
    // a leading clear code is conventional and every decoder expects to survive it
    sink.put(static_cast<unsigned>(1 << min_code_bits), min_code_bits + 1);
    std::size_t pos = 0;
    while (pos < end_pos) {
        const std::size_t to = next_pos[slot_of(pos)];
        const bool final_block = to >= end_pos;
        block.emit(pos, to, final_block, sink);
        ++result.blocks;
        pos = to;
    }

    result.encoded.lzw = sink.take();
    result.encoded.min_code_size = min_code_bits;
    result.encoded.cleared = result.blocks > 1;
    result.bits = best[0] + min_code_bits + 1;
    result.searched = true;
    return result;
}

}  // namespace gifout

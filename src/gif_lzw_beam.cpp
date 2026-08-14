#include "gifoutcpp/gif_lzw_beam.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "lzw_internal.hpp"

namespace gifout {
using namespace detail;
namespace {

// small enough to copy per candidate, which is exactly what carrying a dictionary
// along each branch of the search requires
struct Table {
    static constexpr uint32_t kSlots = 1u << 13;
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

    std::vector<uint32_t> key = std::vector<uint32_t>(kSlots, kEmpty);
    std::vector<uint16_t> value = std::vector<uint16_t>(kSlots, 0);
    int next_code = 0;
    int code_bits = 0;

    static uint32_t slot_of(uint32_t k) { return (k * 2654435761u >> 13) & (kSlots - 1); }
    static uint32_t pack(int prefix, uint8_t suffix) {
        return (static_cast<uint32_t>(prefix) << 8) | suffix;
    }

    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        const uint32_t k = pack(prefix, suffix);
        for (uint32_t i = slot_of(k);; i = (i + 1) & (kSlots - 1)) {
            if (key[i] == kEmpty) return -1;
            if (key[i] == k) return value[i];
        }
    }

    void insert(int prefix, uint8_t suffix, int code) {
        const uint32_t k = pack(prefix, suffix);
        for (uint32_t i = slot_of(k);; i = (i + 1) & (kSlots - 1)) {
            if (key[i] == kEmpty || key[i] == k) {
                key[i] = k;
                value[i] = static_cast<uint16_t>(code);
                return;
            }
        }
    }

    [[nodiscard]] std::size_t longest(const PixelOrder& order, std::size_t from,
                                      std::size_t end) const {
        int code = order.at(from);
        std::size_t length = 1;
        while (from + length < end) {
            const int child = find(code, order.at(from + length));
            if (child < 0) break;
            code = child;
            ++length;
        }
        return length;
    }

    [[nodiscard]] int code_for(const PixelOrder& order, std::size_t from,
                               std::size_t length) const {
        int code = order.at(from);
        for (std::size_t i = 1; i < length; ++i) code = find(code, order.at(from + i));
        return code;
    }
};

struct Step {
    std::shared_ptr<const Step> parent;
    std::size_t from = 0, length = 0;
};

struct State {
    Table table;
    unsigned bits = 0;
    std::shared_ptr<const Step> steps;
    // cost at a position says nothing about the dictionary that came with it, so the
    // greedy line is never pruned: that alone makes the search unable to lose
    bool greedy = true;
};

std::vector<std::pair<std::size_t, std::size_t>> unwind(const std::shared_ptr<const Step>& tail) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const Step* s = tail.get(); s; s = s->parent.get()) out.emplace_back(s->from, s->length);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<uint8_t> emit(const PixelOrder& order, uint8_t min_code_bits,
                          const std::vector<std::pair<std::size_t, std::size_t>>& steps,
                          std::size_t end_pos) {
    BitSink sink;
    Table table;
    table.next_code = (1 << min_code_bits) + 2;
    table.code_bits = min_code_bits + 1;
    sink.put(static_cast<unsigned>(1 << min_code_bits), table.code_bits);
    for (const auto& [from, length] : steps) {
        const int code = table.code_for(order, from, length);
        sink.put(static_cast<unsigned>(code), table.code_bits);
        if (table.next_code < kMaxCode) {
            if (from + length < end_pos)
                table.insert(code, order.at(from + length), table.next_code);
            ++table.next_code;
        }
        if (table.next_code > (1 << table.code_bits) && table.code_bits < kMaxCodeBits)
            ++table.code_bits;
    }
    sink.put(static_cast<unsigned>((1 << min_code_bits) + 1), table.code_bits);
    return sink.take();
}

std::vector<uint8_t> search_once(const PixelOrder& order, std::size_t end, uint8_t min_code_bits,
                                 std::size_t beam, std::size_t candidates) {
    State start;
    start.table.next_code = (1 << min_code_bits) + 2;
    start.table.code_bits = min_code_bits + 1;

    std::map<std::size_t, std::vector<State>> frontier;
    frontier[0].push_back(std::move(start));
    std::shared_ptr<const Step> best_steps;
    unsigned best_bits = 0;
    bool have_best = false;

    while (!frontier.empty()) {
        auto node = frontier.begin();
        const std::size_t pos = node->first;
        std::vector<State> states = std::move(node->second);
        frontier.erase(node);
        if (states.size() > beam) {
            std::partial_sort(states.begin(), states.begin() + static_cast<long>(beam), states.end(),
                              [](const State& a, const State& b) {
                                  if (a.greedy != b.greedy) return a.greedy;
                                  return a.bits < b.bits;
                              });
            states.resize(beam);
        }
        for (State& state : states) {
            if (pos >= end) {
                const unsigned total = state.bits + static_cast<unsigned>(state.table.code_bits);
                if (!have_best || total < best_bits) {
                    best_bits = total;
                    best_steps = state.steps;
                    have_best = true;
                }
                continue;
            }
            const std::size_t longest = state.table.longest(order, pos, end);
            for (std::size_t len = longest; len >= 1 && len + candidates > longest; --len) {
                State next = state;
                const int code = next.table.code_for(order, pos, len);
                next.bits += static_cast<unsigned>(next.table.code_bits);
                if (next.table.next_code < kMaxCode) {
                    if (pos + len < end) next.table.insert(code, order.at(pos + len), next.table.next_code);
                    ++next.table.next_code;
                }
                if (next.table.next_code > (1 << next.table.code_bits) &&
                    next.table.code_bits < kMaxCodeBits)
                    ++next.table.code_bits;
                next.greedy = state.greedy && len == longest;
                next.steps = std::make_shared<Step>(Step{state.steps, pos, len});
                frontier[pos + len].push_back(std::move(next));
            }
        }
    }
    if (!have_best) return {};
    return emit(order, min_code_bits, unwind(best_steps), end);
}

}  // namespace

BeamResult encode_lzw_beam(std::span<const uint8_t> pixels, uint16_t width, uint16_t height,
                           bool interlaced, unsigned palette_size,
                           const EncodeOptions& encode_options, const BeamOptions& beam_options) {
    BeamResult result;
    if (width == 0 || height == 0 || pixels.empty() || beam_options.width == 0) return result;
    const std::size_t end =
        std::min<std::size_t>(pixels.size(), static_cast<std::size_t>(width) * height);
    if (end > beam_options.max_pixels) return result;

    const uint8_t min_code_bits =
        encode_options.min_code_size
            ? encode_options.min_code_size
            : min_code_bits_for(pixels, palette_size, encode_options.careful_min_code_size);
    const PixelOrder order(pixels, width, height, interlaced);

    // the greedy encoder is the floor, and every width is tried in turn: whatever the
    // effort, the answer only ever gets smaller
    result.encoded = encode_lzw(pixels, width, height, interlaced, palette_size, encode_options);
    for (std::size_t beam = 1; beam <= beam_options.width; beam *= 4) {
        auto bytes = search_once(order, end, min_code_bits, beam, beam_options.candidates);
        if (bytes.empty() || bytes.size() >= result.encoded.lzw.size()) continue;
        result.encoded.lzw = std::move(bytes);
        result.encoded.min_code_size = min_code_bits;
        result.width_used = beam;
    }
    result.searched = true;
    return result;
}

}  // namespace gifout

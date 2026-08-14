// second experiment: how close to the ceiling does the lookahead get?
//
// a beam search keeps the B cheapest ways of having reached each position, each with
// its own dictionary, instead of committing to one. B = 1 is the greedy encoder, and
// growing B walks toward the optimal parse the format does not let anybody compute.
// what matters here is the shape of the curve: if B = 16 barely beats B = 4, the
// simple machine is worth building and the complicated one is not.

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gifoutcpp/gifoutcpp.hpp"

namespace {

constexpr uint32_t kSlots = 1u << 13;
constexpr uint32_t kEmpty = 0xFFFFFFFFu;

// small enough to copy per candidate, which is what a beam needs
struct Dict {
    std::vector<uint32_t> key = std::vector<uint32_t>(kSlots, kEmpty);
    std::vector<uint16_t> val = std::vector<uint16_t>(kSlots, 0);
    int next_code = 0;
    int code_bits = 0;

    static uint32_t hash(uint32_t k) { return (k * 2654435761u >> 13) & (kSlots - 1); }
    static uint32_t pack(int prefix, uint8_t suffix) {
        return (static_cast<uint32_t>(prefix) << 8) | suffix;
    }

    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        const uint32_t k = pack(prefix, suffix);
        for (uint32_t i = hash(k);; i = (i + 1) & (kSlots - 1)) {
            if (key[i] == kEmpty) return -1;
            if (key[i] == k) return val[i];
        }
    }

    void insert(int prefix, uint8_t suffix, int code) {
        const uint32_t k = pack(prefix, suffix);
        for (uint32_t i = hash(k);; i = (i + 1) & (kSlots - 1)) {
            if (key[i] == kEmpty || key[i] == k) {
                key[i] = k;
                val[i] = static_cast<uint16_t>(code);
                return;
            }
        }
    }

    // longest match and its code, plus the code for any shorter prefix of it
    [[nodiscard]] std::pair<int, std::size_t> longest(const std::vector<uint8_t>& data,
                                                      std::size_t from, std::size_t end) const {
        int code = data[from];
        std::size_t length = 1;
        while (from + length < end) {
            const int child = find(code, data[from + length]);
            if (child < 0) break;
            code = child;
            ++length;
        }
        return {code, length};
    }

    [[nodiscard]] int code_for(const std::vector<uint8_t>& data, std::size_t from,
                               std::size_t length) const {
        int code = data[from];
        for (std::size_t i = 1; i < length; ++i) code = find(code, data[from + i]);
        return code;
    }
};

struct Token {
    std::shared_ptr<const Token> parent;
    std::size_t from = 0, length = 0;
};

struct State {
    Dict dict;
    unsigned bits = 0;
    std::shared_ptr<const Token> tokens;
    // two states can reach the same position with the same cost and very different
    // dictionaries, so cost alone is not a safe thing to prune on: the greedy lineage
    // is kept whatever it costs, which makes the beam unable to lose
    bool greedy = true;
};

std::vector<std::pair<std::size_t, std::size_t>> unwind(const std::shared_ptr<const Token>& tail) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const Token* t = tail.get(); t; t = t->parent.get()) out.emplace_back(t->from, t->length);
    std::reverse(out.begin(), out.end());
    return out;
}

// beam over positions: a state that reaches the end of the frame cheapest wins
State search(const std::vector<uint8_t>& data, int min_code_bits, std::size_t beam,
             std::size_t widths) {
    const std::size_t end = data.size();
    State start;
    start.dict.next_code = (1 << min_code_bits) + 2;
    start.dict.code_bits = min_code_bits + 1;

    std::map<std::size_t, std::vector<State>> frontier;
    frontier[0].push_back(std::move(start));
    State best;
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
                const unsigned total = state.bits + static_cast<unsigned>(state.dict.code_bits);
                if (!have_best || total < best.bits) {
                    best = std::move(state);
                    best.bits = total;
                    have_best = true;
                }
                continue;
            }
            const auto [code, longest] = state.dict.longest(data, pos, end);
            (void)code;
            for (std::size_t len = longest; len >= 1 && len + widths > longest; --len) {
                State next = state;  // the dictionary comes along, which is the whole point
                const int emitted = next.dict.code_for(data, pos, len);
                next.bits += static_cast<unsigned>(next.dict.code_bits);
                if (next.dict.next_code < 4096) {
                    if (pos + len < end) next.dict.insert(emitted, data[pos + len], next.dict.next_code);
                    ++next.dict.next_code;
                }
                if (next.dict.next_code > (1 << next.dict.code_bits) && next.dict.code_bits < 12)
                    ++next.dict.code_bits;
                next.greedy = state.greedy && len == longest;
                next.tokens = std::make_shared<Token>(Token{state.tokens, pos, len});
                frontier[pos + len].push_back(std::move(next));
            }
        }
    }
    return best;
}

std::vector<uint8_t> emit(const std::vector<uint8_t>& data, int min_code_bits,
                          const std::vector<std::pair<std::size_t, std::size_t>>& tokens) {
    std::vector<uint8_t> bytes;
    std::size_t bit_pos = 0;
    auto put = [&](unsigned code, int width) {
        for (int i = 0; i < width; ++i) {
            if ((bit_pos & 7) == 0) bytes.push_back(0);
            if (code & (1u << i)) bytes.back() |= static_cast<uint8_t>(1u << (bit_pos & 7));
            ++bit_pos;
        }
    };
    Dict dict;
    dict.next_code = (1 << min_code_bits) + 2;
    dict.code_bits = min_code_bits + 1;
    put(static_cast<unsigned>(1 << min_code_bits), min_code_bits + 1);
    for (const auto& [from, length] : tokens) {
        const int code = dict.code_for(data, from, length);
        put(static_cast<unsigned>(code), dict.code_bits);
        if (dict.next_code < 4096) {
            if (from + length < data.size()) dict.insert(code, data[from + length], dict.next_code);
            ++dict.next_code;
        }
        if (dict.next_code > (1 << dict.code_bits) && dict.code_bits < 12) ++dict.code_bits;
    }
    put(static_cast<unsigned>((1 << min_code_bits) + 1), dict.code_bits);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: beam_probe <in.gif> [--max-pixels N] [--widths N]\n");
        return 2;
    }
    std::size_t max_pixels = 20000, widths = 3;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-pixels" && i + 1 < argc) max_pixels = std::stoul(argv[++i]);
        else if (arg == "--widths" && i + 1 < argc) widths = std::stoul(argv[++i]);
        else files.push_back(arg);
    }

    for (const auto& path : files) {
        auto read = gifout::read_gif_file(path);
        if (!read.ok) continue;
        for (std::size_t f = 0; f < read.stream.frames.size(); ++f) {
            const auto& frame = read.stream.frames[f];
            if (!frame.pixels_complete || frame.pixels.size() > max_pixels ||
                frame.pixels.empty())
                continue;
            const int mcb = frame.lzw_min_code_size < 2 ? 2 : frame.lzw_min_code_size;
            std::printf("%s frame %zu, %zu pixels\n",
                        path.substr(path.find_last_of('/') + 1).c_str(), f, frame.pixels.size());
            unsigned baseline = 0;
            for (const std::size_t beam : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                                           std::size_t{8}, std::size_t{16}}) {
                const State best = search(frame.pixels, mcb, beam, widths);
                const auto tokens = unwind(best.tokens);
                const auto bytes = emit(frame.pixels, mcb, tokens);
                std::vector<gifout::Diagnostic> diagnostics;
                const auto back = gifout::decode_lzw(bytes, static_cast<uint8_t>(mcb), frame.width,
                                                     frame.height, false, diagnostics);
                const bool sound = back.pixels == frame.pixels && back.complete;
                if (beam == 1) baseline = best.bits;
                std::printf("   beam %2zu: %8u bits  %+7.3f%%  %s\n", beam, best.bits,
                            baseline ? 100.0 * (double(best.bits) / double(baseline) - 1.0) : 0.0,
                            sound ? "decodes" : "DECODE MISMATCH");
            }
            break;  // one frame per file is enough to see the shape
        }
    }
    return 0;
}

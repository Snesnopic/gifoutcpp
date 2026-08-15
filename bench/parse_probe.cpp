// throwaway experiment: how much is left on the table by parsing greedily?
//
// lzw's dictionary is built from the encoder's own choices, so taking a shorter match
// now can put a better entry in the dictionary and pay off later. the greedy encoder
// never sees that. this measures the prize before anybody builds the machine.
//
// the emitter is the cost model: every candidate parse is really encoded and really
// decoded back, because the one thing that went wrong in flexigif's attempt was a cost
// model that disagreed with the bytes it wrote.

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "optigif/optigif.hpp"

namespace {

using optigif::decode_lzw;

// a dictionary that can undo its last insertions, which is what a lookahead needs
class Trial {
public:
    explicit Trial(int min_code_bits)
        : clear_(1 << min_code_bits), next_code_(clear_ + 2), code_bits_(min_code_bits + 1) {}

    [[nodiscard]] int find(int prefix, uint8_t suffix) const {
        const auto it = table_.find(key(prefix, suffix));
        return it == table_.end() ? -1 : it->second;
    }

    // longest match starting at `from`, returning its code and its length
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

    // the code for exactly `length` pixels from `from`, walking the same chain
    [[nodiscard]] int code_for(const std::vector<uint8_t>& data, std::size_t from,
                               std::size_t length) const {
        int code = data[from];
        for (std::size_t i = 1; i < length; ++i) code = find(code, data[from + i]);
        return code;
    }

    int emit(const std::vector<uint8_t>& data, std::size_t from, std::size_t length,
             std::size_t end, unsigned& bits) {
        const int code = code_for(data, from, length);
        bits += static_cast<unsigned>(code_bits_);
        undo_.push_back({0, code_bits_, next_code_, false});
        if (next_code_ < 4096) {
            if (from + length < end) {
                const uint32_t k = key(code, data[from + length]);
                if (table_.find(k) == table_.end()) {
                    table_.emplace(k, next_code_);
                    undo_.back().key = k;
                    undo_.back().inserted = true;
                }
            }
            ++next_code_;
        }
        if (next_code_ > (1 << code_bits_) && code_bits_ < 12) ++code_bits_;
        return code;
    }

    void rewind(std::size_t to) {
        while (undo_.size() > to) {
            const Undo& u = undo_.back();
            if (u.inserted) table_.erase(u.key);
            code_bits_ = u.code_bits;
            next_code_ = u.next_code;
            undo_.pop_back();
        }
    }

    [[nodiscard]] std::size_t mark() const { return undo_.size(); }
    [[nodiscard]] int code_bits() const { return code_bits_; }

private:
    struct Undo {
        uint32_t key;
        int code_bits;
        int next_code;
        bool inserted;
    };
    static uint32_t key(int prefix, uint8_t suffix) {
        return (static_cast<uint32_t>(prefix) << 8) | suffix;
    }

    std::unordered_map<uint32_t, int> table_;
    std::vector<Undo> undo_;
    int clear_, next_code_, code_bits_;
};

struct Parse {
    std::vector<std::pair<std::size_t, std::size_t>> tokens;  // (from, length)
    unsigned bits = 0;
};

// depth = how many shorter matches to consider, probe = how many tokens to look ahead
Parse parse(const std::vector<uint8_t>& data, int min_code_bits, unsigned depth, unsigned probe) {
    Parse out;
    Trial dict(min_code_bits);
    const std::size_t end = data.size();
    std::size_t pos = 0;
    while (pos < end) {
        const auto [greedy_code, greedy_len] = dict.longest(data, pos, end);
        std::size_t choice = greedy_len;
        if (depth > 1 && greedy_len > 1) {
            double best_score = 1e30;
            for (std::size_t len = greedy_len; len >= 1 && len + depth > greedy_len; --len) {
                const std::size_t mark = dict.mark();
                unsigned bits = 0;
                std::size_t at = pos;
                dict.emit(data, at, len, end, bits);
                at += len;
                for (unsigned p = 0; p < probe && at < end; ++p) {
                    const auto [c, l] = dict.longest(data, at, end);
                    (void)c;
                    dict.emit(data, at, l, end, bits);
                    at += l;
                }
                dict.rewind(mark);
                const double score = static_cast<double>(bits) / static_cast<double>(at - pos);
                if (score < best_score - 1e-12) {
                    best_score = score;
                    choice = len;
                }
            }
        }
        unsigned bits = 0;
        dict.emit(data, pos, choice, end, bits);
        out.bits += bits;
        out.tokens.emplace_back(pos, choice);
        pos += choice;
    }
    out.bits += static_cast<unsigned>(dict.code_bits());  // end of information
    return out;
}

// write the parse out for real, so it can be decoded back
std::vector<uint8_t> emit(const std::vector<uint8_t>& data, int min_code_bits, const Parse& parse) {
    std::vector<uint8_t> bytes;
    std::size_t bit_pos = 0;
    auto put = [&](unsigned code, int width) {
        for (int i = 0; i < width; ++i) {
            if ((bit_pos & 7) == 0) bytes.push_back(0);
            if (code & (1u << i)) bytes.back() |= static_cast<uint8_t>(1u << (bit_pos & 7));
            ++bit_pos;
        }
    };
    Trial dict(min_code_bits);
    put(static_cast<unsigned>(1 << min_code_bits), min_code_bits + 1);
    for (const auto& [from, length] : parse.tokens) {
        const int width = dict.code_bits();
        unsigned ignored = 0;
        const int code = dict.emit(data, from, length, data.size(), ignored);
        put(static_cast<unsigned>(code), width);
    }
    put(static_cast<unsigned>((1 << min_code_bits) + 1), dict.code_bits());
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: parse_probe <in.gif>... [--depth N] [--probe N]\n");
        return 2;
    }
    unsigned depth = 3, probe = 2;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--depth" && i + 1 < argc) depth = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (arg == "--probe" && i + 1 < argc) probe = static_cast<unsigned>(std::stoul(argv[++i]));
        else files.push_back(arg);
    }

    unsigned long long total_greedy = 0, total_smart = 0;
    for (const auto& path : files) {
        auto read = optigif::read_gif_file(path);
        if (!read.ok) continue;
        unsigned long long greedy_bits = 0, smart_bits = 0;
        bool sound = true;
        for (const auto& frame : read.stream.frames) {
            if (!frame.pixels_complete || frame.pixels.empty()) continue;
            const int mcb = frame.lzw_min_code_size < 2 ? 2 : frame.lzw_min_code_size;
            const Parse a = parse(frame.pixels, mcb, 1, 0);
            const Parse b = parse(frame.pixels, mcb, depth, probe);
            greedy_bits += a.bits;
            smart_bits += b.bits;

            // both parses must really decode back to the frame
            for (const Parse* p : {&a, &b}) {
                const auto bytes = emit(frame.pixels, mcb, *p);
                std::vector<optigif::Diagnostic> diagnostics;
                const auto back = decode_lzw(bytes, static_cast<uint8_t>(mcb), frame.width,
                                             frame.height, false, diagnostics);
                if (back.pixels != frame.pixels || !back.complete) sound = false;
            }
        }
        total_greedy += greedy_bits;
        total_smart += smart_bits;
        const double gain = greedy_bits ? 100.0 * (1.0 - double(smart_bits) / double(greedy_bits)) : 0;
        std::printf("%-46s greedy %9llu bits, lookahead %9llu, %+6.3f%%%s\n",
                    path.substr(path.find_last_of('/') + 1).substr(0, 46).c_str(), greedy_bits,
                    smart_bits, -gain, sound ? "" : "  DECODE MISMATCH");
    }
    if (total_greedy)
        std::printf("\ntotal: %llu -> %llu bits, %+.3f%% with depth %u probe %u\n", total_greedy,
                    total_smart, 100.0 * (double(total_smart) / double(total_greedy) - 1.0), depth,
                    probe);
    return 0;
}

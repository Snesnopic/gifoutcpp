// same flexigif stage as stage_bench, but with an instrumented copy of the
// encoder so the pre-pass cost can be attributed to dictionary clearing,
// match walking and dp bookkeeping.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "GifImage.hpp"
#include "LzwEncoder_inst.hpp"

using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: inst_bench <in.gif> <out.gif> [maxTokens] [alignment]\n");
        return 2;
    }
    const std::filesystem::path in = argv[1], out = argv[2];
    const unsigned int maxTokens = argc > 3 ? std::stoul(argv[3]) : 10000;
    const unsigned int alignment = argc > 4 ? std::stoul(argv[4]) : 10;
    const bool greedy = argc > 5 ? std::stoi(argv[5]) != 0 : true;

    const auto t0 = clk::now();
    GifImage gif(in.string(), false);
    const unsigned int numFrames = gif.getNumFrames();
    std::vector<std::vector<bool>> optimizedBits;

    double prepass = 0, finalpass = 0;
    unsigned long long pixels = 0;
    for (unsigned int f = 0; f < numFrames; f++) {
        const auto& frame = gif.getFrame(f);
        const auto& indices = frame.pixels;
        if (indices.empty()) { optimizedBits.emplace_back(); continue; }
        pixels += indices.size();

        LzwEncoderInst encoder(indices, true);
        LzwEncoderInst::OptimizationSettings s{};
        s.minCodeSize = frame.codeSize;
        s.startWithClearCode = true;
        s.verbose = false;
        s.greedy = greedy;
        s.minNonGreedyMatch = 2;
        s.minImprovement = 1;
        s.maxDictionary = 4096;
        s.maxTokens = maxTokens;
        s.splitRuns = false;
        s.alignment = alignment;
        s.readOnlyBest = false;
        s.avoidNonGreedyAgain = true;

        auto t = clk::now();
        const int lastPos = static_cast<int>(indices.size()) - 1;
        for (int i = lastPos; i >= 0; i--) {
            if ((i % static_cast<int>(s.alignment)) != 0) continue;
            encoder.optimizePartial(static_cast<unsigned int>(i), 0, false, true, s);
        }
        prepass += std::chrono::duration<double, std::milli>(clk::now() - t).count();

        t = clk::now();
        s.readOnlyBest = true;
        optimizedBits.push_back(encoder.optimize(s));
        finalpass += std::chrono::duration<double, std::milli>(clk::now() - t).count();
    }
    gif.writeOptimized(out.string(), optimizedBits);
    const double total = std::chrono::duration<double, std::milli>(clk::now() - t0).count();

    const auto& g = g_lzwStats;
    std::printf("{\"file\":\"%s\",\"maxTokens\":%u,\"alignment\":%u,\"pixels\":%llu,"
                "\"in\":%llu,\"out\":%llu,\"total_ms\":%.1f,\"prepass_ms\":%.1f,"
                "\"greedy\":%d,\"finalpass_ms\":%.1f,\"clear_ms\":%.1f,\"calls\":%llu,"
                "\"cleared_entries\":%llu,\"tokens\":%llu,\"match_steps\":%llu,"
                "\"addcode_steps\":%llu,\"bytes_scanned\":%llu,\"best_updates\":%llu}\n",
                in.filename().c_str(), maxTokens, alignment, pixels,
                static_cast<unsigned long long>(std::filesystem::file_size(in)),
                static_cast<unsigned long long>(std::filesystem::file_size(out)),
                total, prepass, greedy ? 1 : 0, finalpass, g.clearMs, g.calls, g.clearedEntries,
                g.tokens, g.matchSteps, g.addCodeSteps, g.bytesScanned, g.bestUpdates);
    return 0;
}

// stage_bench: replicates chisel's two GIF stages in isolation so each can be
// timed and profiled separately. the code inside each stage is a faithful copy
// of libchisel/src/processors/{gif,flexigif}_processor.cpp minus logging.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "GifImage.hpp"
#include "LzwEncoder.hpp"

extern "C" {
#include <lcdfgif/gif.h>
}

using clk = std::chrono::steady_clock;
namespace fs = std::filesystem;

static double ms_since(const clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

struct Timings {
    double decode = 0, prepass = 0, finalpass = 0, write = 0, total = 0;
};

static void stage_gifsicle(const fs::path& in, const fs::path& out, bool preserve_metadata) {
    FILE* f = std::fopen(in.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open input");
    Gif_Stream* gfs = Gif_ReadFile(f);
    std::fclose(f);
    if (!gfs) throw std::runtime_error("Gif_ReadFile failed");

    if (!preserve_metadata) {
        if (gfs->end_comment) {
            Gif_DeleteComment(gfs->end_comment);
            gfs->end_comment = nullptr;
        }
        for (int i = 0; i < gfs->nimages; ++i) {
            if (gfs->images[i]->comment) {
                Gif_DeleteComment(gfs->images[i]->comment);
                gfs->images[i]->comment = nullptr;
            }
        }
    }

    FILE* o = std::fopen(out.c_str(), "wb");
    if (!o) { Gif_DeleteStream(gfs); throw std::runtime_error("cannot open output"); }
    Gif_CompressInfo ci = {};
    Gif_InitCompressInfo(&ci);
    const int ok = Gif_FullWriteFile(gfs, &ci, o);
    std::fclose(o);
    Gif_DeleteStream(gfs);
    if (!ok) throw std::runtime_error("Gif_FullWriteFile failed");
}

static void stage_flexigif(const fs::path& in, const fs::path& out,
                           unsigned int maxTokens, Timings& t) {
    auto t0 = clk::now();
    GifImage gif(in.string(), false);
    t.decode += ms_since(t0);

    const unsigned int numFrames = gif.getNumFrames();
    if (numFrames == 0) throw std::runtime_error("no frames");

    std::vector<std::vector<bool>> optimizedBits;
    optimizedBits.reserve(numFrames);

    for (unsigned int frameIndex = 0; frameIndex < numFrames; frameIndex++) {
        const auto& frame = gif.getFrame(frameIndex);
        const auto& indices = frame.pixels;
        if (indices.empty()) { optimizedBits.emplace_back(); continue; }

        LzwEncoder encoder(indices, true);
        LzwEncoder::OptimizationSettings s{};
        s.minCodeSize = frame.codeSize;
        s.startWithClearCode = true;
        s.verbose = false;
        s.greedy = true;
        s.minNonGreedyMatch = 2;
        s.minImprovement = 1;
        s.maxDictionary = 4096;
        s.maxTokens = maxTokens;
        s.splitRuns = false;
        s.alignment = 10;
        s.readOnlyBest = false;
        s.avoidNonGreedyAgain = true;

        t0 = clk::now();
        const int lastPos = static_cast<int>(indices.size()) - 1;
        for (int i = lastPos; i >= 0; i--) {
            if ((i % static_cast<int>(s.alignment)) != 0) continue;
            encoder.optimizePartial(static_cast<unsigned int>(i), 0, false, true, s);
        }
        t.prepass += ms_since(t0);

        t0 = clk::now();
        s.readOnlyBest = true;
        optimizedBits.push_back(encoder.optimize(s));
        t.finalpass += ms_since(t0);
    }

    t0 = clk::now();
    gif.writeOptimized(out.string(), optimizedBits);
    t.write += ms_since(t0);
}

// losslessness check: two gifs are equivalent when they render identically,
// i.e. same frame geometry/timing/disposal and same resolved rgb per pixel
// (palette order may legitimately differ)
static int stage_cmp(const fs::path& a, const fs::path& b) {
    auto load = [](const fs::path& p) -> Gif_Stream* {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return nullptr;
        Gif_Stream* s = Gif_FullReadFile(f, GIF_READ_UNCOMPRESSED, nullptr, nullptr);
        std::fclose(f);
        return s;
    };
    Gif_Stream* sa = load(a);
    Gif_Stream* sb = load(b);
    auto fail = [&](const char* why) {
        std::printf("DIFF %s vs %s: %s\n", a.filename().c_str(), b.filename().c_str(), why);
        if (sa) Gif_DeleteStream(sa);
        if (sb) Gif_DeleteStream(sb);
        return 1;
    };
    if (!sa || !sb) return fail("unreadable");
    if (sa->nimages != sb->nimages) return fail("frame count");
    if (sa->screen_width != sb->screen_width || sa->screen_height != sb->screen_height)
        return fail("screen size");
    if (sa->loopcount != sb->loopcount) return fail("loop count");

    for (int i = 0; i < sa->nimages; ++i) {
        Gif_Image* ia = sa->images[i];
        Gif_Image* ib = sb->images[i];
        if (ia->width != ib->width || ia->height != ib->height ||
            ia->left != ib->left || ia->top != ib->top) return fail("frame geometry");
        if (ia->delay != ib->delay) return fail("frame delay");
        if (ia->disposal != ib->disposal) return fail("frame disposal");
        if ((ia->transparent >= 0) != (ib->transparent >= 0)) return fail("transparency");
        Gif_UncompressImage(sa, ia);
        Gif_UncompressImage(sb, ib);
        Gif_Colormap* ca = ia->local ? ia->local : sa->global;
        Gif_Colormap* cb = ib->local ? ib->local : sb->global;
        for (int y = 0; y < ia->height; ++y) {
            const uint8_t* ra = ia->img[y];
            const uint8_t* rb = ib->img[y];
            for (int x = 0; x < ia->width; ++x) {
                const bool ta = (ia->transparent >= 0 && ra[x] == ia->transparent);
                const bool tb = (ib->transparent >= 0 && rb[x] == ib->transparent);
                if (ta != tb) return fail("transparent pixel mismatch");
                if (ta) continue;
                if (!ca || !cb || ra[x] >= ca->ncol || rb[x] >= cb->ncol)
                    return fail("palette index out of range");
                const Gif_Color& pa = ca->col[ra[x]];
                const Gif_Color& pb = cb->col[rb[x]];
                if (pa.gfc_red != pb.gfc_red || pa.gfc_green != pb.gfc_green ||
                    pa.gfc_blue != pb.gfc_blue) return fail("pixel color mismatch");
            }
        }
    }
    Gif_DeleteStream(sa);
    Gif_DeleteStream(sb);
    std::printf("SAME %s %s\n", a.filename().c_str(), b.filename().c_str());
    return 0;
}

// dump structural properties with gifsicle's reader (compressed mode, so the
// original lzw bytes stay available for per-frame accounting)
static int stage_stat(const fs::path& in) {
    FILE* f = std::fopen(in.c_str(), "rb");
    if (!f) return 1;
    Gif_Stream* gfs = Gif_FullReadFile(f, GIF_READ_COMPRESSED, nullptr, nullptr);
    std::fclose(f);
    if (!gfs) { std::fprintf(stderr, "ERROR stat %s: unreadable\n", in.filename().c_str()); return 1; }

    unsigned long long pixels = 0, lzw_bytes = 0;
    int comments = 0, comment_bytes = 0, local_maps = 0, interlaced = 0, transparent = 0;
    for (int i = 0; i < gfs->nimages; ++i) {
        Gif_Image* gfi = gfs->images[i];
        pixels += static_cast<unsigned long long>(gfi->width) * gfi->height;
        lzw_bytes += gfi->compressed_len;
        if (gfi->comment) { comments += gfi->comment->count; for (int c = 0; c < gfi->comment->count; ++c) comment_bytes += gfi->comment->len[c]; }
        if (gfi->local) local_maps++;
        if (gfi->interlace) interlaced++;
        if (gfi->transparent >= 0) transparent++;
    }
    if (gfs->end_comment) { comments += gfs->end_comment->count; for (int c = 0; c < gfs->end_comment->count; ++c) comment_bytes += gfs->end_comment->len[c]; }

    std::printf("{\"file\":\"%s\",\"bytes\":%llu,\"w\":%d,\"h\":%d,\"frames\":%d,\"pixels\":%llu,"
                "\"lzw_bytes\":%llu,\"comments\":%d,\"comment_bytes\":%d,\"global_map\":%d,"
                "\"local_maps\":%d,\"interlaced\":%d,\"transparent\":%d,\"loop\":%d}\n",
                in.filename().c_str(),
                static_cast<unsigned long long>(fs::file_size(in)),
                gfs->screen_width, gfs->screen_height, gfs->nimages, pixels, lzw_bytes,
                comments, comment_bytes, gfs->global ? gfs->global->ncol : 0,
                local_maps, interlaced, transparent, gfs->loopcount);
    Gif_DeleteStream(gfs);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "stat") return stage_stat(argv[2]);
    if (argc == 4 && std::string(argv[1]) == "cmp") return stage_cmp(argv[2], argv[3]);
    if (argc == 3 && std::string(argv[1]) == "decode") {
        // does flexigif's parser accept this file at all?
        try {
            GifImage g(argv[2], false);
            std::printf("PARSE_OK %s frames=%u\n", fs::path(argv[2]).filename().c_str(),
                        g.getNumFrames());
            return 0;
        } catch (const std::exception& e) {
            std::printf("PARSE_FAIL %s %s\n", fs::path(argv[2]).filename().c_str(), e.what());
            return 1;
        }
    }
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: stage_bench <gifsicle|flexigif|pipe> <in.gif> <out.gif> "
                     "[maxTokens=10000] [preserve_metadata=0]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const fs::path in = argv[2], out = argv[3];
    const unsigned int maxTokens = argc > 4 ? std::stoul(argv[4]) : 10000;
    const bool preserve = argc > 5 && std::stoi(argv[5]) != 0;

    Timings t;
    const auto t0 = clk::now();
    try {
        if (mode == "gifsicle") {
            stage_gifsicle(in, out, preserve);
        } else if (mode == "flexigif") {
            stage_flexigif(in, out, maxTokens, t);
        } else if (mode == "pipe") {
            const fs::path mid = out.string() + ".stage1";
            const auto ts = clk::now();
            stage_gifsicle(in, mid, preserve);
            t.decode = ms_since(ts); // stage1 wall time, reported separately below
            stage_flexigif(mid, out, maxTokens, t);
        } else {
            std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s: %s\n", in.filename().c_str(), e.what());
        return 1;
    }
    t.total = ms_since(t0);

    const auto in_sz = fs::file_size(in);
    const auto out_sz = fs::exists(out) ? fs::file_size(out) : 0;
    std::printf("{\"file\":\"%s\",\"mode\":\"%s\",\"maxTokens\":%u,\"in\":%llu,\"out\":%llu,"
                "\"total_ms\":%.2f,\"decode_ms\":%.2f,\"prepass_ms\":%.2f,"
                "\"finalpass_ms\":%.2f,\"write_ms\":%.2f}\n",
                in.filename().c_str(), mode.c_str(), maxTokens,
                static_cast<unsigned long long>(in_sz),
                static_cast<unsigned long long>(out_sz),
                t.total, t.decode, t.prepass, t.finalpass, t.write);
    return 0;
}

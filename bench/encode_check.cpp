// cross-checks our greedy encoder against gifsicle's on the same pixels. the
// heuristic is ported from gifsicle, so the two must agree byte for byte.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gifoutcpp/gif_encoder.hpp"
#include "gifoutcpp/gif_reader.hpp"

extern "C" {
#include <lcdfgif/gif.h>
}

namespace {

// gifsicle stores the code size and the 255 byte block split inside the buffer
struct Unpacked {
    uint8_t min_code_size = 0;
    std::vector<uint8_t> payload;
};

Unpacked unpack(const uint8_t* data, uint32_t len) {
    Unpacked out;
    if (len == 0) return out;
    out.min_code_size = data[0];
    uint32_t i = 1;
    while (i < len && data[i] != 0) {
        const uint32_t n = data[i];
        ++i;
        for (uint32_t k = 0; k < n && i + k < len; ++k) out.payload.push_back(data[i + k]);
        i += n;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: encode_check <in.gif>\n");
        return 2;
    }
    const char* path = argv[1];

    auto ours = gifout::read_gif_file(path);
    if (!ours.ok) {
        std::printf("SKIP %s: unreadable\n", path);
        return 0;
    }

    std::FILE* f = std::fopen(path, "rb");
    if (!f) return 2;
    Gif_Stream* gfs = Gif_FullReadFile(f, GIF_READ_UNCOMPRESSED, nullptr, nullptr);
    std::fclose(f);
    if (!gfs) {
        std::printf("SKIP %s: gifsicle rejected it\n", path);
        return 0;
    }

    std::size_t ours_total = 0, theirs_total = 0;
    int mismatches = 0;
    const int frames = std::min(static_cast<int>(ours.stream.frames.size()), gfs->nimages);

    for (int i = 0; i < frames; ++i) {
        auto& frame = ours.stream.frames[static_cast<std::size_t>(i)];
        Gif_Image* gfi = gfs->images[i];

        Gif_CompressInfo ci = {};
        Gif_InitCompressInfo(&ci);
        Gif_ReleaseCompressedImage(gfi);
        if (!Gif_FullCompressImage(gfs, gfi, &ci)) {
            std::printf("SKIP %s frame %d: gifsicle failed to compress\n", path, i);
            continue;
        }
        const Unpacked theirs = unpack(gfi->compressed, gfi->compressed_len);

        const unsigned palette = gifout::effective_palette_size(frame, ours.stream);
        const auto mine = gifout::encode_lzw(frame.pixels, frame.width, frame.height,
                                             frame.interlaced, palette);

        ours_total += mine.lzw.size();
        theirs_total += theirs.payload.size();

        if (mine.min_code_size != theirs.min_code_size) {
            std::printf("MISMATCH %s frame %d: min code size %u vs %u\n", path, i,
                        mine.min_code_size, theirs.min_code_size);
            ++mismatches;
            continue;
        }
        if (mine.lzw.size() != theirs.payload.size()) {
            std::printf("MISMATCH %s frame %d: %zu bytes vs %zu\n", path, i, mine.lzw.size(),
                        theirs.payload.size());
            ++mismatches;
            continue;
        }
        if (std::memcmp(mine.lzw.data(), theirs.payload.data(), mine.lzw.size()) != 0) {
            std::size_t at = 0;
            while (at < mine.lzw.size() && mine.lzw[at] == theirs.payload[at]) ++at;
            std::printf("MISMATCH %s frame %d: same size, first differing byte at %zu\n", path, i,
                        at);
            ++mismatches;
        }
    }

    Gif_DeleteStream(gfs);
    if (mismatches == 0)
        std::printf("OK %s %d frames, %zu lzw bytes\n", path, frames, ours_total);
    else
        std::printf("FAILED %s %d of %d frames differ (ours %zu, gifsicle %zu)\n", path, mismatches,
                    frames, ours_total, theirs_total);
    return mismatches == 0 ? 0 : 1;
}

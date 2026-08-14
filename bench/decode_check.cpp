// cross-checks our reader against gifsicle's: same frame geometry, same timing,
// same decoded indices. gifsicle reads 51/51 of the corpus, so it is the oracle.

#include <cstdio>
#include <cstring>
#include <string>

#include "gif_reader.hpp"

extern "C" {
#include <lcdfgif/gif.h>
}

namespace {

int fail(const char* file, const std::string& why) {
    std::printf("MISMATCH %s: %s\n", file, why.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: decode_check <in.gif>\n");
        return 2;
    }
    const char* path = argv[1];

    auto ours = gifout::read_gif_file(path);
    if (!ours.ok) return fail(path, "our reader rejected the file");

    std::FILE* f = std::fopen(path, "rb");
    if (!f) return fail(path, "cannot open");
    Gif_Stream* gfs = Gif_FullReadFile(f, GIF_READ_UNCOMPRESSED, nullptr, nullptr);
    std::fclose(f);
    if (!gfs) return fail(path, "gifsicle rejected the file");

    if (static_cast<int>(ours.stream.frames.size()) != gfs->nimages) {
        const std::string why = "frame count " + std::to_string(ours.stream.frames.size()) +
                                " vs gifsicle " + std::to_string(gfs->nimages);
        Gif_DeleteStream(gfs);
        return fail(path, why);
    }
    if (ours.stream.screen_width != gfs->screen_width ||
        ours.stream.screen_height != gfs->screen_height) {
        Gif_DeleteStream(gfs);
        return fail(path, "screen size");
    }

    for (int i = 0; i < gfs->nimages; ++i) {
        const auto& a = ours.stream.frames[static_cast<std::size_t>(i)];
        Gif_Image* b = gfs->images[i];
        const std::string at = "frame " + std::to_string(i) + ": ";
        if (a.width != b->width || a.height != b->height || a.left != b->left || a.top != b->top) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "geometry");
        }
        if (a.delay != b->delay) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "delay");
        }
        if (static_cast<uint8_t>(a.disposal) != b->disposal) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "disposal");
        }
        if (a.transparent != b->transparent) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "transparent index");
        }
        const std::size_t local_size = a.local ? a.local->size() : 0;
        const std::size_t gs_local = b->local ? static_cast<std::size_t>(b->local->ncol) : 0;
        if (local_size != gs_local) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "local colormap size");
        }
        Gif_UncompressImage(gfs, b);
        if (a.pixels.size() != static_cast<std::size_t>(a.width) * a.height) {
            Gif_DeleteStream(gfs);
            return fail(path, at + "pixel buffer size");
        }
        for (int y = 0; y < b->height; ++y) {
            if (std::memcmp(a.pixels.data() + static_cast<std::size_t>(y) * a.width, b->img[y],
                            b->width) != 0) {
                Gif_DeleteStream(gfs);
                return fail(path, at + "pixels differ on row " + std::to_string(y));
            }
        }
    }

    Gif_DeleteStream(gfs);
    std::printf("OK %s %zu frames\n", path, ours.stream.frames.size());
    return 0;
}

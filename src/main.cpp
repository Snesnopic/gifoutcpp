#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gif_encoder.hpp"
#include "gif_optimizer.hpp"
#include "gif_reader.hpp"
#include "gif_writer.hpp"

#ifndef GIFOUTCPP_VERSION
#define GIFOUTCPP_VERSION "0.0.0"
#endif

namespace {

void usage() {
    std::printf(
        "gifoutcpp %s - lossless gif recompression\n"
        "\n"
        "usage: gifoutcpp [options] <input.gif> [output.gif]\n"
        "\n"
        "  -O, --optimize    rebuild frames and palettes, keeping only the rendered result\n"
        "      --deinterlace drop interlacing, which costs size and only helps slow links\n"
        "  -i, --info        report structure and diagnostics, write nothing\n"
        "  -c, --copy        copy the lzw data instead of re-encoding it\n"
        "      --careful     take the code size from the palette, not from the pixels\n"
        "      --eager-clear restart the dictionary as soon as it fills\n"
        "      --both-clears try both restart policies and keep the smaller\n"
        "      --reblock     re-chunk copied lzw data at 255 bytes\n"
        "  -q, --quiet       only report errors\n"
        "  -h, --help        this message\n"
        "  -v, --version     version number\n",
        GIFOUTCPP_VERSION);
}

const char* severity_name(gifout::Severity s) {
    switch (s) {
        case gifout::Severity::Error: return "error";
        case gifout::Severity::Warning: return "warning";
        default: return "info";
    }
}

void print_diagnostics(const gifout::ReadResult& result, const std::string& name) {
    for (const auto& d : result.diagnostics)
        std::fprintf(stderr, "%s: %s at offset %zu: %s\n", name.c_str(), severity_name(d.severity),
                     d.offset, d.message.c_str());
}

void print_info(const gifout::Stream& s) {
    std::printf("screen %ux%u, version %s, %zu frames, loop %ld\n", s.screen_width, s.screen_height,
                s.version.c_str(), s.frames.size(), s.loopcount);
    std::printf("global colormap: %zu entries%s\n", s.global ? s.global->size() : 0,
                s.global ? "" : " (none)");
    std::size_t index = 0;
    for (const auto& f : s.frames) {
        std::printf("  frame %3zu %5ux%-5u at %5u,%-5u delay %4u disposal %u%s%s local %zu lzw %zu\n",
                    index++, f.width, f.height, f.left, f.top, f.delay,
                    static_cast<unsigned>(f.disposal), f.interlaced ? " interlaced" : "",
                    f.transparent >= 0 ? " transparent" : "", f.local ? f.local->size() : 0,
                    f.lzw.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool info = false, quiet = false, copy = false, optimize = false;
    gifout::OptimizeOptions optimize_options;
    gifout::WriteOptions write_options;
    gifout::EncodeOptions encode_options;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::printf("%s\n", GIFOUTCPP_VERSION);
            return 0;
        } else if (arg == "-i" || arg == "--info") {
            info = true;
        } else if (arg == "-O" || arg == "--optimize") {
            optimize = true;
        } else if (arg == "--deinterlace") {
            optimize = true;
            optimize_options.deinterlace = true;
        } else if (arg == "-c" || arg == "--copy") {
            copy = true;
        } else if (arg == "--careful") {
            encode_options.careful_min_code_size = true;
        } else if (arg == "--eager-clear") {
            encode_options.eager_clear = true;
        } else if (arg == "--both-clears") {
            encode_options.try_both_clear_policies = true;
        } else if (arg == "--reblock") {
            write_options.reblock_lzw = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        usage();
        return 2;
    }

    const std::string& input = positional[0];
    gifout::ReadOptions read_options;
    read_options.decode_pixels = info || !copy || optimize;
    auto result = gifout::read_gif_file(input, read_options);
    if (!quiet || result.has_errors()) print_diagnostics(result, input);
    if (!result.ok) {
        std::fprintf(stderr, "%s: unreadable\n", input.c_str());
        return 1;
    }

    if (info) {
        print_info(result.stream);
        return 0;
    }
    if (positional.size() < 2) {
        std::fprintf(stderr, "no output file given\n");
        return 2;
    }

    // restructuring can lose on some files, so the plain recompression is kept as a
    // floor: the tool must never hand back something larger than it could have
    std::vector<uint8_t> fallback;
    if (optimize) {
        gifout::Stream plain = result.stream;
        for (auto& frame : plain.frames)
            if (frame.pixels_complete)
                gifout::encode_frame(frame, gifout::effective_palette_size(frame, plain),
                                     encode_options);
        fallback = gifout::write_gif(plain, write_options);

        const auto stats = gifout::optimize(result.stream, optimize_options);
        if (!quiet) {
            if (!stats.skipped.empty())
                std::fprintf(stderr, "%s: not optimized: %s\n", input.c_str(),
                             stats.skipped.c_str());
            else
                std::fprintf(stderr,
                             "%s: %zu frames (%zu dropped), %zu -> %zu pixels, %zu -> %zu colors\n",
                             input.c_str(), result.stream.frames.size(), stats.frames_dropped,
                             stats.pixels_before, stats.pixels_after, stats.colors_before,
                             stats.colors_after);
        }
        copy = false;
    }

    if (!copy) {
        for (auto& frame : result.stream.frames) {
            // a frame the decoder had to repair no longer matches its own lzw data, so
            // re-encoding it would change the image rather than just its encoding
            if (!frame.pixels_complete) {
                if (!quiet)
                    std::fprintf(stderr, "%s: frame with a damaged lzw stream copied verbatim\n",
                                 input.c_str());
                continue;
            }
            gifout::encode_frame(frame, gifout::effective_palette_size(frame, result.stream),
                                 encode_options);
        }
    }

    auto bytes = gifout::write_gif(result.stream, write_options);
    if (!fallback.empty() && fallback.size() < bytes.size()) {
        if (!quiet)
            std::fprintf(stderr, "%s: restructuring lost %zu bytes, keeping the plain encode\n",
                         input.c_str(), bytes.size() - fallback.size());
        bytes = std::move(fallback);
    }
    std::FILE* out = std::fopen(positional[1].c_str(), "wb");
    if (!out || std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size() ||
        std::fclose(out) != 0) {
        std::fprintf(stderr, "%s: cannot write\n", positional[1].c_str());
        return 1;
    }
    return 0;
}

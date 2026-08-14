#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
        "  -i, --info      report structure and diagnostics, write nothing\n"
        "      --reblock   re-chunk lzw data at 255 bytes instead of keeping the source split\n"
        "  -q, --quiet     only report errors\n"
        "  -h, --help      this message\n"
        "  -v, --version   version number\n",
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
    bool info = false, quiet = false;
    gifout::WriteOptions write_options;
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
    read_options.decode_pixels = info;
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
    if (!gifout::write_gif_file(result.stream, positional[1], write_options)) {
        std::fprintf(stderr, "%s: cannot write\n", positional[1].c_str());
        return 1;
    }
    return 0;
}

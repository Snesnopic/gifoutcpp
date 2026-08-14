#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "gifoutcpp/gifoutcpp.hpp"

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
        "  -u, --unoptimize  expand every frame back to full screen, disposal applied\n"
        "      --level L     l1 keeps the structure and only re-encodes, l2 (default) may\n"
        "                    rebuild it as long as the animation plays the same\n"
        "  -s, --search      search for the best dictionary restart points, much slower\n"
        "      --alignment N how far apart restart points may sit, in pixels (default 160)\n"
        "      --max-tokens N how far a block is explored (default 10000)\n"
        "  -j, --threads N   threads for the search, 0 means as many as the machine has\n"
        "                    (default 1; the output is identical whatever you pick)\n"
        "      --deinterlace drop interlacing, which costs size and only helps slow links\n"
        "      --strip       drop comments and application metadata, keep the loop block\n"
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

void print_diagnostics(const std::vector<gifout::Diagnostic>& diagnostics,
                       const std::string& name) {
    for (const auto& d : diagnostics)
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
    gifout::Options options;
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
            options.restructure = true;
        } else if (arg == "--deinterlace") {
            options.deinterlace = true;
        } else if (arg == "-u" || arg == "--unoptimize") {
            options.unoptimize = true;
        } else if (arg == "--strip") {
            options.strip_metadata = true;
        } else if (arg == "--level" && i + 1 < argc) {
            const std::string level = argv[++i];
            if (level == "l1" || level == "L1" || level == "structure") {
                options.level = gifout::Lossless::Structure;
            } else if (level == "l2" || level == "L2" || level == "rendering") {
                options.level = gifout::Lossless::Rendering;
            } else {
                std::fprintf(stderr, "unknown level %s, expected l1 or l2\n", level.c_str());
                return 2;
            }
        } else if (arg == "-s" || arg == "--search") {
            options.search_restarts = true;
        } else if ((arg == "-j" || arg == "--threads") && i + 1 < argc) {
            options.search.threads = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (arg == "--alignment" && i + 1 < argc) {
            options.search.alignment = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (arg == "--max-tokens" && i + 1 < argc) {
            options.search.max_tokens = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (arg == "-c" || arg == "--copy") {
            options.copy_lzw = true;
        } else if (arg == "--careful") {
            options.encode.careful_min_code_size = true;
        } else if (arg == "--eager-clear") {
            options.encode.eager_clear = true;
        } else if (arg == "--both-clears") {
            options.encode.try_both_clear_policies = true;
        } else if (arg == "--reblock") {
            options.reblock_lzw = true;
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

    if (info) {
        gifout::ReadOptions read_options;
        auto read = gifout::read_gif_file(input);
        if (!quiet || read.has_errors()) print_diagnostics(read.diagnostics, input);
        if (!read.ok) {
            std::fprintf(stderr, "%s: unreadable\n", input.c_str());
            return 1;
        }
        print_info(read.stream);
        return 0;
    }

    if (positional.size() < 2) {
        std::fprintf(stderr, "no output file given\n");
        return 2;
    }

    const auto result = gifout::recompress_file(input, positional[1], options);
    const bool has_error = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                                       [](const gifout::Diagnostic& d) {
                                           return d.severity == gifout::Severity::Error;
                                       });
    if (!quiet || has_error) print_diagnostics(result.diagnostics, input);
    if (!result.ok) {
        std::fprintf(stderr, "%s: failed\n", input.c_str());
        return 1;
    }
    if (!quiet) {
        std::fprintf(stderr, "%s: %zu -> %zu bytes, %zu -> %zu frames%s%s\n", input.c_str(),
                     result.input_bytes, result.output_bytes, result.frames_in, result.frames_out,
                     result.metadata_removed ? ", metadata dropped" : "",
                     result.restructure_note.empty() ? "" : (", " + result.restructure_note).c_str());
    }
    return 0;
}

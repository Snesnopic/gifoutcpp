#include "gifoutcpp/gifoutcpp.hpp"

#include <cstdio>

namespace gifout {
namespace {

void encode_frames(Stream& stream, const Options& options) {
    for (auto& frame : stream.frames) {
        // a frame the decoder had to repair no longer matches its own lzw data, so
        // re-encoding it would change the image rather than just its encoding
        if (!frame.pixels_complete) continue;
        const unsigned palette = effective_palette_size(frame, stream);
        if (!options.search_restarts) {
            encode_frame(frame, palette, options.encode);
            continue;
        }
        auto found = encode_lzw_search(frame.pixels, frame.width, frame.height, frame.interlaced,
                                       palette, options.encode, options.search);
        auto greedy = encode_lzw(frame.pixels, frame.width, frame.height, frame.interlaced, palette,
                                 options.encode);
        frame.lzw_min_code_size = greedy.min_code_size;
        frame.block_sizes.clear();
        // the search should win, but a heuristic occasionally lands better
        frame.lzw = (found.searched && found.encoded.lzw.size() < greedy.lzw.size())
                        ? std::move(found.encoded.lzw)
                        : std::move(greedy.lzw);
    }
}

}  // namespace

Result recompress_stream(Stream& stream, std::vector<uint8_t>& output, const Options& options) {
    Result result;
    result.frames_in = stream.frames.size();

    const bool restructure = options.restructure || options.deinterlace;
    if (options.level == Lossless::Structure && (restructure || options.unoptimize)) {
        result.restructure_note = "level l1 cannot restructure";
        result.frames_out = stream.frames.size();
        output.clear();
        return result;
    }

    if (options.strip_metadata) result.metadata_removed = strip_metadata(stream);
    if (options.unoptimize) {
        const auto stats = unoptimize(stream);
        if (!stats.skipped.empty()) result.restructure_note = stats.skipped;
    }

    // restructuring can lose on some inputs, so the plain encode is kept as a floor
    std::vector<uint8_t> fallback;
    if (restructure) {
        Stream plain = stream;
        if (!options.copy_lzw) encode_frames(plain, options);
        WriteOptions plain_write;
        plain_write.reblock_lzw = options.reblock_lzw;
        fallback = write_gif(plain, plain_write);

        OptimizeOptions optimize_options;
        optimize_options.level = options.level;
        optimize_options.deinterlace = options.deinterlace;
        const auto stats = optimize(stream, optimize_options);
        if (!stats.skipped.empty())
            result.restructure_note = stats.skipped;
        else
            result.restructured = true;
    }

    if (!options.copy_lzw || restructure) encode_frames(stream, options);

    WriteOptions write_options;
    write_options.reblock_lzw = options.reblock_lzw;
    auto bytes = write_gif(stream, write_options);
    if (!fallback.empty() && fallback.size() < bytes.size()) {
        bytes = std::move(fallback);
        result.restructured = false;
        result.restructure_note = "restructuring produced a larger file";
    }

    output = std::move(bytes);
    result.frames_out = stream.frames.size();
    result.output_bytes = output.size();
    result.ok = true;
    return result;
}

Result recompress(std::span<const uint8_t> input, std::vector<uint8_t>& output,
                  const Options& options) {
    ReadOptions read_options;
    read_options.decode_pixels = !options.copy_lzw || options.restructure ||
                                 options.deinterlace || options.unoptimize;
    auto read = read_gif(input, read_options);
    Result result;
    result.diagnostics = std::move(read.diagnostics);
    result.input_bytes = input.size();
    if (!read.ok) return result;

    auto pipeline = recompress_stream(read.stream, output, options);
    pipeline.diagnostics = std::move(result.diagnostics);
    pipeline.input_bytes = input.size();
    return pipeline;
}

Result recompress_file(const std::filesystem::path& input, const std::filesystem::path& output,
                       const Options& options) {
    Result result;
    std::FILE* in = std::fopen(input.string().c_str(), "rb");
    if (!in) {
        result.diagnostics.push_back({Severity::Error, 0, "cannot open " + input.string()});
        return result;
    }
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<std::size_t>(size) : 0);
    const std::size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), in);
    std::fclose(in);
    data.resize(got);

    std::vector<uint8_t> bytes;
    result = recompress(data, bytes, options);
    if (!result.ok) return result;

    std::FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        result.ok = false;
        result.diagnostics.push_back({Severity::Error, 0, "cannot write " + output.string()});
        return result;
    }
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), out);
    if (std::fclose(out) != 0 || written != bytes.size()) {
        result.ok = false;
        result.diagnostics.push_back({Severity::Error, 0, "short write to " + output.string()});
    }
    return result;
}

std::string version() {
    return std::to_string(GIFOUTCPP_VERSION_MAJOR) + "." + std::to_string(GIFOUTCPP_VERSION_MINOR) +
           "." + std::to_string(GIFOUTCPP_VERSION_PATCH);
}

}  // namespace gifout

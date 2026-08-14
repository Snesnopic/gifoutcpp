#include "gifoutcpp/gif_optimizer.hpp"

#include <algorithm>
#include <unordered_map>

#include "gifoutcpp/gif_encoder.hpp"

namespace gifout {
namespace {

// every frame is lifted into one shared color space before anything else, because
// frames with different local palettes are otherwise not comparable pixel by pixel
constexpr uint16_t kTransparent = 0;

struct ColorSpace {
    std::vector<Color> colors{Color{}};  // slot 0 stands for transparent
    std::unordered_map<uint32_t, uint16_t> lookup;

    uint16_t intern(Color c) {
        const uint32_t key = (static_cast<uint32_t>(c.r) << 16) |
                             (static_cast<uint32_t>(c.g) << 8) | static_cast<uint32_t>(c.b);
        auto it = lookup.find(key);
        if (it != lookup.end()) return it->second;
        const auto id = static_cast<uint16_t>(colors.size());
        colors.push_back(c);
        lookup.emplace(key, id);
        return id;
    }
};

struct Rect {
    unsigned left = 0, top = 0, width = 0, height = 0;

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
};

Rect bounding_box(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned width,
                  unsigned height) {
    unsigned min_x = width;
    unsigned min_y = height;
    unsigned max_x = 0;
    unsigned max_y = 0;
    for (unsigned y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            if (a[row + x] == b[row + x]) continue;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (min_x > max_x) return {};
    return {.left = min_x, .top = min_y,
            .width = max_x - min_x + 1, .height = max_y - min_y + 1};
}

Rect union_of(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const unsigned left = std::min(a.left, b.left);
    const unsigned top = std::min(a.top, b.top);
    const unsigned right = std::max(a.left + a.width, b.left + b.width);
    const unsigned bottom = std::max(a.top + a.height, b.top + b.height);
    return {.left = left, .top = top, .width = right - left, .height = bottom - top};
}

class Optimizer {
public:
    Optimizer(Stream& stream, const OptimizeOptions& options) : stream_(stream), options_(options) {}

    OptimizeStats run() {
        stats_.frames_before = stream_.frames.size();
        for (const auto& f : stream_.frames) stats_.pixels_before += f.pixel_count();

        if (const char* why = refuse()) {
            stats_.skipped = why;
            return stats_;
        }

        width_ = stream_.screen_width;
        height_ = stream_.screen_height;
        lift_frames();
        choose_disposals();
        build_frames();
        // the lifted originals are only needed while the canvas is being walked, and
        // holding them alongside the variants is what makes the peak so high
        lifted_.clear();
        lifted_.shrink_to_fit();
        if (options_.drop_redundant_frames) drop_redundant();
        plain_index_.resize(stream_.frames.size());
        transparent_index_.resize(stream_.frames.size());
        greedy_index_.resize(stream_.frames.size());
        lone_index_.resize(stream_.frames.size());
        assign_palettes();
        if (stats_.skipped.empty()) choose_variants();
        if (stats_.skipped.empty()) keep_background_transparent();

        for (const auto& f : stream_.frames) stats_.pixels_after += f.pixel_count();
        return stats_;
    }

private:
    const char* refuse() {
        if (options_.level == Lossless::Structure) return "structure level forbids restructuring";
        if (stream_.frames.empty()) return "no frames";
        if (stream_.screen_width == 0 || stream_.screen_height == 0) return "empty screen";
        // the canvas is walked twice and held whole, so an absurd screen is refused
        if (static_cast<std::size_t>(stream_.screen_width) * stream_.screen_height >
            (std::size_t{1} << 28))
            return "screen too large to re-render";
        for (const auto& f : stream_.frames) {
            if (!f.pixels_complete) return "a frame's lzw stream had to be repaired";
            if (f.pixels.size() != f.pixel_count()) return "a frame has no decoded pixels";
            if (static_cast<unsigned>(f.left) + f.width > stream_.screen_width ||
                static_cast<unsigned>(f.top) + f.height > stream_.screen_height)
                return "a frame reaches outside the screen";
        }
        return nullptr;
    }

    void lift_frames() {
        // a viewer only shows the background colour when the first frame declares no
        // transparency; otherwise uncovered area is transparent, and gifdiff agrees
        if (stream_.frames[0].transparent < 0 && stream_.global &&
            stream_.background < stream_.global->size())
            background_id_ = space_.intern(stream_.global->colors[stream_.background]);
        lifted_.resize(stream_.frames.size());
        for (std::size_t i = 0; i < stream_.frames.size(); ++i) {
            const Frame& f = stream_.frames[i];
            const Colormap* map = f.local ? &*f.local : (stream_.global ? &*stream_.global : nullptr);
            auto& out = lifted_[i];
            out.resize(f.pixels.size());
            std::vector<uint16_t> cache(256, 0xFFFF);
            for (std::size_t p = 0; p < f.pixels.size(); ++p) {
                const uint8_t index = f.pixels[p];
                if (f.transparent >= 0 && index == f.transparent) {
                    out[p] = kTransparent;
                    continue;
                }
                if (cache[index] == 0xFFFF) {
                    const Color c = map && index < map->colors.size() ? map->colors[index] : Color{};
                    cache[index] = space_.intern(c);
                }
                out[p] = cache[index];
            }
        }
        stats_.colors_before = space_.colors.size() - 1;
    }

    // walks the original animation one frame at a time; two passes over it cost less
    // than holding a full canvas per frame, which is gigabytes on long animations
    class TargetWalker {
    public:
        explicit TargetWalker(const Optimizer& opt) : opt_(opt), canvas_(opt.area(), kTransparent) {}

        // canvas state while frame i is on screen
        const std::vector<uint16_t>& advance(std::size_t i) {
            const Frame& f = opt_.stream_.frames[i];
            if (f.disposal == Disposal::Previous) saved_ = canvas_;
            opt_.paint(canvas_, opt_.lifted_[i], f, true);
            target_ = canvas_;
            if (f.disposal == Disposal::Background)
                opt_.erase(canvas_, f);
            else if (f.disposal == Disposal::Previous)
                canvas_ = saved_;
            return target_;
        }

    private:
        const Optimizer& opt_;
        std::vector<uint16_t> canvas_;
        std::vector<uint16_t> target_;
        std::vector<uint16_t> saved_;
    };

    [[nodiscard]] std::size_t area() const { return static_cast<std::size_t>(width_) * height_; }

    void paint(std::vector<uint16_t>& canvas, const std::vector<uint16_t>& src, const Frame& f,
               bool skip_transparent) const {
        for (unsigned y = 0; y < f.height; ++y) {
            const std::size_t dst_row = (static_cast<std::size_t>(f.top + y) * width_) + f.left;
            const std::size_t src_row = static_cast<std::size_t>(y) * f.width;
            for (unsigned x = 0; x < f.width; ++x) {
                const uint16_t v = src[src_row + x];
                if (skip_transparent && v == kTransparent) continue;
                canvas[dst_row + x] = v;
            }
        }
    }

    void erase(std::vector<uint16_t>& canvas, const Frame& f) const {
        for (unsigned y = 0; y < f.height; ++y) {
            const std::size_t row = (static_cast<std::size_t>(f.top + y) * width_) + f.left;
            std::fill(canvas.begin() + static_cast<std::ptrdiff_t>(row),
                      canvas.begin() + static_cast<std::ptrdiff_t>(row + f.width), background_id_);
        }
    }

    // a frame may only need clearing if the next one shows the background through where
    // this one painted; that is the single reason to keep background disposal
    void choose_disposals() {
        const std::size_t n = stream_.frames.size();
        disposal_.assign(n, Disposal::None);
        clear_rect_.assign(n, Rect{});
        // an opaque background can simply be painted, so nothing ever needs erasing;
        // only a transparent one forces a real clear
        if (background_id_ != kTransparent) return;
        TargetWalker walker(*this);
        std::vector<uint16_t> prev = walker.advance(0);
        for (std::size_t i = 1; i < n; ++i) {
            Rect needed;
            const std::vector<uint16_t> cur = walker.advance(i);
            unsigned min_x = width_;
            unsigned min_y = height_;
            unsigned max_x = 0;
            unsigned max_y = 0;
            for (unsigned y = 0; y < height_; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * width_;
                for (unsigned x = 0; x < width_; ++x) {
                    if (cur[row + x] != background_id_ || prev[row + x] == background_id_) continue;
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                }
            }
            if (min_x <= max_x)
                needed = {.left = min_x, .top = min_y,
                          .width = max_x - min_x + 1, .height = max_y - min_y + 1};
            if (!needed.empty()) {
                disposal_[i - 1] = Disposal::Background;
                clear_rect_[i - 1] = needed;
            }
            prev = std::move(cur);
        }
    }

    void build_frames() {
        std::vector<uint16_t> canvas(area(), kTransparent);
        TargetWalker walker(*this);
        std::vector<Frame> out;
        out.reserve(stream_.frames.size());
        new_pixels_.clear();
        new_pixels_.reserve(stream_.frames.size());
        frame_needs_transparency_.clear();
        paints_nothing_.clear();
        transparent_masks_.clear();
        transparent_masks_.reserve(stream_.frames.size());
        greedy_masks_.clear();
        greedy_masks_.reserve(stream_.frames.size());
        lone_masks_.clear();
        lone_masks_.reserve(stream_.frames.size());

        for (std::size_t i = 0; i < stream_.frames.size(); ++i) {
            Frame frame = stream_.frames[i];
            const std::vector<uint16_t> target = walker.advance(i);

            Rect rect = options_.crop_frames ? bounding_box(canvas, target, width_, height_)
                                             : Rect{.left = 0, .top = 0,
                                                    .width = width_, .height = height_};
            // the area this frame will erase has to be inside the frame itself
            if (disposal_[i] == Disposal::Background) rect = union_of(rect, clear_rect_[i]);
            if (rect.empty()) rect = {.left = 0, .top = 0, .width = 1, .height = 1};

            const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
            std::vector<uint16_t> pixels(count);
            std::vector<uint8_t> same(count, 0);
            bool wants_transparency = false;
            bool any_same = false;
            std::size_t same_count = 0;
            for (unsigned y = 0; y < rect.height; ++y) {
                const std::size_t src_row = (static_cast<std::size_t>(rect.top + y) * width_) + rect.left;
                const std::size_t dst_row = static_cast<std::size_t>(y) * rect.width;
                for (unsigned x = 0; x < rect.width; ++x) {
                    const uint16_t want = target[src_row + x];
                    pixels[dst_row + x] = want;
                    if (want == kTransparent) wants_transparency = true;
                    if (want == canvas[src_row + x]) {
                        same[dst_row + x] = 1;
                        any_same = true;
                        ++same_count;
                    }
                }
            }

            // gifsicle's rule, and the subtlety is the second condition: a matching run
            // only becomes transparent if its colour differs from what was just emitted,
            // otherwise replacing it would break a longer run of the same index.
            // a variant differs from the base image only by which pixels go transparent,
            // so it is a bitmask rather than a second copy of the frame
            std::vector<bool> transparent_mask;
            std::vector<bool> lone_mask;
            if (options_.use_transparency && any_same) {
                transparent_mask.assign(count, false);
                std::size_t begin_same = 0;
                unsigned nsame = 0;
                bool lone_seen = false;
                std::vector<std::pair<std::size_t, std::size_t>> lone_runs;
                for (std::size_t p = 0; p < count; ++p) {
                    if (!same[p] && pixels[p] != kTransparent) {
                        // a run of exactly one matching pixel: gifsicle only swallows it
                        // in the extra variant it tries at -O3
                        if (nsame == 1 && p > 0 && !transparent_mask[p - 1]) {
                            lone_runs.emplace_back(begin_same, p);
                            lone_seen = true;
                        }
                        nsame = 0;
                    } else if (nsame == 0) {
                        begin_same = p;
                        nsame = 1;
                    } else if (nsame == 1 && p > 0) {
                        // nsame is never 1 at p == 0, but reading pixels[p - 1] to find that
                        // out is a bounds violation waiting for a refactor to reach it
                        const uint16_t previous =
                            transparent_mask[p - 1] ? kTransparent : pixels[p - 1];
                        if (pixels[p] != previous) {
                            for (std::size_t q = begin_same; q < p; ++q) transparent_mask[q] = true;
                            nsame = 2;
                        }
                    }
                    if (nsame > 1) transparent_mask[p] = true;
                }
                if (std::find(transparent_mask.begin(), transparent_mask.end(), true) ==
                    transparent_mask.end())
                    transparent_mask.clear();
                if (lone_seen) {
                    lone_mask = transparent_mask;
                    if (lone_mask.empty()) lone_mask.assign(count, false);
                    for (const auto& [from, to] : lone_runs)
                        for (std::size_t q = from; q < to; ++q) lone_mask[q] = true;
                    if (lone_mask == transparent_mask) lone_mask.clear();
                }
            }

            // the blunt variant: every matching run of two or more, whatever the colour
            std::vector<bool> greedy_mask;
            if (options_.use_transparency && any_same) {
                greedy_mask.assign(count, false);
                std::size_t run_start = 0;
                std::size_t run_length = 0;
                for (std::size_t p = 0; p <= count; ++p) {
                    if (p < count && same[p]) {
                        if (run_length == 0) run_start = p;
                        ++run_length;
                        continue;
                    }
                    if (run_length >= 2)
                        for (std::size_t q = run_start; q < run_start + run_length; ++q)
                            greedy_mask[q] = true;
                    run_length = 0;
                }
                if (std::find(greedy_mask.begin(), greedy_mask.end(), true) == greedy_mask.end() ||
                    greedy_mask == transparent_mask)
                    greedy_mask.clear();
            }

            frame.left = static_cast<uint16_t>(rect.left);
            frame.top = static_cast<uint16_t>(rect.top);
            frame.width = static_cast<uint16_t>(rect.width);
            frame.height = static_cast<uint16_t>(rect.height);
            frame.disposal = disposal_[i];
            frame.has_gce = frame.has_gce || frame.disposal != Disposal::None || wants_transparency ||
                            frame.delay != 0;
            frame.interlaced = options_.deinterlace ? false : frame.interlaced;
            frame.lzw.clear();
            frame.block_sizes.clear();
            frame.local.reset();
            frame.transparent =
                (wants_transparency || !transparent_mask.empty() || !greedy_mask.empty() ||
                 !lone_mask.empty())
                    ? 0
                    : -1;  // index assigned later
            frame_needs_transparency_.push_back(wants_transparency ? 1 : 0);
            paints_nothing_.push_back(same_count == count ? 1 : 0);
            frame.raw_transparent_index = 0;
            frame.raw_local_size_bits = 0;

            canvas = target;
            if (disposal_[i] == Disposal::Background) {
                Frame erased = frame;
                erase(canvas, erased);
            }

            out.push_back(std::move(frame));
            new_pixels_.push_back(std::move(pixels));
            transparent_masks_.push_back(std::move(transparent_mask));
            greedy_masks_.push_back(std::move(greedy_mask));
            lone_masks_.push_back(std::move(lone_mask));
        }
        stream_.frames = std::move(out);
    }

    // a frame that paints nothing new only exists to hold a delay, which the frame
    // before it can carry instead
    void drop_redundant() {
        for (std::size_t i = stream_.frames.size(); i-- > 1;) {
            const Frame& f = stream_.frames[i];
            if (f.disposal != Disposal::None || stream_.frames[i - 1].disposal != Disposal::None)
                continue;
            if (!paints_nothing_[i]) continue;
            const unsigned merged = stream_.frames[i - 1].delay + f.delay;
            if (merged > 0xFFFF) continue;
            stream_.frames[i - 1].delay = static_cast<uint16_t>(merged);
            stream_.frames[i - 1].has_gce = true;
            stream_.frames.erase(stream_.frames.begin() + static_cast<std::ptrdiff_t>(i));
            new_pixels_.erase(new_pixels_.begin() + static_cast<std::ptrdiff_t>(i));
            transparent_masks_.erase(transparent_masks_.begin() + static_cast<std::ptrdiff_t>(i));
            greedy_masks_.erase(greedy_masks_.begin() + static_cast<std::ptrdiff_t>(i));
            lone_masks_.erase(lone_masks_.begin() + static_cast<std::ptrdiff_t>(i));
            frame_needs_transparency_.erase(frame_needs_transparency_.begin() +
                                            static_cast<std::ptrdiff_t>(i));
            paints_nothing_.erase(paints_nothing_.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats_.frames_dropped;
        }
    }

    void assign_palettes() {
        std::vector<uint16_t> used;
        used.reserve(space_.colors.size());
        std::vector<uint8_t> seen(space_.colors.size(), 0);
        // the code size a frame pays follows its highest index, so the frames with the
        // fewest colours are served first: they are the ones a low index can rescue
        // what a frame is likely to end up painting, which is the transparent variant
        // when it has one: ordering on the full image would rank by colours the frame
        // is about to throw away
        std::vector<std::vector<uint16_t>> palette_of(new_pixels_.size());
        std::vector<std::vector<uint16_t>> rest_of(new_pixels_.size());
        bool any_transparent = false;
        for (std::size_t i = 0; i < new_pixels_.size(); ++i) {
            if (stream_.frames[i].transparent >= 0) any_transparent = true;
            const auto& mask = transparent_masks_[i];
            std::vector<uint8_t> here(space_.colors.size(), 0);
            for (std::size_t p = 0; p < new_pixels_[i].size(); ++p) {
                const uint16_t v = new_pixels_[i][p];
                if (v == kTransparent || here[v]) continue;
                here[v] = 1;
                if (!mask.empty() && mask[p])
                    rest_of[i].push_back(v);
                else
                    palette_of[i].push_back(v);
            }
        }
        std::vector<std::size_t> order(new_pixels_.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return palette_of[a].size() < palette_of[b].size();
        });
        for (const std::size_t i : order)
            for (const uint16_t v : palette_of[i])
                if (!seen[v]) {
                    seen[v] = 1;
                    used.push_back(v);
                }
        // whatever only the full variants need goes after, since a frame that keeps them
        // is paying for the whole picture anyway
        for (const std::size_t i : order)
            for (const uint16_t v : rest_of[i])
                if (!seen[v]) {
                    seen[v] = 1;
                    used.push_back(v);
                }
        if (background_id_ != kTransparent && !seen[background_id_]) {
            seen[background_id_] = 1;
            used.push_back(background_id_);
        }
        // the transparent index does not need a slot of its own: it only has to be an
        // index the frame never paints with, and a frame rarely uses the whole table.
        // reserving one instead costs a palette that overflows by exactly one entry.
        stats_.colors_after = used.size();
        stats_.transparency_used = any_transparent;

        if (used.size() <= 256 && build_shared_palette(used)) return;
        build_local_palettes();
    }

    bool build_shared_palette(const std::vector<uint16_t>& used) {
        Colormap global;
        std::vector<uint8_t> to_index(space_.colors.size(), 0);
        for (uint16_t id : used) {
            to_index[id] = static_cast<uint8_t>(global.colors.size());
            global.colors.push_back(space_.colors[id]);
        }
        // one spare slot serves every frame that needs it: no pixel ever maps to it, so
        // it is free in all of them at once. growing the table per frame instead is what
        // turned a 32 colour animation into a 256 entry palette, code size and all
        int spare = -1;
        std::vector<int> transparent_index(new_pixels_.size(), -1);
        for (std::size_t i = 0; i < new_pixels_.size(); ++i) {
            if (stream_.frames[i].transparent < 0) continue;
            std::vector<uint8_t> taken(256, 0);
            for (uint16_t v : new_pixels_[i])
                if (v != kTransparent) taken[to_index[v]] = 1;
            int free_index = -1;
            for (std::size_t slot = 0; slot < global.colors.size() && slot < 256; ++slot)
                if (!taken[slot]) {
                    free_index = static_cast<int>(slot);
                    break;
                }
            if (free_index < 0 && spare >= 0) free_index = spare;
            if (free_index < 0 && global.colors.size() < 256) {
                spare = static_cast<int>(global.colors.size());
                free_index = spare;
                global.colors.push_back(Color{});
            }
            if (free_index < 0) {
                // this frame paints with every index there is and still needs to show
                // something through: only its own palette can help
                if (frame_needs_transparency_[i]) return false;
                stream_.frames[i].transparent = -1;
                transparent_masks_[i].clear();
                greedy_masks_[i].clear();
                continue;
            }
            transparent_index[i] = free_index;
        }

        stream_.global = std::move(global);
        stream_.background =
            background_id_ == kTransparent ? 0 : to_index[background_id_];

        for (std::size_t i = 0; i < new_pixels_.size(); ++i) {
            stream_.frames[i].local.reset();
            const auto slot = static_cast<uint8_t>(std::max(0, transparent_index[i]));
            map_variants(i, slot, [&](uint16_t v) { return to_index[v]; });
            if (transparent_index[i] >= 0) stream_.frames[i].transparent = transparent_index[i];
        }
        return true;
    }

    void build_local_palettes() {
        stats_.local_colormaps = true;
        // the background index only resolves against a global map, so keep a minimal one
        Colormap global;
        global.colors.push_back(background_id_ == kTransparent ? Color{}
                                                              : space_.colors[background_id_]);
        stream_.global = std::move(global);
        stream_.background = 0;
        for (std::size_t i = 0; i < new_pixels_.size(); ++i) {
            Frame& f = stream_.frames[i];
            std::unordered_map<uint16_t, uint8_t> index_of;
            Colormap map;
            if (f.transparent >= 0) map.colors.push_back(Color{});
            bool overflow = false;
            map_variants(i, 0, [&](uint16_t v) -> uint8_t {
                auto it = index_of.find(v);
                if (it == index_of.end()) {
                    if (map.colors.size() >= 256) {
                        overflow = true;
                        return 0;
                    }
                    const auto slot = static_cast<uint8_t>(map.colors.size());
                    map.colors.push_back(space_.colors[v]);
                    it = index_of.emplace(v, slot).first;
                }
                return it->second;
            });
            if (overflow) {
                stats_.skipped = "a frame needed more than 256 colors";
                return;
            }
            f.local = std::move(map);
        }
    }

    template <typename Map>
    void map_variants(std::size_t i, uint8_t transparent_slot, Map to_index) {
        // each variant is freed as soon as it has been mapped to palette indices, so
        // the wide and the narrow copy of the same frame never both sit in memory
        const auto& base = new_pixels_[i];
        auto apply = [&](const std::vector<bool>& mask, std::vector<uint8_t>& dst) {
            dst.resize(base.size());
            for (std::size_t p = 0; p < base.size(); ++p)
                dst[p] = (base[p] == kTransparent || (!mask.empty() && mask[p]))
                             ? transparent_slot
                             : to_index(base[p]);
        };
        apply({}, plain_index_[i]);
        if (!transparent_masks_[i].empty()) apply(transparent_masks_[i], transparent_index_[i]);
        if (!greedy_masks_[i].empty()) apply(greedy_masks_[i], greedy_index_[i]);
        if (!lone_masks_[i].empty()) apply(lone_masks_[i], lone_index_[i]);
        // the base image is not needed once every variant has been mapped
        new_pixels_[i].clear();
        new_pixels_[i].shrink_to_fit();
    }

    // the encoder is the only honest judge of whether transparency paid off, which is
    // why the greedy one had to exist before this optimizer
    void choose_variants() {
        for (std::size_t i = 0; i < stream_.frames.size(); ++i) {
            Frame& f = stream_.frames[i];
            const unsigned palette = effective_palette_size(f, stream_);
            if (transparent_index_[i].empty() && greedy_index_[i].empty() &&
                lone_index_[i].empty()) {
                f.pixels = std::move(plain_index_[i]);
                plain_index_[i].shrink_to_fit();
                f.pixels_complete = true;
                continue;
            }
            std::vector<uint8_t>* best = &plain_index_[i];
            std::size_t best_size =
                encode_lzw(plain_index_[i], f.width, f.height, f.interlaced, palette).lzw.size();
            bool transparent_won = false;
            for (std::vector<uint8_t>* candidate :
                 {&transparent_index_[i], &greedy_index_[i], &lone_index_[i]}) {
                if (candidate->empty()) continue;
                const std::size_t size =
                    encode_lzw(*candidate, f.width, f.height, f.interlaced, palette).lzw.size();
                if (size < best_size) {
                    best_size = size;
                    best = candidate;
                    transparent_won = true;
                }
            }
            f.pixels = std::move(*best);
            for (std::vector<uint8_t>* other :
                 {&plain_index_[i], &transparent_index_[i], &greedy_index_[i], &lone_index_[i]}) {
                other->clear();
                other->shrink_to_fit();
            }
            if (transparent_won) {
                stats_.transparency_used = true;
            } else if (!frame_needs_transparency_[i]) {
                f.transparent = -1;
            }
            f.pixels_complete = true;
        }
    }

    // a viewer only reads the background as transparent when the first frame declares a
    // transparent index; without that declaration, area cropped away comes back as a
    // colour. any index the frame never uses can carry the declaration, so this costs
    // nothing rather than a whole palette slot
    void keep_background_transparent() {
        if (background_id_ != kTransparent || stream_.frames.empty()) return;
        Frame& first = stream_.frames[0];
        if (first.transparent >= 0) return;
        const unsigned palette = effective_palette_size(first, stream_);
        std::vector<uint8_t> used(256, 0);
        for (uint8_t p : first.pixels) used[p] = 1;
        for (unsigned i = 0; i < palette && i < 256; ++i) {
            if (used[i]) continue;
            first.transparent = static_cast<int>(i);
            first.has_gce = true;
            return;
        }
        // every index is taken, so buy one; at worst this rounds the table up to the next
        // power of two, which the size fallback in the caller will reject if it does not pay
        Colormap* map = first.local ? &*first.local : (stream_.global ? &*stream_.global : nullptr);
        if (!map || map->colors.size() >= 256) return;
        first.transparent = static_cast<int>(map->colors.size());
        first.has_gce = true;
        map->colors.push_back(Color{});
    }

    Stream& stream_;
    OptimizeOptions options_;
    OptimizeStats stats_;
    ColorSpace space_;
    unsigned width_ = 0, height_ = 0;
    uint16_t background_id_ = kTransparent;
    std::vector<std::vector<uint16_t>> lifted_;
    std::vector<std::vector<uint16_t>> new_pixels_;
    std::vector<std::vector<bool>> transparent_masks_;
    std::vector<std::vector<uint8_t>> plain_index_;
    std::vector<std::vector<uint8_t>> transparent_index_;
    std::vector<std::vector<bool>> greedy_masks_;
    std::vector<std::vector<uint8_t>> greedy_index_;
    std::vector<std::vector<bool>> lone_masks_;
    std::vector<std::vector<uint8_t>> lone_index_;
    std::vector<uint8_t> frame_needs_transparency_;
    std::vector<uint8_t> paints_nothing_;
    std::vector<Disposal> disposal_;
    std::vector<Rect> clear_rect_;
};

}  // namespace

OptimizeStats optimize(Stream& stream, const OptimizeOptions& options) {
    return Optimizer(stream, options).run();
}

std::size_t strip_metadata(Stream& stream) {
    std::size_t removed = 0;
    const auto keep = [&removed](std::vector<Extension>& list) {
        std::vector<Extension> kept;
        for (auto& ext : list) {
            if (ext.label == 0xFF && ext.is_application("NETSCAPE2.0")) {
                kept.push_back(std::move(ext));
                continue;
            }
            if (ext.label != 0xFE && ext.label != 0x01 && ext.label != 0xFF) {
                kept.push_back(std::move(ext));
                continue;
            }
            removed += 3;  // introducer, label, terminator
            for (const auto& block : ext.blocks) removed += block.size() + 1;
        }
        list = std::move(kept);
    };
    for (auto& frame : stream.frames) {
        const std::size_t before = frame.extensions.size();
        keep(frame.extensions);
        if (frame.gce_position > frame.extensions.size())
            frame.gce_position = frame.extensions.size();
        (void)before;
    }
    keep(stream.trailing_extensions);
    return removed;
}

OptimizeStats unoptimize(Stream& stream) {
    OptimizeOptions options;
    options.crop_frames = false;
    options.use_transparency = false;
    options.drop_redundant_frames = false;
    return Optimizer(stream, options).run();
}

}  // namespace gifout

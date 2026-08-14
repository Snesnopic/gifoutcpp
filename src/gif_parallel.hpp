/**
 * @file gif_parallel.hpp
 * @brief Internal: splitting an index range over threads without changing the answer.
 */
#ifndef GIFOUTCPP_GIF_PARALLEL_HPP
#define GIFOUTCPP_GIF_PARALLEL_HPP

#include <cstddef>
#include <thread>
#include <vector>

namespace gifout {

/**
 * @brief Runs body(i, worker) for every i in [0, count), in contiguous blocks.
 *
 * Every index is handled by exactly one thread, and @c worker identifies which, so per
 * thread scratch space can be indexed by it without any locking. One thread is a plain
 * loop with no setup at all, and the result never depends on how the work was split.
 *
 * @param count   Number of indices to cover.
 * @param threads Upper bound on threads; 1 or 0 stays on the caller's thread.
 * @param body    Callable invoked as body(index, worker).
 */
template <typename Body>
void parallel_for(std::size_t count, unsigned threads, Body body) {
    if (count == 0) return;
    if (threads <= 1 || count == 1) {
        for (std::size_t i = 0; i < count; ++i) body(i, 0u);
        return;
    }
    const std::size_t workers = std::min<std::size_t>(threads, count);
    const std::size_t block = (count + workers - 1) / workers;
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t w = 1; w < workers; ++w) {
        const std::size_t from = w * block;
        if (from >= count) break;
        const std::size_t to = std::min(count, from + block);
        pool.emplace_back([&body, from, to, w] {
            for (std::size_t i = from; i < to; ++i) body(i, static_cast<unsigned>(w));
        });
    }
    for (std::size_t i = 0; i < std::min(count, block); ++i) body(i, 0u);
    for (auto& t : pool) t.join();
}

/**
 * @brief Turns a requested thread count into a usable one.
 * @param requested Zero means "as many as the machine reports".
 * @return At least one.
 */
inline unsigned resolve_threads(unsigned requested) {
    if (requested != 0) return requested;
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware ? hardware : 1;
}

}  // namespace gifout

#endif  // GIFOUTCPP_GIF_PARALLEL_HPP

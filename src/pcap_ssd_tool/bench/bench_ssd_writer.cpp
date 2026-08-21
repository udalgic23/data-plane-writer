// Benchmarks SsdWriterPool's write path in isolation (synthetic data, no
// pcap reading/hashing involved) across:
//   - core count    (number of writer threads)
//   - block size    (bytes per write)
//   - queue depth   (max outstanding/in-flight writes)
//   - access pattern (sequential vs random offsets)
//
// Reports IOPS, throughput, and submission-to-completion latency percentiles,
// matching the metrics/dimensions requested in issue #5.

#include "packet_task.hpp"
#include "ssd_writer_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static void usage(const char *prog) {
    std::cerr << "usage: " << prog
              << " <output_path> <core_count> <block_size_bytes> <io_count> <queue_depth> <seq|rand>\n"
              << "\n"
              << "  output_path        file to write to (will be created/truncated)\n"
              << "  core_count         number of writer threads (SsdWriterPool workers)\n"
              << "  block_size_bytes   size of each write, in bytes\n"
              << "  io_count           total number of write operations to issue\n"
              << "  queue_depth        max outstanding (in-flight, unacknowledged) writes\n"
              << "  seq|rand           offset pattern: sequential or a random permutation\n"
              << "                     of the same block-aligned offsets (each block\n"
              << "                     written exactly once either way)\n";
}

static double percentile(const std::vector<double> &sorted_ns, double p) {
    if (sorted_ns.empty())
        return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted_ns.size() - 1));
    return sorted_ns[idx];
}

int main(int argc, char **argv) {
    if (argc != 7) {
        usage(argv[0]);
        return 1;
    }

    const std::string path = argv[1];
    const size_t core_count = std::stoul(argv[2]);
    const size_t block_size = std::stoul(argv[3]);
    const size_t io_count = std::stoul(argv[4]);
    const int64_t queue_depth = std::stoll(argv[5]);
    const std::string pattern = argv[6];

    if (core_count == 0 || block_size == 0 || io_count == 0 || queue_depth <= 0) {
        std::cerr << "core_count, block_size_bytes, io_count, and queue_depth must all be positive\n";
        return 1;
    }
    if (pattern != "seq" && pattern != "rand") {
        std::cerr << "pattern must be 'seq' or 'rand'\n";
        return 1;
    }

    try {
        // Build the offset list up front: every block-aligned offset in range,
        // written exactly once, either in order or shuffled. This keeps "random"
        // and "sequential" runs directly comparable (same total bytes, same set
        // of blocks) while varying only access order.
        std::vector<off_t> offsets(io_count);
        for (size_t i = 0; i < io_count; ++i) {
            offsets[i] = static_cast<off_t>(i) * static_cast<off_t>(block_size);
        }
        if (pattern == "rand") {
            std::mt19937_64 rng(std::random_device {}());
            std::shuffle(offsets.begin(), offsets.end(), rng);
        }

        // Content is irrelevant for an I/O benchmark; each dispatched task still
        // gets its own owned buffer (mirrors real PacketTask/dispatch() usage).
        std::vector<uint8_t> source_buf(block_size, 0xAB);

        std::vector<Clock::time_point> submit_ts(io_count);
        std::vector<double> latency_ns(io_count, 0.0);

        std::atomic<int64_t> in_flight {0};
        std::atomic<uint64_t> failures {0};

        auto on_complete = [&](uint64_t req_id, bool ok) {
            Clock::time_point now = Clock::now();
            latency_ns[req_id] =
                std::chrono::duration<double, std::nano>(now - submit_ts[req_id]).count();
            if (!ok) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
            in_flight.fetch_sub(1, std::memory_order_release);
        };

        SsdWriterPool pool(path, core_count, on_complete);

        const off_t total_size = static_cast<off_t>(block_size) * static_cast<off_t>(io_count);
        if (!pool.preallocate(total_size)) {
            std::cerr << "warning: preallocation failed; results may include "
                         "filesystem extent-allocation overhead\n";
        }

        std::cout << "core_count=" << core_count << " block_size=" << block_size
                  << "B io_count=" << io_count << " queue_depth=" << queue_depth
                  << " pattern=" << pattern << "\n";

        const Clock::time_point t_start = Clock::now();

        for (size_t i = 0; i < io_count; ++i) {
            // Bound outstanding writes to queue_depth before submitting the next one.
            while (in_flight.load(std::memory_order_acquire) >= queue_depth) {
                std::this_thread::yield();
            }

            PacketTask task;
            task.req_id = static_cast<uint64_t>(i);
            task.offset = offsets[i];
            task.data.assign(source_buf.begin(), source_buf.end());

            submit_ts[i] = Clock::now();
            in_flight.fetch_add(1, std::memory_order_relaxed);
            pool.dispatch(std::move(task));
        }

        pool.finish();
        pool.join();
        const Clock::time_point t_end = Clock::now();

        const bool had_failures = pool.has_error() || failures.load(std::memory_order_relaxed) > 0;
        if (had_failures) {
            std::cerr << "warning: " << failures.load(std::memory_order_relaxed)
                      << " write(s) reported failure\n";
        }
        pool.sync_and_close();

        const double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();
        const double iops = static_cast<double>(io_count) / elapsed_s;
        const double throughput_mb_s =
            (static_cast<double>(io_count) * static_cast<double>(block_size)) / elapsed_s / (1024.0 * 1024.0);

        std::sort(latency_ns.begin(), latency_ns.end());
        const double sum_ns = std::accumulate(latency_ns.begin(), latency_ns.end(), 0.0);
        const double avg_us = (sum_ns / static_cast<double>(latency_ns.size())) / 1000.0;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "elapsed:     " << elapsed_s << " s\n";
        std::cout << "IOPS:        " << iops << "\n";
        std::cout << "throughput:  " << throughput_mb_s << " MB/s\n";
        std::cout << "latency avg: " << avg_us << " us\n";
        std::cout << "latency p50: " << percentile(latency_ns, 0.50) / 1000.0 << " us\n";
        std::cout << "latency p95: " << percentile(latency_ns, 0.95) / 1000.0 << " us\n";
        std::cout << "latency p99: " << percentile(latency_ns, 0.99) / 1000.0 << " us\n";
        std::cout << "latency max: " << latency_ns.back() / 1000.0 << " us\n";

        return had_failures ? 1 : 0;

    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

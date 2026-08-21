#pragma once

#include "packet_task.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class SsdWriterPool {
public:
    // Invoked from a writer thread immediately after each pwrite() completes.
    // Optional — pass nullptr (the default) if you don't need per-op completion tracking.
    using CompletionCallback = std::function<void(uint64_t req_id, bool success)>;

    SsdWriterPool(const std::string &path, size_t num_writers, CompletionCallback on_complete = nullptr);
    ~SsdWriterPool();

    void dispatch(PacketTask &&task);
    void finish();
    void join();
    bool has_error() const;
    void sync_and_close();

    // Pre-size the output file with posix_fallocate() so writes during a run don't pay for on-demand extent allocation.
    bool preallocate(off_t size);

private:
    struct Writer {
        SpscQueue<PacketTask, 4096> queue;
        std::thread thread;
        std::atomic<bool> stop_requested {false};
    };
    void writer_loop(Writer &w);

    std::vector<std::unique_ptr<Writer>> writers_;
    std::atomic<bool> any_error_ {false};
    CompletionCallback on_complete_;
    int fd_ = -1;
    size_t next_writer_ = 0;
    bool joined_ = false;
    bool closed_ = false;
};
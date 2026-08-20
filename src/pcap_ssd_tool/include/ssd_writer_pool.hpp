#pragma once

#include "packet_task.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class SsdWriterPool {
public:
    SsdWriterPool(const std::string &path, size_t num_writers);
    ~SsdWriterPool();

    void dispatch(PacketTask &&task);
    void finish();
    void join();
    bool has_error() const;
    void sync_and_close();

private:
    struct Writer {
        SpscQueue<PacketTask, 4096> queue;
        std::thread thread;
        std::atomic<bool> stop_requested {false};
    };
    void writer_loop(Writer &w);

    std::vector<std::unique_ptr<Writer>> writers_;
    std::atomic<bool> any_error_ {false};
    int fd_ = -1;
    size_t next_writer_ = 0;
};
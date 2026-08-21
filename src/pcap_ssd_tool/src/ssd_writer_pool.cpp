#include "ssd_writer_pool.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

SsdWriterPool::SsdWriterPool(const std::string &path, size_t num_writers, CompletionCallback on_complete)
    : on_complete_(std::move(on_complete)) {
    if (num_writers == 0)
        throw std::invalid_argument("num_writers must be >= 1");

    fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0)
        throw std::runtime_error("failed to open '" + path + "': " + std::strerror(errno));

    writers_.reserve(num_writers);
    for (size_t i = 0; i < num_writers; ++i) {
        auto w = std::make_unique<Writer>();
        Writer *raw = w.get();
        writers_.push_back(std::move(w));
        writers_.back()->thread = std::thread([this, raw] { writer_loop(*raw); });
    }
}

SsdWriterPool::~SsdWriterPool() {
    if (!joined_) {
        finish();
        join();
    }

    if (!closed_ && fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool SsdWriterPool::preallocate(off_t size) {
    if (fd_ < 0)
        return false;
    return posix_fallocate(fd_, 0, size) == 0;
}

void SsdWriterPool::dispatch(PacketTask &&task) {
    size_t idx = next_writer_;
    next_writer_ = (next_writer_ + 1) % writers_.size();

    Writer &w = *writers_[idx];

    while (!w.queue.try_push(std::move(task))) {
        std::this_thread::yield();
    }
}

void SsdWriterPool::finish() {
    for (auto &w : writers_) {
        w->stop_requested.store(true, std::memory_order_release);
    }
}

void SsdWriterPool::join() {
    if (joined_)
        return;
    for (auto &w : writers_) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
    joined_ = true;
}

bool SsdWriterPool::has_error() const {
    return any_error_.load(std::memory_order_relaxed);
}

void SsdWriterPool::sync_and_close() {
    if (!joined_) {
        throw std::logic_error("sync_and_close() called before join()");
    }
    if (closed_)
        return;

    if (fsync(fd_) != 0) {
        any_error_.store(true, std::memory_order_relaxed);
    }
    if (close(fd_) != 0) {
        any_error_.store(true, std::memory_order_relaxed);
    }
    fd_ = -1;
    closed_ = true;
}

void SsdWriterPool::writer_loop(Writer &w) {
    PacketTask task;

    auto do_write = [this](const PacketTask &t) {
        ssize_t n = pwrite(fd_, t.data.data(), t.data.size(), t.offset);
        bool ok = (n >= 0 && static_cast<size_t>(n) == t.data.size());
        if (!ok) {
            any_error_.store(true, std::memory_order_relaxed);
        }
        if (on_complete_) {
            on_complete_(t.req_id, ok);
        }
    };

    while (true) {
        if (w.queue.try_pop(task)) {
            do_write(task);
            continue;
        }

        if (w.stop_requested.load(std::memory_order_acquire)) {
            if (w.queue.try_pop(task)) {
                do_write(task);
                continue;
            }
            break;
        }

        std::this_thread::yield();
    }
}

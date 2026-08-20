#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

template <typename T, size_t Capacity>
class SpscQueue {
public:
    bool try_push(T &&item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % Capacity;
        if (next == head_.load(std::memory_order_acquire))
            return false;
        buf_[tail] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T &out) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false;
        out = std::move(buf_[head]);
        head_.store((head + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buf;
    alignas(64) std::atomic<size_t> head_ {0};
    alignas(64) std::atomic<size_t> tail_ {0};
};
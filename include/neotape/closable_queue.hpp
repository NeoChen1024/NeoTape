#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace neotape {

template <typename T> class ClosableQueue {
  public:
    explicit ClosableQueue(std::size_t max_size = 0) : max_size_(max_size) {}

    ClosableQueue(const ClosableQueue &) = delete;
    ClosableQueue &operator=(const ClosableQueue &) = delete;

    bool push(T item) {
        std::unique_lock lock(mtx_);
        if (max_size_ > 0) {
            cv_.wait(lock, [this] {
                return closed_ || queue_.size() < max_size_;
            });
        }
        if (closed_)
            return false;
        queue_.push(std::move(item));
        cv_.notify_all();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        cv_.notify_all();
        return item;
    }

    void close() {
        std::lock_guard lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard lock(mtx_);
        return closed_;
    }

  private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    std::size_t max_size_ = 0;
    bool closed_ = false;
};

} // namespace neotape

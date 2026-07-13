#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace neotape {

template <typename T> class ResultStore {
  public:
    explicit ResultStore(std::size_t capacity = 0) : max_size_(capacity) {}

    ResultStore(const ResultStore &) = delete;
    ResultStore &operator=(const ResultStore &) = delete;

    bool put(uint64_t seq, T value) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] {
            return closed_ || max_size_ == 0 || results_.size() < max_size_;
        });
        if (closed_) {
            return false;
        }
        results_.emplace(seq, std::move(value));
        cv_.notify_all();
        return true;
    }

    std::optional<T> take(uint64_t seq) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock,
                 [this, seq] { return closed_ || results_.contains(seq); });
        auto it = results_.find(seq);
        if (it == results_.end()) {
            return std::nullopt;
        }
        T value = std::move(it->second);
        results_.erase(it);
        cv_.notify_all();
        return value;
    }

    void close() {
        std::scoped_lock const lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::map<uint64_t, T> results_;
    std::size_t max_size_ = 0;
    bool closed_ = false;
};

} // namespace neotape

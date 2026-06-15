#include "neotape/closable_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

void require(bool cond, const char *msg) {
    if (!cond) {
        std::cerr << "test_pax_pipeline: " << msg << "\n";
        std::exit(1);
    }
}

void test_pop_wakes_on_close() {
    neotape::ClosableQueue<int> q(1);
    std::optional<int> got = 42;
    std::thread t([&] { got = q.pop(); });
    std::this_thread::sleep_for(25ms);
    q.close();
    t.join();
    require(!got.has_value(), "closed empty queue returns nullopt");
}

void test_push_wakes_on_close() {
    neotape::ClosableQueue<int> q(1);
    require(q.push(1), "initial push succeeds");
    std::atomic<bool> finished{false};
    bool pushed = true;
    std::thread t([&] {
        pushed = q.push(2);
        finished.store(true, std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(25ms);
    require(!finished.load(std::memory_order_relaxed), "second push blocks while full");
    q.close();
    t.join();
    require(!pushed, "blocked push returns false after close");
}

void test_close_drains_existing_items() {
    neotape::ClosableQueue<int> q(2);
    require(q.push(7), "push before close succeeds");
    q.close();
    auto first = q.pop();
    auto second = q.pop();
    require(first.has_value() && *first == 7, "closed queue drains existing item");
    require(!second.has_value(), "closed drained queue returns nullopt");
    require(!q.push(8), "push after close fails");
}

class TestResultStore {
  public:
    explicit TestResultStore(size_t max_size = 0) : max_size_(max_size) {}

    bool put(uint64_t seq, std::string value) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] {
            return closed_ || max_size_ == 0 || results_.size() < max_size_;
        });
        if (closed_) {
            return false;
}
        results_.emplace(seq, std::move(value));
        cv_.notify_all();
        return true;
    }

    std::optional<std::string> take(uint64_t seq) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return closed_ || results_.contains(seq); });
        auto it = results_.find(seq);
        if (it == results_.end()) {
            return std::nullopt;
}
        std::string value = std::move(it->second);
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
    std::map<uint64_t, std::string> results_;
    size_t max_size_ = 0;
    bool closed_ = false;
};

void test_result_store_waits_for_exact_sequence() {
    TestResultStore store;
    std::optional<std::string> got;
    std::atomic<bool> completed{false};
    std::thread t([&] {
        got = store.take(5);
        completed.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(25ms);
    store.put(4, "wrong");
    std::this_thread::sleep_for(25ms);
    require(!completed.load(std::memory_order_acquire),
            "take ignores other sequence results");
    store.put(5, "right");
    t.join();
    require(got.has_value() && *got == "right", "take returns requested result");
}

void test_result_store_close_wakes_take() {
    TestResultStore store;
    std::optional<std::string> got = "not closed";
    std::thread t([&] { got = store.take(9); });
    std::this_thread::sleep_for(25ms);
    store.close();
    t.join();
    require(!got.has_value(), "closed result store wakes pending take");
}

void test_result_store_bounded_put_waits_for_space() {
    TestResultStore store(1);
    require(store.put(1, "first"), "initial bounded put succeeds");
    std::atomic<bool> second_done{false};
    bool second_put = false;
    std::thread t([&] {
        second_put = store.put(2, "second");
        second_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(25ms);
    require(!second_done.load(std::memory_order_acquire),
            "bounded result store put blocks while full");
    auto first = store.take(1);
    t.join();
    require(first.has_value() && *first == "first",
            "take returns first bounded result");
    require(second_put, "blocked bounded put succeeds after take frees space");
    auto second = store.take(2);
    require(second.has_value() && *second == "second",
            "second bounded result is stored after wait");
}

void test_result_store_close_wakes_bounded_put() {
    TestResultStore store(1);
    require(store.put(1, "first"), "initial bounded close test put succeeds");
    std::atomic<bool> second_done{false};
    bool second_put = true;
    std::thread t([&] {
        second_put = store.put(2, "second");
        second_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(25ms);
    require(!second_done.load(std::memory_order_acquire),
            "bounded result store put waits before close");
    store.close();
    t.join();
    require(!second_put, "blocked bounded put returns false after close");
}

void test_result_store_capacity_allows_earliest_late_completion() {
    TestResultStore store(3);
    require(store.put(2, "two"), "put later result 2 succeeds");
    require(store.put(1, "one"), "put later result 1 succeeds");
    std::atomic<bool> finished{false};
    bool put_zero = false;
    std::thread t([&] {
        put_zero = store.put(0, "zero");
        finished.store(true, std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(25ms);
    require(finished.load(std::memory_order_relaxed),
            "earliest result can publish after later results");
    t.join();
    require(put_zero, "earliest result put succeeds");
    auto zero = store.take(0);
    require(zero.has_value() && *zero == "zero",
            "earliest result is available");
}

} // namespace

int main() {
    test_pop_wakes_on_close();
    test_push_wakes_on_close();
    test_close_drains_existing_items();
    test_result_store_waits_for_exact_sequence();
    test_result_store_close_wakes_take();
    test_result_store_bounded_put_waits_for_space();
    test_result_store_close_wakes_bounded_put();
    test_result_store_capacity_allows_earliest_late_completion();
    return 0;
}

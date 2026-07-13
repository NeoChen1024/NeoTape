#include "neotape/closable_queue.hpp"
#include "neotape/result_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
    require(!finished.load(std::memory_order_relaxed),
            "second push blocks while full");
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
    require(first.has_value() && *first == 7,
            "closed queue drains existing item");
    require(!second.has_value(), "closed drained queue returns nullopt");
    require(!q.push(8), "push after close fails");
}

void test_result_store_waits_for_exact_sequence() {
    neotape::ResultStore<std::string> store;
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
    require(got.has_value() && *got == "right",
            "take returns requested result");
}

void test_result_store_close_wakes_take() {
    neotape::ResultStore<std::string> store;
    std::optional<std::string> got = "not closed";
    std::thread t([&] { got = store.take(9); });
    std::this_thread::sleep_for(25ms);
    store.close();
    t.join();
    require(!got.has_value(), "closed result store wakes pending take");
}

void test_result_store_bounded_put_waits_for_space() {
    neotape::ResultStore<std::string> store(1);
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
    neotape::ResultStore<std::string> store(1);
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
    neotape::ResultStore<std::string> store(3);
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

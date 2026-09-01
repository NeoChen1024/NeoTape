#include "neotape/closable_queue.hpp"
#include "neotape/result_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

void require(bool cond, const char *msg) {
    INFO(msg);
    REQUIRE(cond);
}

void test_pop_wakes_on_close() {
    neotape::ClosableQueue<int> q(1);
    auto pending = std::async(std::launch::async, [&] { return q.pop(); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "empty queue pop remains pending");
    q.close();
    require(pending.wait_for(1s) == std::future_status::ready,
            "close wakes empty queue pop before deadline");
    std::optional<int> const got = pending.get();
    require(!got.has_value(), "closed empty queue returns nullopt");
}

void test_push_wakes_on_close() {
    neotape::ClosableQueue<int> q(1);
    require(q.push(1), "initial push succeeds");
    auto pending =
        std::async(std::launch::async, [&] { return q.push(2); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "second push blocks while full");
    q.close();
    require(pending.wait_for(1s) == std::future_status::ready,
            "close wakes blocked push before deadline");
    bool const pushed = pending.get();
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
    auto pending =
        std::async(std::launch::async, [&] { return store.take(5); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "take waits for requested sequence");
    store.put(4, "wrong");
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "take ignores other sequence results");
    store.put(5, "right");
    require(pending.wait_for(1s) == std::future_status::ready,
            "requested sequence wakes take before deadline");
    std::optional<std::string> const got = pending.get();
    require(got.has_value() && *got == "right",
            "take returns requested result");
}

void test_result_store_close_wakes_take() {
    neotape::ResultStore<std::string> store;
    auto pending =
        std::async(std::launch::async, [&] { return store.take(9); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "missing result keeps take pending");
    store.close();
    require(pending.wait_for(1s) == std::future_status::ready,
            "close wakes pending take before deadline");
    std::optional<std::string> const got = pending.get();
    require(!got.has_value(), "closed result store wakes pending take");
}

void test_result_store_bounded_put_waits_for_space() {
    neotape::ResultStore<std::string> store(1);
    require(store.put(1, "first"), "initial bounded put succeeds");
    auto pending =
        std::async(std::launch::async, [&] { return store.put(2, "second"); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "bounded result store put blocks while full");
    auto first = store.take(1);
    require(pending.wait_for(1s) == std::future_status::ready,
            "free capacity wakes blocked put before deadline");
    bool const second_put = pending.get();
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
    auto pending =
        std::async(std::launch::async, [&] { return store.put(2, "second"); });
    require(pending.wait_for(50ms) == std::future_status::timeout,
            "bounded result store put waits before close");
    store.close();
    require(pending.wait_for(1s) == std::future_status::ready,
            "close wakes bounded put before deadline");
    bool const second_put = pending.get();
    require(!second_put, "blocked bounded put returns false after close");
}

void test_result_store_capacity_allows_earliest_late_completion() {
    neotape::ResultStore<std::string> store(3);
    require(store.put(2, "two"), "put later result 2 succeeds");
    require(store.put(1, "one"), "put later result 1 succeeds");
    auto pending =
        std::async(std::launch::async, [&] { return store.put(0, "zero"); });
    require(pending.wait_for(1s) == std::future_status::ready,
            "earliest result can publish after later results");
    bool const put_zero = pending.get();
    require(put_zero, "earliest result put succeeds");
    auto zero = store.take(0);
    require(zero.has_value() && *zero == "zero",
            "earliest result is available");
}

} // namespace

TEST_CASE("pax pipeline synchronization primitives", "[unit][pax]") {
    test_pop_wakes_on_close();
    test_push_wakes_on_close();
    test_close_drains_existing_items();
    test_result_store_waits_for_exact_sequence();
    test_result_store_close_wakes_take();
    test_result_store_bounded_put_waits_for_space();
    test_result_store_close_wakes_bounded_put();
    test_result_store_capacity_allows_earliest_late_completion();
}

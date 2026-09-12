#include "neotape/closable_queue.hpp"
#include "neotape/result_store.hpp"
#include "support/process.hpp"

#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

// Re-exec one Catch case so a stuck future destructor cannot hang the caller.
std::optional<neotape::test::ProcessResult>
run_isolated(std::chrono::milliseconds timeout) {
    if (std::getenv("NEOTAPE_PIPELINE_CHILD"))
        return std::nullopt;
    neotape::test::ProcessOptions options{
        {NEOTAPE_PAX_PIPELINE_TEST,
         Catch::getResultCapture().getCurrentTestName(), "--reporter",
         "compact"}};
    options.environment.emplace_back("NEOTAPE_PIPELINE_CHILD", "1");
    return neotape::test::Process::run(std::move(options), timeout);
}

template <typename Function> auto start_operation(Function operation) {
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto pending = std::async(std::launch::async,
                              [operation = std::move(operation),
                               entered = std::move(entered)]() mutable {
                                  entered.set_value();
                                  return operation();
                              });
    // Establish worker startup explicitly; wait_for below observes
    // non-completion, rather than treating elapsed time as proof of a
    // particular internal state.
    ready.wait();
    return pending;
}

TEST_CASE("pipeline: pop wakes on close", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ClosableQueue<int> q(1);
    auto pending = start_operation([&] { return q.pop(); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    q.close();
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    std::optional<int> const got = pending.get();
    REQUIRE(!got.has_value());
}

TEST_CASE("pipeline: push wakes on close", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ClosableQueue<int> q(1);
    REQUIRE(q.push(1));
    auto pending = start_operation([&] { return q.push(2); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    q.close();
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    bool const pushed = pending.get();
    REQUIRE(!pushed);
}

TEST_CASE("pipeline: close drains existing items", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ClosableQueue<int> q(2);
    REQUIRE(q.push(7));
    q.close();
    auto first = q.pop();
    auto second = q.pop();
    REQUIRE((first.has_value() && *first == 7));
    REQUIRE(!second.has_value());
    REQUIRE(!q.push(8));
}

TEST_CASE("pipeline: result store waits for exact sequence", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ResultStore<std::string> store;
    auto pending = start_operation([&] { return store.take(5); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    REQUIRE(store.put(4, "wrong"));
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    REQUIRE(store.put(5, "right"));
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    std::optional<std::string> const got = pending.get();
    REQUIRE((got.has_value() && *got == "right"));
}

TEST_CASE("pipeline: result store close wakes take", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ResultStore<std::string> store;
    auto pending = start_operation([&] { return store.take(9); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    store.close();
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    std::optional<std::string> const got = pending.get();
    REQUIRE(!got.has_value());
}

TEST_CASE("pipeline: result store bounded put waits for space", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ResultStore<std::string> store(1);
    REQUIRE(store.put(1, "first"));
    auto pending = start_operation([&] { return store.put(2, "second"); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    auto first = store.take(1);
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    bool const second_put = pending.get();
    REQUIRE((first.has_value() && *first == "first"));
    REQUIRE(second_put);
    auto second = store.take(2);
    REQUIRE((second.has_value() && *second == "second"));
}

TEST_CASE("pipeline: result store close wakes bounded put", "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ResultStore<std::string> store(1);
    REQUIRE(store.put(1, "first"));
    auto pending = start_operation([&] { return store.put(2, "second"); });
    REQUIRE(pending.wait_for(50ms) == std::future_status::timeout);
    store.close();
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    bool const second_put = pending.get();
    REQUIRE(!second_put);
}

TEST_CASE("pipeline: result store capacity allows earliest late completion",
          "[unit][pax]") {
    if (auto result = run_isolated(3s)) {
        INFO(result->standard_output);
        INFO(result->standard_error);
        REQUIRE_FALSE(result->timed_out);
        REQUIRE(result->exit_code == 0);
        return;
    }
    neotape::ResultStore<std::string> store(3);
    REQUIRE(store.put(2, "two"));
    REQUIRE(store.put(1, "one"));
    auto pending = start_operation([&] { return store.put(0, "zero"); });
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    bool const put_zero = pending.get();
    REQUIRE(put_zero);
    auto zero = store.take(0);
    REQUIRE((zero.has_value() && *zero == "zero"));
}

} // namespace

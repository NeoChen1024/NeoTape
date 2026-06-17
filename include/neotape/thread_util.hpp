#pragma once

#include <thread>

namespace neotape {

// RAII thread joiner — joins on destruction.
struct ThreadJoiner {
    std::thread &thread;
    explicit ThreadJoiner(std::thread &t) : thread(t) {}
    ~ThreadJoiner() {
        if (thread.joinable()) {
            thread.join();
        }
    }
    ThreadJoiner(const ThreadJoiner &) = delete;
    ThreadJoiner &operator=(const ThreadJoiner &) = delete;
    ThreadJoiner(ThreadJoiner &&) = delete;
    ThreadJoiner &operator=(ThreadJoiner &&) = delete;
};

} // namespace neotape

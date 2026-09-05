#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace neotape {

// Callback runs on one owned thread. Stop/join before destroying its inputs.
// Renderers retain ownership of their display format and final summary.
class PeriodicProgress {
  public:
    explicit PeriodicProgress(std::function<void()> sample);
    ~PeriodicProgress();
    PeriodicProgress(const PeriodicProgress &) = delete;
    PeriodicProgress &operator=(const PeriodicProgress &) = delete;
    void stop();

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::thread thread_;
};

struct ProgressRates {
    uint64_t input = 0, output = 0, items = 0;
};

class RateSampler {
  public:
    ProgressRates sample(uint64_t input, uint64_t output, uint64_t items);

  private:
    std::chrono::steady_clock::time_point time_ =
        std::chrono::steady_clock::now();
    ProgressRates previous_;
};

std::string count_rate(uint64_t per_second);
unsigned buffer_percent(uint64_t used, uint64_t capacity);

} // namespace neotape

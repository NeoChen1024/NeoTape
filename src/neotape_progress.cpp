#include "neotape/common.hpp"
#include "neotape/progress.hpp"

#include <algorithm>
#include <format>

namespace neotape {

PeriodicProgress::PeriodicProgress(std::function<void()> sample)
    : thread_([this, sample = std::move(sample)] {
          std::unique_lock lock(mutex_);
          while (!cv_.wait_for(lock, std::chrono::seconds(1),
                               [this] { return stopped_; })) {
              lock.unlock();
              try {
                  sample();
              } catch (...) {
                  // Progress must not terminate the data pipeline.
                  return;
              }
              lock.lock();
          }
      }) {}

PeriodicProgress::~PeriodicProgress() { stop(); }

void PeriodicProgress::stop() {
    {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    finish_progress();
}

ProgressRates RateSampler::sample(uint64_t input, uint64_t output,
                                  uint64_t items) {
    auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - time_).count();
    auto rate = [seconds](uint64_t current, uint64_t old) {
        return seconds > 0 && current >= old
                   ? static_cast<uint64_t>((current - old) / seconds)
                   : 0;
    };
    ProgressRates result{rate(input, previous_.input),
                         rate(output, previous_.output),
                         rate(items, previous_.items)};
    previous_ = {input, output, items};
    time_ = now;
    return result;
}

std::string count_rate(uint64_t n) {
    if (n < 1000)
        return std::to_string(n);
    if (n < 1000000)
        return std::format("{:.1f}k", n / 1000.0);
    return std::format("{:.1f}M", n / 1000000.0);
}

unsigned buffer_percent(uint64_t used, uint64_t capacity) {
    return capacity == 0
               ? 0
               : static_cast<unsigned>(std::min(
                     100.0L, static_cast<long double>(used) * 100 / capacity));
}

} // namespace neotape

# Pax Writer Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fragile unplanned pax writer multi-source wake protocol with a reliable channel-driven ordered pipeline.

**Architecture:** The walker emits one ordered `OrderItem` per sequence into `order_queue`. Small files are serialized by workers through `work_queue` and `ResultStore`; large files are streamed by the serializer when their ordered item is reached. A `PaxPipeline` owner centralizes cancellation, queue closure, thread joining, and error propagation.

**Tech Stack:** C++20, GNU Make, libarchive, bundled BLAKE3, existing shell smoke tests.

---

## File Structure

- Create: `include/neotape/closable_queue.hpp` — reusable bounded close-aware blocking queue for pipeline channels and unit tests.
- Create: `tests/test_pax_pipeline.cpp` — focused tests for `ClosableQueue` and a small test-only `ResultStore` exercise through public test helpers.
- Modify: `Makefile` — build and run `bin/test_pax_pipeline` in `make test`.
- Modify: `src/neotape_pax_writer.cpp` — replace unplanned-mode `BlockingQueue`/worker-slot/`notify_generation` pipeline with `PaxPipeline`, `ResultStore`, `OrderItem`, `WorkItem`, RAII entry/fd ownership, and structured cancellation.
- Modify: `docs/implementation/mt-pax-architecture.md` — document the new single ordered stream and remove the obsolete generation-counter architecture.

## Task 1: Add Closable Queue Tests First

**Files:**
- Create: `include/neotape/closable_queue.hpp`
- Create: `tests/test_pax_pipeline.cpp`
- Modify: `Makefile:8,44,88-91`

- [ ] **Step 1: Create the test file with failing queue tests**

Add `tests/test_pax_pipeline.cpp`:

```cpp
#include "neotape/closable_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

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

} // namespace

int main() {
    test_pop_wakes_on_close();
    test_push_wakes_on_close();
    test_close_drains_existing_items();
    return 0;
}
```

- [ ] **Step 2: Wire the test into the Makefile**

Update `Makefile` exactly in these places:

```make
EXE	= bin/mt-pax bin/neotape bin/neotape-inspect bin/test_tape bin/test_cli bin/test_restore_validation bin/test_pax_pipeline
```

```make
.PHONY: all clean countline format test test_pax_cli test_tape_backup_wiring test_inspect_diagnostic test_file_backed_tape generate tidy
```

No `.PHONY` change is required for the new binary.

Add after the `bin/test_restore_validation` rule:

```make
$(BINDIR)/test_pax_pipeline : tests/test_pax_pipeline.cpp Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@
```

Update `test` dependencies and commands:

```make
test: $(BINDIR)/test_tape $(BINDIR)/test_cli $(BINDIR)/test_restore_validation $(BINDIR)/test_pax_pipeline $(BINDIR)/neotape $(BINDIR)/neotape-inspect
	$(BINDIR)/test_tape
	$(BINDIR)/test_cli
	$(BINDIR)/test_restore_validation
	$(BINDIR)/test_pax_pipeline
```

Keep the existing shell smoke commands after these binary invocations.

- [ ] **Step 3: Run the focused test to verify it fails**

Run: `make bin/test_pax_pipeline`

Expected: compile failure because `neotape/closable_queue.hpp` does not exist.

- [ ] **Step 4: Add the minimal `ClosableQueue` implementation**

Create `include/neotape/closable_queue.hpp`:

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>

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
```

- [ ] **Step 5: Run the focused test to verify it passes**

Run: `make bin/test_pax_pipeline && bin/test_pax_pipeline`

Expected: command exits `0` with no output.

- [ ] **Step 6: Commit if authorized**

Run only if commits have been explicitly requested: `git add include/neotape/closable_queue.hpp tests/test_pax_pipeline.cpp Makefile && git commit -m "test: add pax pipeline queue coverage"`

## Task 2: Add ResultStore and Tests

**Files:**
- Modify: `src/neotape_pax_writer.cpp`
- Modify: `tests/test_pax_pipeline.cpp`

- [ ] **Step 1: Add `ResultStore` tests through a small test-local equivalent**

First add these includes at the top of `tests/test_pax_pipeline.cpp` with the existing includes:

```cpp
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
```

Then append these helpers and tests before `main()`:

```cpp
class TestResultStore {
  public:
    void put(uint64_t seq, std::string value) {
        std::lock_guard lock(mtx_);
        results_.emplace(seq, std::move(value));
        cv_.notify_all();
    }

    std::optional<std::string> take(uint64_t seq) {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return closed_ || results_.contains(seq); });
        auto it = results_.find(seq);
        if (it == results_.end())
            return std::nullopt;
        std::string value = std::move(it->second);
        results_.erase(it);
        return value;
    }

    void close() {
        std::lock_guard lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::map<uint64_t, std::string> results_;
    bool closed_ = false;
};

void test_result_store_waits_for_exact_sequence() {
    TestResultStore store;
    std::optional<std::string> got;
    std::thread t([&] { got = store.take(5); });
    std::this_thread::sleep_for(25ms);
    store.put(4, "wrong");
    std::this_thread::sleep_for(25ms);
    require(!got.has_value(), "take ignores other sequence results");
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
```

Call them in `main()`:

```cpp
int main() {
    test_pop_wakes_on_close();
    test_push_wakes_on_close();
    test_close_drains_existing_items();
    test_result_store_waits_for_exact_sequence();
    test_result_store_close_wakes_take();
    return 0;
}
```

- [ ] **Step 2: Run the focused test**

Run: `make bin/test_pax_pipeline && bin/test_pax_pipeline`

Expected: command exits `0` with no output.

- [ ] **Step 3: Add production `ResultStore` to `src/neotape_pax_writer.cpp`**

Add this after `WorkItem` and before planned-mode types:

```cpp
struct ResultStore {
    std::mutex mtx;
    std::condition_variable cv;
    std::map<uint64_t, Result> results;
    bool closed = false;

    bool put(Result result) {
        std::lock_guard lock(mtx);
        if (closed)
            return false;
        results.emplace(result.seq, std::move(result));
        cv.notify_all();
        return true;
    }

    std::optional<Result> take(uint64_t seq) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [&] { return closed || results.contains(seq); });
        auto it = results.find(seq);
        if (it == results.end())
            return std::nullopt;
        Result result = std::move(it->second);
        results.erase(it);
        cv.notify_all();
        return result;
    }

    void close() {
        std::lock_guard lock(mtx);
        closed = true;
        cv.notify_all();
    }
};
```

- [ ] **Step 4: Build to verify the unused production type compiles**

Run: `make src/neotape_pax_writer.o bin/test_pax_pipeline`

Expected: both targets build successfully.

- [ ] **Step 5: Commit if authorized**

Run only if commits have been explicitly requested: `git add src/neotape_pax_writer.cpp tests/test_pax_pipeline.cpp && git commit -m "feat: add pax worker result store"`

## Task 3: Add RAII Ownership and Ordered Item Types

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Add RAII wrappers and ordered item types**

In `src/neotape_pax_writer.cpp`, add `#include <variant>` with the other standard includes.

Add these new types after the current `WorkItem` struct. The old `WorkItem` and slot code stay in place until Task 4 removes them, so use `Pipeline*` names in this task:

```cpp
struct EntryHandle {
    archive_entry *ptr = nullptr;

    EntryHandle() = default;
    explicit EntryHandle(archive_entry *entry) : ptr(entry) {}
    ~EntryHandle() { reset(); }

    EntryHandle(const EntryHandle &) = delete;
    EntryHandle &operator=(const EntryHandle &) = delete;

    EntryHandle(EntryHandle &&other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    EntryHandle &operator=(EntryHandle &&other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    archive_entry *get() const { return ptr; }
    archive_entry *release() {
        archive_entry *entry = ptr;
        ptr = nullptr;
        return entry;
    }
    explicit operator bool() const { return ptr != nullptr; }

    void reset(archive_entry *entry = nullptr) {
        if (ptr != nullptr)
            archive_entry_free(ptr);
        ptr = entry;
    }
};

struct FdHandle {
    int fd = -1;

    FdHandle() = default;
    explicit FdHandle(int file_fd) : fd(file_fd) {}
    ~FdHandle() { reset(); }

    FdHandle(const FdHandle &) = delete;
    FdHandle &operator=(const FdHandle &) = delete;

    FdHandle(FdHandle &&other) noexcept : fd(other.fd) { other.fd = -1; }

    FdHandle &operator=(FdHandle &&other) noexcept {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    int get() const { return fd; }
    int release() {
        int file_fd = fd;
        fd = -1;
        return file_fd;
    }
    explicit operator bool() const { return fd >= 0; }

    void reset(int file_fd = -1) {
        if (fd >= 0)
            close(fd);
        fd = file_fd;
    }
};

struct PipelineWorkItem {
    uint64_t seq = 0;
    EntryHandle entry;
    FdHandle fd;
};

struct InlineBytes {
    uint64_t seq = 0;
    vector<std::byte> bytes;
};

struct WorkerResultRef {
    uint64_t seq = 0;
};

struct PipelineLargeEntry {
    uint64_t seq = 0;
    EntryHandle entry;
    FdHandle fd;
};

using PipelineOrderPayload = std::variant<InlineBytes, WorkerResultRef, PipelineLargeEntry>;

struct PipelineOrderItem {
    uint64_t seq = 0;
    PipelineOrderPayload payload;
};
```

- [ ] **Step 2: Build to verify the new ownership types compile beside the old pipeline**

Run: `make src/neotape_pax_writer.o`

Expected: target builds successfully.

- [ ] **Step 3: Commit if authorized**

Run only if commits have been explicitly requested: `git add src/neotape_pax_writer.cpp && git commit -m "feat: add pax pipeline ownership types"`

## Task 4: Replace Worker Slots with Work Queue and ResultStore

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Include the new queue header**

Add near existing project includes:

```cpp
#include "neotape/closable_queue.hpp"
```

- [ ] **Step 2: Delete obsolete synchronization structures**

Remove the old internal `BlockingQueue<T>` class, `SlotState`, `WorkerSlot`, old `worker_main(...)`, and old `serializer_main(...)` definitions. Rename the new types from Task 3 as part of the same edit:

```cpp
using WorkItem = PipelineWorkItem;
using LargeEntry = PipelineLargeEntry;
using OrderPayload = PipelineOrderPayload;
using OrderItem = PipelineOrderItem;
```

- [ ] **Step 3: Add `PipelineCancel` helper**

Add before the new worker/serializer functions:

```cpp
struct PipelineCancel {
    std::mutex mtx;
    std::exception_ptr error;
    std::atomic<bool> requested{false};

    void request(std::exception_ptr e) {
        bool expected = false;
        if (requested.compare_exchange_strong(expected, true)) {
            std::lock_guard lock(mtx);
            error = e;
        }
    }

    void rethrow_if_set() {
        std::exception_ptr saved;
        {
            std::lock_guard lock(mtx);
            saved = error;
        }
        if (saved)
            std::rethrow_exception(saved);
    }
};
```

- [ ] **Step 4: Add new worker main**

Add:

```cpp
void worker_main(ClosableQueue<WorkItem> &work_queue, ResultStore &results,
                 PipelineCancel &cancel) {
    for (;;) {
        auto item = work_queue.pop();
        if (!item.has_value())
            return;
        try {
            vector<std::byte> bytes = serialize_entry(item->entry.get(), item->fd.get());
            Result result{item->seq, std::move(bytes)};
            if (!results.put(std::move(result)))
                return;
        } catch (...) {
            cancel.request(std::current_exception());
            work_queue.close();
            results.close();
            return;
        }
    }
}
```

- [ ] **Step 5: Build to expose serializer dependencies**

Run: `make src/neotape_pax_writer.o`

Expected: compile failure because `write_pax_archive()` still starts the old serializer and worker slot code that was removed.

## Task 5: Add New Serializer and PaxPipeline Owner

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Add ordered emit helper**

Add before serializer:

```cpp
bool emit_bytes_to_bb1(BBSink &sink, vector<std::byte> bytes) {
    if (bytes.empty())
        return true;
    size_t chunk_size = bytes.size();
    if (!sink.dest->push(std::move(bytes)))
        return false;
    sink.stats->input_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
    return true;
}
```

- [ ] **Step 2: Add new serializer main**

Add:

```cpp
void serializer_main(ClosableQueue<OrderItem> &order_queue,
                     ResultStore &results, BBSink &bb1_sink,
                     PipelineCancel &cancel) {
    for (;;) {
        auto item = order_queue.pop();
        if (!item.has_value())
            return;
        try {
            bool keep_running = true;
            std::visit(
                [&](auto &payload) {
                    using Payload = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<Payload, InlineBytes>) {
                        keep_running = emit_bytes_to_bb1(bb1_sink, std::move(payload.bytes));
                    } else if constexpr (std::is_same_v<Payload, WorkerResultRef>) {
                        auto result = results.take(payload.seq);
                        if (!result.has_value()) {
                            keep_running = false;
                            return;
                        }
                        keep_running = emit_bytes_to_bb1(bb1_sink, std::move(result->bytes));
                    } else if constexpr (std::is_same_v<Payload, LargeEntry>) {
                        stream_large_entry(bb1_sink, payload.entry.get(), payload.fd.get());
                    }
                },
                item->payload);
            if (!keep_running)
                return;
        } catch (...) {
            cancel.request(std::current_exception());
            order_queue.close();
            results.close();
            bb1_sink.dest->close();
            return;
        }
    }
}
```

- [ ] **Step 3: Add `PaxPipeline` owner**

Add after serializer:

```cpp
class PaxPipeline {
  public:
    PaxPipeline(const Options &opts, PaxWriterCallbacks &callbacks,
                ArchiveStats &stats, blake3_hasher &hasher)
        : opts_(opts), callbacks_(callbacks), stats_(stats), hasher_(hasher),
          bb1_(opts.output_buf_size),
          order_queue_(std::max<size_t>(64, opts.io_thread * 8)),
          work_queue_(std::max<size_t>(1, opts.io_thread * 4)),
          bb1_sink_{&bb1_, &stats_, {}, false} {}

    void start() {
        output_thread_ = std::thread([this] { output_main(); });
        unsigned nworkers = opts_.io_thread > 0 ? opts_.io_thread - 1 : 0;
        for (unsigned i = 0; i < nworkers; ++i)
            workers_.emplace_back(worker_main, std::ref(work_queue_),
                                  std::ref(results_), std::ref(cancel_));
        serializer_thread_ = std::thread(serializer_main, std::ref(order_queue_),
                                         std::ref(results_), std::ref(bb1_sink_),
                                         std::ref(cancel_));
    }

    bool enqueue_inline(uint64_t seq, vector<std::byte> bytes) {
        return order_queue_.push(OrderItem{seq, InlineBytes{seq, std::move(bytes)}});
    }

    bool enqueue_small(uint64_t seq, EntryHandle entry, FdHandle fd) {
        if (!work_queue_.push(WorkItem{seq, std::move(entry), std::move(fd)}))
            return false;
        if (!order_queue_.push(OrderItem{seq, WorkerResultRef{seq}})) {
            request_cancel(std::make_exception_ptr(
                std::runtime_error("pax pipeline cancelled before ordered work publish")));
            return false;
        }
        return true;
    }

    bool enqueue_large(uint64_t seq, EntryHandle entry, FdHandle fd) {
        return order_queue_.push(
            OrderItem{seq, LargeEntry{seq, std::move(entry), std::move(fd)}});
    }

    void finish_input() {
        work_queue_.close();
        order_queue_.close();
    }

    void request_cancel(std::exception_ptr e) {
        cancel_.request(e);
        work_queue_.close();
        order_queue_.close();
        results_.close();
        bb1_.close();
    }

    void join() {
        finish_input();
        for (auto &worker : workers_)
            if (worker.joinable())
                worker.join();
        results_.close();
        if (serializer_thread_.joinable())
            serializer_thread_.join();
        bb1_.close();
        if (output_thread_.joinable())
            output_thread_.join();
        cancel_.rethrow_if_set();
    }

    size_t buffered_bytes() const { return bb1_.size_bytes(); }
    size_t buffer_capacity() const { return bb1_.capacity_bytes(); }

  private:
    void output_main() {
        size_t output_restart_bytes =
            opts_.output_buf_size * opts_.buffer_percent / 100;
        bool wait_for_waterline = output_restart_bytes > 0;
        try {
            for (;;) {
                auto chunk = wait_for_waterline
                                 ? bb1_.pop_after_fill(output_restart_bytes)
                                 : bb1_.pop();
                if (chunk.empty())
                    break;
                callbacks_.write_chunk(PaxChunk{
                    .slice = 0,
                    .bytes = std::span<const std::byte>(chunk.data(), chunk.size()),
                });
                blake3_hasher_update(&hasher_, chunk.data(), chunk.size());
                stats_.output_bytes.fetch_add(chunk.size(), std::memory_order_relaxed);
                wait_for_waterline =
                    output_restart_bytes > 0 && bb1_.size_bytes() == 0;
            }
        } catch (...) {
            request_cancel(std::current_exception());
        }
    }

    const Options &opts_;
    PaxWriterCallbacks &callbacks_;
    ArchiveStats &stats_;
    blake3_hasher &hasher_;
    BoundedBuffer bb1_;
    ClosableQueue<OrderItem> order_queue_;
    ClosableQueue<WorkItem> work_queue_;
    ResultStore results_;
    PipelineCancel cancel_;
    BBSink bb1_sink_;
    vector<std::thread> workers_;
    std::thread serializer_thread_;
    std::thread output_thread_;
};
```

- [ ] **Step 4: Build to catch missing includes and type errors**

Run: `make src/neotape_pax_writer.o`

Expected: compile errors where `write_pax_archive()` still uses removed local variables. Resolve in Task 6.

## Task 6: Rewrite Unplanned `write_pax_archive()` Dispatch Path

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Replace old pipeline setup**

In unplanned `write_pax_archive()`, keep callback defaults, `callbacks.begin_slice(0)`, stats thread, chdir, hardlink resolver, source walking, hardlink flush, hash finalization, and final `callbacks.end_slice(0)`. Replace local `BoundedBuffer`, `bb0`, `output_thread`, `WorkerSlot`, `idle_queue`, `notify_*`, `completed_queue`, worker start, serializer start, and old shutdown with:

```cpp
PaxPipeline pipeline(opts, callbacks, stats, hasher);
pipeline.start();
```

The stats thread should report buffer state through:

```cpp
size_t buffered = pipeline.buffered_bytes();
size_t capacity = pipeline.buffer_capacity();
```

- [ ] **Step 2: Replace `dispatch_entry` body**

Use this dispatch body:

```cpp
auto dispatch_entry = [&](archive_entry *raw_entry) {
    EntryHandle entry(raw_entry);
    if (!entry)
        return;
    stats.walked_entries.fetch_add(1, std::memory_order_relaxed);

    bool is_reg = (archive_entry_filetype(entry.get()) == AE_IFREG);
    la_int64_t size = archive_entry_size(entry.get());
    bool has_data = (is_reg && size > 0);

    if (opts.verbose > 1)
        cerr << format("\n{}", verbose_line(entry.get()));
    else if (opts.verbose > 0)
        cerr << format("\na {}", entry_display_path(entry.get()));

    if (!has_data) {
        uint64_t seq = next_seq++;
        vector<std::byte> bytes = serialize_entry(entry.get(), -1);
        entry.reset();
        if (!pipeline.enqueue_inline(seq, std::move(bytes)))
            throw std::runtime_error("pax pipeline closed while enqueueing metadata entry");
        return;
    }

    FdHandle fd(open_entry_file(entry.get()));
    if (!fd) {
        entry.reset();
        return;
    }

    uint64_t seq = next_seq++;
    unsigned nworkers = opts.io_thread > 0 ? opts.io_thread - 1 : 0;
    bool queued = false;
    if (nworkers == 0 || static_cast<size_t>(size) > SMALL_FILE_THRESHOLD)
        queued = pipeline.enqueue_large(seq, std::move(entry), std::move(fd));
    else
        queued = pipeline.enqueue_small(seq, std::move(entry), std::move(fd));
    if (!queued)
        throw std::runtime_error("pax pipeline closed while enqueueing file entry");
};
```

- [ ] **Step 3: Replace shutdown with structured join**

After hardlink flush and `archive_entry_linkresolver_free(resolver);`, call:

```cpp
pipeline.finish_input();
pipeline.join();
```

Then stop stats:

```cpp
stats.done.store(true, std::memory_order_relaxed);
if (stats_thread.joinable())
    stats_thread.join();
```

- [ ] **Step 4: Ensure cancellation on walker exceptions**

Wrap the source walk and hardlink flush region in:

```cpp
try {
    // existing source walk and hardlink flush
    archive_entry_linkresolver_free(resolver);
    pipeline.finish_input();
} catch (...) {
    archive_entry_linkresolver_free(resolver);
    pipeline.request_cancel(std::current_exception());
}
pipeline.join();
```

Use a boolean or small RAII wrapper if needed so `archive_entry_linkresolver_free()` is called exactly once.

- [ ] **Step 5: Build `mt-pax` and `neotape`**

Run: `make bin/mt-pax bin/neotape`

Expected: both binaries build successfully.

- [ ] **Step 6: Commit if authorized**

Run only if commits have been explicitly requested: `git add src/neotape_pax_writer.cpp && git commit -m "feat: replace pax writer wake protocol"`

## Task 7: Convert Worker-Capable Fatal Exits to Exceptions

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Add throwing archive helpers**

Add near diagnostics:

```cpp
[[noreturn]] void throw_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    throw std::runtime_error(format("{}{}", context,
                                    msg != nullptr ? format(": {}", msg) : string()));
}

[[noreturn]] void throw_errno(const string &context) {
    throw std::runtime_error(format("{}: {}", context, std::strerror(errno)));
}

void check_archive_throw(int r, archive *a, const char *context) {
    if (r == ARCHIVE_OK)
        return;
    if (r == ARCHIVE_WARN) {
        const char *msg = archive_error_string(a);
        cerr << format("pax: warning: {}{}\n", context,
                       msg != nullptr ? format(": {}", msg) : string());
        return;
    }
    throw_archive(context, a);
}
```

- [ ] **Step 2: Use throwing helpers in `copy_file_data`, `serialize_entry`, and `stream_large_entry`**

In worker-capable helpers only, replace `fail_archive`, `fail_errno`, and `std::exit(1)` paths with `throw_archive`, `throw_errno`, or `throw std::runtime_error(...)`. Preserve existing warnings for `ARCHIVE_WARN`.

The important replacements are:

```cpp
if (src == nullptr)
    throw_archive("entry has no source path", writer);
```

```cpp
if (n < 0)
    throw_errno(string("read ") + src);
```

```cpp
if (w < 0)
    throw_archive("write file data", writer);
if (w != n)
    throw std::runtime_error(format("short archive write for {}", src));
```

```cpp
if (!a)
    throw std::runtime_error("cannot allocate archive writer");
```

- [ ] **Step 3: Build and run focused tests**

Run: `make bin/mt-pax bin/neotape bin/test_pax_pipeline && bin/test_pax_pipeline`

Expected: build succeeds and `bin/test_pax_pipeline` exits `0`.

- [ ] **Step 4: Commit if authorized**

Run only if commits have been explicitly requested: `git add src/neotape_pax_writer.cpp && git commit -m "fix: propagate pax worker failures"`

## Task 8: Add Stress and Regression Smoke Coverage

**Files:**
- Create: `tests/smoke_mt_pax_pipeline.sh`
- Modify: `Makefile:88-96`

- [ ] **Step 1: Add shell smoke test**

Create `tests/smoke_mt_pax_pipeline.sh`:

```sh
#!/bin/sh
set -eu

root=/tmp/neotape-mt-pax-pipeline-root
archive=/tmp/neotape-mt-pax-pipeline.tar
out=/tmp/neotape-mt-pax-pipeline-out
err=/tmp/neotape-mt-pax-pipeline.err

rm -rf "$root" "$archive" "$out" "$err"
mkdir -p "$root/src/dirs" "$root/src/small" "$out"

i=0
while test "$i" -lt 80; do
    mkdir -p "$root/src/dirs/dir-$i"
    printf 'small file %s\n' "$i" > "$root/src/small/file-$i.txt"
    ln -s "../small/file-$i.txt" "$root/src/dirs/dir-$i/link-$i"
    i=$((i + 1))
done

dd if=/dev/zero of="$root/src/large-a.bin" bs=1M count=5 2>/dev/null
dd if=/dev/zero of="$root/src/large-b.bin" bs=1M count=6 2>/dev/null

bin/mt-pax -f "$archive" -C "$root" --io-thread 4 -P 25 --output-buffer-size 8M src >/dev/null 2>"$err"
test -s "$archive"
grep -q 'in @ .* out @ .* files @ .* slice .* total, buffer' "$err"

if command -v bsdtar >/dev/null 2>&1; then
    bsdtar -xpf "$archive" -C "$out"
    cmp "$root/src/small/file-17.txt" "$out/src/small/file-17.txt"
    test -L "$out/src/dirs/dir-17/link-17"
fi
```

- [ ] **Step 2: Wire the smoke test into `make test`**

Add to `Makefile` test recipe after `sh tests/smoke_pax_backup_restore.sh`:

```make
	sh tests/smoke_mt_pax_pipeline.sh
```

- [ ] **Step 3: Run the new smoke test**

Run: `make bin/mt-pax && sh tests/smoke_mt_pax_pipeline.sh`

Expected: command exits `0`.

- [ ] **Step 4: Commit if authorized**

Run only if commits have been explicitly requested: `git add tests/smoke_mt_pax_pipeline.sh Makefile && git commit -m "test: cover mt-pax ordered pipeline"`

## Task 9: Update Architecture Documentation

**Files:**
- Modify: `docs/implementation/mt-pax-architecture.md`

- [ ] **Step 1: Update thread role descriptions**

Replace references to `bb0`, `completed_queue`, `notify_generation`, worker slots, and large slot with these concepts:

```markdown
- **Walker** emits one `OrderItem` per sequence into `order_queue`; small-file data work is separately submitted to `work_queue`.
- **Workers** consume `work_queue` and publish completed small-file bytes into `ResultStore` by sequence number.
- **Serializer** consumes `order_queue` FIFO; for `WorkerResult(seq)` it waits for `ResultStore::take(seq)`, for `LargeEntry` it streams directly to `bb1`, and for `InlineBytes` it pushes bytes to `bb1`.
- **Output** drains `bb1`, writes callbacks, updates BLAKE3, and reports callback errors through pipeline cancellation.
```

- [ ] **Step 2: Update shutdown sequence**

Document:

```markdown
1. Walker finishes dispatch and closes `work_queue` and `order_queue`.
2. Workers drain accepted work and exit when `work_queue` is closed.
3. Serializer drains `order_queue`, waits only for referenced worker results, then exits.
4. Pipeline closes `ResultStore` and `bb1`, joins workers, serializer, and output.
5. Any stored exception is rethrown after joins.
```

- [ ] **Step 3: Run doc whitespace check**

Run: `git diff --check -- docs/implementation/mt-pax-architecture.md docs/superpowers/specs/2026-05-29-pax-writer-reliability-design.md docs/superpowers/plans/2026-05-29-pax-writer-reliability.md`

Expected: no output and exit `0`.

- [ ] **Step 4: Commit if authorized**

Run only if commits have been explicitly requested: `git add docs/implementation/mt-pax-architecture.md docs/superpowers/specs/2026-05-29-pax-writer-reliability-design.md docs/superpowers/plans/2026-05-29-pax-writer-reliability.md && git commit -m "docs: describe pax writer reliability redesign"`

## Task 10: Full Verification

**Files:**
- No code changes unless verification reveals defects.

- [ ] **Step 1: Build all binaries**

Run: `make -j "$(nproc)"`

Expected: build exits `0` and produces `bin/mt-pax`, `bin/neotape`, and all test binaries.

- [ ] **Step 2: Run full test suite**

Run: `make test`

Expected: all C++ tests and shell smoke tests exit `0`.

- [ ] **Step 3: Run repeated pipeline stress loop**

Run: `i=0; while test "$i" -lt 20; do sh tests/smoke_mt_pax_pipeline.sh; i=$((i + 1)); done`

Expected: loop exits `0` without hangs.

- [ ] **Step 4: Inspect final diff**

Run: `git diff -- src/neotape_pax_writer.cpp include/neotape/closable_queue.hpp tests/test_pax_pipeline.cpp tests/smoke_mt_pax_pipeline.sh Makefile docs/implementation/mt-pax-architecture.md docs/superpowers/specs/2026-05-29-pax-writer-reliability-design.md docs/superpowers/plans/2026-05-29-pax-writer-reliability.md`

Expected: diff contains only the reliability redesign, tests, Makefile wiring, and docs.

- [ ] **Step 5: Commit if authorized**

Run only if commits have been explicitly requested: `git status --short && git diff --check && git log --oneline -10`

If status and diff are expected, run: `git add include/neotape/closable_queue.hpp tests/test_pax_pipeline.cpp tests/smoke_mt_pax_pipeline.sh Makefile src/neotape_pax_writer.cpp docs/implementation/mt-pax-architecture.md docs/superpowers/specs/2026-05-29-pax-writer-reliability-design.md docs/superpowers/plans/2026-05-29-pax-writer-reliability.md && git commit -m "refactor: harden pax writer pipeline"`

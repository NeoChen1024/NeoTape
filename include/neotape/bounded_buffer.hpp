#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace neotape {

class BoundedBuffer {
public:
	explicit BoundedBuffer(size_t capacity_bytes);
	~BoundedBuffer();

	BoundedBuffer(const BoundedBuffer &) = delete;
	BoundedBuffer &operator=(const BoundedBuffer &) = delete;

	bool push(std::vector<std::byte> item);
	std::vector<std::byte> pop();
	void close();
	bool drained() const;

private:
	mutable std::mutex mtx_;
	std::condition_variable not_full_;
	std::condition_variable not_empty_;
	std::deque<std::vector<std::byte>> buf_;
	size_t total_bytes_ = 0;
	size_t capacity_;
	bool closed_ = false;
};

} // namespace neotape

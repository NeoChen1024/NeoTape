#include "neotape/bounded_buffer.hpp"

using neotape::BoundedBuffer;

BoundedBuffer::BoundedBuffer(size_t capacity_bytes) : capacity_(capacity_bytes) {}
BoundedBuffer::~BoundedBuffer() = default;

bool BoundedBuffer::push(std::vector<std::byte> item) {
	std::unique_lock lock(mtx_);
	not_full_.wait(lock, [this] { return total_bytes_ < capacity_ || closed_; });
	if (closed_)
		return false;
	total_bytes_ += item.size();
	buf_.push_back(std::move(item));
	not_empty_.notify_one();
	return true;
}

std::vector<std::byte> BoundedBuffer::pop() {
	std::unique_lock lock(mtx_);
	not_empty_.wait(lock, [this] { return !buf_.empty() || closed_; });
	if (buf_.empty())
		return {};
	auto item = std::move(buf_.front());
	buf_.pop_front();
	total_bytes_ -= item.size();
	not_full_.notify_one();
	return item;
}

void BoundedBuffer::close() {
	std::lock_guard lock(mtx_);
	closed_ = true;
	not_empty_.notify_all();
	not_full_.notify_all();
}

bool BoundedBuffer::drained() const {
	std::lock_guard lock(mtx_);
	return closed_ && buf_.empty();
}

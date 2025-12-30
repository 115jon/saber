#ifndef SABER_RING_BUFFER_HPP
#define SABER_RING_BUFFER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace saber {

/**
 * Lock-free ring buffer for single-producer, single-consumer scenarios.
 * Optimized for audio streaming with O(1) read/write operations.
 */
struct RingBuffer {
	explicit RingBuffer(size_t capacity)
		: m_buffer(capacity), m_capacity(capacity) {}

	/**
	 * Write data to the ring buffer.
	 * Returns the number of bytes actually written (may be less than requested
	 * if buffer is full).
	 */
	size_t write(const uint8_t *data, size_t length) {
		if (length == 0 || data == nullptr) { return 0; }

		// Don't write more than available space
		const size_t available = m_capacity - m_size;
		const size_t to_write = std::min(length, available);

		if (to_write == 0) { return 0; }

		// Calculate how much we can write before wrapping
		const size_t until_end = m_capacity - m_write_pos;
		const size_t first_chunk = std::min(to_write, until_end);

		// Write first chunk
		std::memcpy(m_buffer.data() + m_write_pos, data, first_chunk);

		// Write second chunk if we wrap around
		if (first_chunk < to_write) {
			const size_t second_chunk = to_write - first_chunk;
			std::memcpy(m_buffer.data(), data + first_chunk, second_chunk);
		}

		// Update write position (with wrap)
		m_write_pos = (m_write_pos + to_write) % m_capacity;
		m_size += to_write;

		return to_write;
	}

	/**
	 * Read data from the ring buffer into destination.
	 * Returns the number of bytes actually read (may be less than requested
	 * if buffer doesn't have enough data).
	 */
	size_t read(uint8_t *dest, size_t length) {
		if (length == 0 || dest == nullptr) { return 0; }

		// Don't read more than available data
		const size_t to_read = std::min(length, m_size);

		if (to_read == 0) { return 0; }

		// Calculate how much we can read before wrapping
		const size_t until_end = m_capacity - m_read_pos;
		const size_t first_chunk = std::min(to_read, until_end);

		// Read first chunk
		std::memcpy(dest, m_buffer.data() + m_read_pos, first_chunk);

		// Read second chunk if we wrap around
		if (first_chunk < to_read) {
			const size_t second_chunk = to_read - first_chunk;
			std::memcpy(dest + first_chunk, m_buffer.data(), second_chunk);
		}

		// Update read position (with wrap)
		m_read_pos = (m_read_pos + to_read) % m_capacity;
		m_size -= to_read;

		return to_read;
	}

	/**
	 * Peek at data without consuming it.
	 * Useful for checking if enough data is available before reading.
	 */
	size_t peek(uint8_t *dest, size_t length) const {
		if (length == 0 || dest == nullptr) { return 0; }

		const size_t to_peek = std::min(length, m_size);
		if (to_peek == 0) { return 0; }

		const size_t until_end = m_capacity - m_read_pos;
		const size_t first_chunk = std::min(to_peek, until_end);

		std::memcpy(dest, m_buffer.data() + m_read_pos, first_chunk);

		if (first_chunk < to_peek) {
			const size_t second_chunk = to_peek - first_chunk;
			std::memcpy(dest + first_chunk, m_buffer.data(), second_chunk);
		}

		return to_peek;
	}

	/**
	 * Discard bytes without copying them.
	 * Useful for skipping data after peeking.
	 */
	void consume(size_t length) {
		const size_t to_consume = std::min(length, m_size);
		m_read_pos = (m_read_pos + to_consume) % m_capacity;
		m_size -= to_consume;
	}

	/**
	 * Clear all data from the buffer.
	 */
	void clear() {
		m_read_pos = 0;
		m_write_pos = 0;
		m_size = 0;
	}

	// Accessors
	[[nodiscard]] size_t size() const { return m_size; }
	[[nodiscard]] size_t capacity() const { return m_capacity; }
	[[nodiscard]] size_t available() const { return m_capacity - m_size; }
	[[nodiscard]] bool empty() const { return m_size == 0; }
	[[nodiscard]] bool full() const { return m_size == m_capacity; }

   private:
	std::vector<uint8_t> m_buffer;
	size_t m_capacity;
	size_t m_read_pos{};
	size_t m_write_pos{};
	size_t m_size{};
};

}  // namespace saber

#endif	// SABER_RING_BUFFER_HPP
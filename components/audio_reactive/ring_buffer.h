#pragma once
#include <cstddef>
#include <algorithm>

namespace esphome {
namespace audio_reactive {

template <typename T, size_t N>
class RingBuffer {
 public:
    size_t write(const T *data, size_t count) {
        size_t space = N - size_;
        size_t to_write = std::min(count, space);
        for (size_t i = 0; i < to_write; i++) {
            buf_[(write_pos_ + i) % N] = data[i];
        }
        write_pos_ = (write_pos_ + to_write) % N;
        size_ += to_write;
        return to_write;
    }

    size_t read(T *out, size_t count) {
        size_t to_read = std::min(count, size_);
        for (size_t i = 0; i < to_read; i++) {
            out[i] = buf_[(read_pos_ + i) % N];
        }
        read_pos_ = (read_pos_ + to_read) % N;
        size_ -= to_read;
        return to_read;
    }

    size_t peek(T *out, size_t count) const {
        size_t to_read = std::min(count, size_);
        for (size_t i = 0; i < to_read; i++) {
            out[i] = buf_[(read_pos_ + i) % N];
        }
        return to_read;
    }

    void advance(size_t count) {
        size_t to_advance = std::min(count, size_);
        read_pos_ = (read_pos_ + to_advance) % N;
        size_ -= to_advance;
    }

    size_t available() const { return size_; }
    size_t capacity() const { return N; }
    void clear() { read_pos_ = 0; write_pos_ = 0; size_ = 0; }

 private:
    T buf_[N]{};
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t size_{0};
};

}  // namespace audio_reactive
}  // namespace esphome

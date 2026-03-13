#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace esphome {
namespace audio_reactive {

/// Automatic gain control via rolling min/max normalization.
class AGC {
 public:
    explicit AGC(size_t window_size)
        : window_size_(window_size) {
        samples_.reserve(window_size);
    }

    void update(float value) {
        if (samples_.size() >= window_size_) {
            samples_.erase(samples_.begin());
        }
        samples_.push_back(value);
        recompute_range();
    }

    float normalize(float value) const {
        if (range_ <= 0.0f) return 0.0f;
        float norm = (value - min_) / range_;
        return std::max(0.0f, std::min(1.0f, norm));
    }

    void reset() {
        samples_.clear();
        min_ = 0.0f;
        max_ = 0.0f;
        range_ = 0.0f;
    }

    float current_min() const { return min_; }
    float current_max() const { return max_; }

 private:
    size_t window_size_;
    std::vector<float> samples_;
    float min_ = 0.0f;
    float max_ = 0.0f;
    float range_ = 0.0f;

    void recompute_range() {
        if (samples_.empty()) return;
        min_ = *std::min_element(samples_.begin(), samples_.end());
        max_ = *std::max_element(samples_.begin(), samples_.end());
        range_ = max_ - min_;
    }
};

}  // namespace audio_reactive
}  // namespace esphome

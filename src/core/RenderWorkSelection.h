#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <algorithm>

namespace lab {
class FrameWorkSet {
    std::vector<uint32_t> active_;
    std::vector<uint32_t> marks_;
    uint32_t generation_{1};
public:
    void Configure(uint32_t capacity) {
        active_.clear();
        active_.reserve(capacity);
        marks_.assign(capacity, 0);
        generation_ = 1;
    }
    void Reset() {
        active_.clear();
        marks_.clear();
        generation_ = 1;
    }
    void BeginFrame() {
        active_.clear();
        if (++generation_ == 0) {
            std::fill(marks_.begin(), marks_.end(), 0);
            generation_ = 1;
        }
    }
    bool Activate(uint32_t index) {
        if (index >= marks_.size()) return false;
        if (marks_[index] == generation_) return true;
        marks_[index] = generation_;
        active_.push_back(index);
        return true;
    }
    bool Empty() const noexcept { return active_.empty(); }
    uint32_t Count() const noexcept { return static_cast<uint32_t>(active_.size()); }
    uint32_t Capacity() const noexcept { return static_cast<uint32_t>(marks_.size()); }
    std::span<const uint32_t> Active() const noexcept { return active_; }
};
}

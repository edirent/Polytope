#pragma once

#include <cstddef>
#include <deque>
#include <span>
#include <vector>

namespace trading_engine::paper {

template <typename T>
class RollingWindow {
public:
    explicit RollingWindow(std::size_t capacity) : capacity_(capacity) {}

    void push(const T& value) {
        if (capacity_ == 0) {
            return;
        }
        if (values_.size() == capacity_) {
            values_.pop_front();
        }
        values_.push_back(value);
    }

    [[nodiscard]] std::vector<T> values() const {
        return {values_.begin(), values_.end()};
    }

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    void clear() noexcept { values_.clear(); }

private:
    std::size_t capacity_ = 0;
    std::deque<T> values_;
};

}  // namespace trading_engine::paper

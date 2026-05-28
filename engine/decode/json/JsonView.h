#pragma once

#include <boost/json.hpp>

namespace trading_engine::decode {

class JsonView {
public:
    JsonView() = default;
    explicit JsonView(const boost::json::value* value) noexcept
        : value_(value) {}

    [[nodiscard]] const boost::json::value* get() const noexcept {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr;
    }

private:
    const boost::json::value* value_{nullptr};
};

}  // namespace trading_engine::decode

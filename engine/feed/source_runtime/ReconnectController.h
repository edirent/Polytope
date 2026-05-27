#pragma once

#include <chrono>
#include <cstdint>

namespace trading_engine::feed {

class ReconnectController {
public:
    void record_failure() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::chrono::milliseconds next_delay() const noexcept;
    [[nodiscard]] std::uint32_t attempts() const noexcept;

private:
    std::uint32_t attempts_{0};
};

}  // namespace trading_engine::feed

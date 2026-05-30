#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::execution {

enum class ReservationDispositionType : std::uint8_t {
    None,
    Release,
    Consume,
    Expire
};

struct ReservationDisposition {
    std::string reservation_id;
    std::uint64_t plan_id = 0;
    ReservationDispositionType type = ReservationDispositionType::None;
    std::string reason;
};

}  // namespace trading_engine::execution

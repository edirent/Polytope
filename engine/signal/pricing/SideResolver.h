#pragma once

#include "oracle/public/CandidateBundle.h"

#include <cstdint>

namespace trading_engine::signal {

enum class ExecutableBookSide : std::uint8_t {
    Bids,
    Asks,
    Unsupported
};

class SideResolver {
public:
    [[nodiscard]] ExecutableBookSide resolve(
        trading_engine::oracle::Side side
    ) const noexcept;
};

}  // namespace trading_engine::signal

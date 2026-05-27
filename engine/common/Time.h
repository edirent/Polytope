#pragma once

#include <chrono>

namespace trading_engine::common {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

}  // namespace trading_engine::common

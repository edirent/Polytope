#pragma once

#include <cstdint>

namespace trading_engine::common {

using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using SequenceNumber = std::uint64_t;

}  // namespace trading_engine::common

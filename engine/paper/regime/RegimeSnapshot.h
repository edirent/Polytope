#pragma once

#include <cstdint>

namespace trading_engine::paper {

enum class DataRegime : std::uint8_t {
    Unknown,
    Healthy,
    Stale
};

enum class LiquidityRegime : std::uint8_t {
    Unknown,
    Healthy,
    Degraded,
    Crossed
};

enum class ChainRegime : std::uint8_t {
    Unknown,
    Healthy,
    Lagging,
    Error
};

enum class SignalRegime : std::uint8_t {
    Unknown,
    Healthy,
    Quiet,
    Rejecting
};

enum class RiskRegime : std::uint8_t {
    Unknown,
    Healthy,
    Constrained,
    KillSwitch
};

enum class ExecutionRegime : std::uint8_t {
    Unknown,
    Healthy,
    PartialFill,
    HedgeRequired
};

struct RegimeSnapshot {
    DataRegime data = DataRegime::Unknown;
    LiquidityRegime liquidity = LiquidityRegime::Unknown;
    ChainRegime chain = ChainRegime::Unknown;
    SignalRegime signal = SignalRegime::Unknown;
    RiskRegime risk = RiskRegime::Unknown;
    ExecutionRegime execution = ExecutionRegime::Unknown;

    std::uint64_t version = 0;
    std::uint64_t ts_ns = 0;
};

}  // namespace trading_engine::paper

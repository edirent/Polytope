#pragma once

#include <cstdint>

namespace trading_engine::risk {

struct RiskConfig {
    bool enabled = true;
    bool emit_rejections = true;

    std::uint32_t max_decisions_per_scan = 1024;
};

}  // namespace trading_engine::risk

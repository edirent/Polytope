#pragma once

#include <cstdint>

namespace trading_engine::risk {

struct RiskConfig {
    bool enabled = true;
    bool emit_rejections = true;
    bool enable_full_audit_trace = false;
    bool emit_audit_strings = false;

    std::uint32_t max_decisions_per_scan = 1024;
};

}  // namespace trading_engine::risk

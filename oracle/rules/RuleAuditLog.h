#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct RuleAuditEntry {
    std::string rule_id;
    std::string action;
    std::string actor;
    std::uint64_t at_ns = 0;
    std::string reason;
};

struct RuleAuditLog {
    std::vector<RuleAuditEntry> entries;
};

}  // namespace trading_engine::oracle

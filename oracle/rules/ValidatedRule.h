#pragma once

#include "oracle/rules/RuleDraft.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct ValidatedRule {
    std::string rule_id;
    RuleType type = RuleType::Custom;

    std::vector<std::string> variable_ids;

    bool approved = false;
    std::string approved_by;
    std::uint64_t approved_at_ns = 0;

    std::string source_rule_draft_id;
};

}  // namespace trading_engine::oracle

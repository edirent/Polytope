#pragma once

#include "oracle/rules/Rulebook.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

class ManualRuleEditor {
public:
    [[nodiscard]] RulebookLoadResult load_rulebook(
        const std::string& path
    ) const;

    [[nodiscard]] bool write_approved_rulebook(
        const Rulebook& rulebook,
        const std::string& path,
        std::vector<std::string>* errors = nullptr
    ) const;

    [[nodiscard]] std::vector<ValidatedRule> list_unapproved_rules(
        const Rulebook& rulebook
    ) const;
};

}  // namespace trading_engine::oracle

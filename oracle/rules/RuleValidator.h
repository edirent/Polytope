#pragma once

#include "oracle/rules/Rulebook.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::oracle {

struct RulebookValidationResult {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<std::string> unapproved_rules;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }

    [[nodiscard]] bool compiler_ready() const noexcept {
        return errors.empty() && unapproved_rules.empty();
    }
};

class RuleValidator {
public:
    [[nodiscard]] RulebookValidationResult validate_rulebook(
        const Rulebook& rulebook,
        const std::unordered_set<std::string>& known_variables = {}
    ) const;
};

}  // namespace trading_engine::oracle

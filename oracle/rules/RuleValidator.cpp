#include "oracle/rules/RuleValidator.h"

#include <unordered_set>

namespace trading_engine::oracle {

RulebookValidationResult RuleValidator::validate_rulebook(
    const Rulebook& rulebook,
    const std::unordered_set<std::string>& known_variables
) const {
    RulebookValidationResult result;
    std::unordered_set<std::string> seen_rule_ids;

    for (const auto& rule : rulebook.rules()) {
        if (rule.rule_id.empty()) {
            result.errors.push_back("rule missing rule_id");
        } else {
            const auto [_, inserted] = seen_rule_ids.insert(rule.rule_id);
            if (!inserted) {
                result.errors.push_back(
                    "duplicate rule_id: " + rule.rule_id
                );
            }
        }

        if (rule.variable_ids.empty()) {
            result.errors.push_back(
                "rule " + rule.rule_id + " has empty variable set"
            );
        }

        for (const auto& variable_id : rule.variable_ids) {
            if (variable_id.empty()) {
                result.errors.push_back(
                    "rule " + rule.rule_id + " has empty variable_id"
                );
                continue;
            }

            if (!known_variables.empty() &&
                known_variables.find(variable_id) == known_variables.end()) {
                result.errors.push_back(
                    "rule " + rule.rule_id +
                    " references unknown variable: " + variable_id
                );
            }
        }

        if (!rule.approved) {
            result.unapproved_rules.push_back(rule.rule_id);
        }
    }

    return result;
}

}  // namespace trading_engine::oracle

#pragma once

#include "oracle/rules/ValidatedRule.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

struct RulebookLoadResult {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<ValidatedRule> rules;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class Rulebook {
public:
    void add_rule(const ValidatedRule& rule);
    void clear();

    [[nodiscard]] const std::vector<ValidatedRule>& rules() const noexcept;
    [[nodiscard]] std::vector<ValidatedRule> approved_rules() const;
    [[nodiscard]] std::vector<ValidatedRule> unapproved_rules() const;

    [[nodiscard]] static RulebookLoadResult load_json(
        const std::string& path
    );
    [[nodiscard]] bool save_json(
        const std::string& path,
        std::vector<std::string>* errors = nullptr
    ) const;

private:
    std::vector<ValidatedRule> rules_;
};

}  // namespace trading_engine::oracle

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

enum class RuleType : std::uint8_t {
    MutuallyExclusive,
    CollectivelyExhaustive,
    AtMostOne,
    AtLeastOne,
    ExactlyOne,
    Implies,
    Equivalent,
    Disjoint,
    Custom
};

enum class RuleCoverage : std::uint8_t {
    ExclusiveOnly,
    ExhaustiveAndExclusive
};

struct RuleDraft {
    std::string rule_id;
    RuleType type = RuleType::Custom;
    RuleCoverage coverage = RuleCoverage::ExclusiveOnly;

    std::vector<std::string> variable_ids;

    std::string source_text;
    std::string rationale;

    bool requires_manual_review = true;
};

[[nodiscard]] const char* rule_type_to_string(RuleType type) noexcept;
[[nodiscard]] bool rule_type_from_string(
    const std::string& value,
    RuleType* out
) noexcept;

[[nodiscard]] const char* rule_coverage_to_string(
    RuleCoverage coverage
) noexcept;
[[nodiscard]] bool rule_coverage_from_string(
    const std::string& value,
    RuleCoverage* out
) noexcept;
[[nodiscard]] bool rule_is_exhaustive(RuleCoverage coverage) noexcept;

}  // namespace trading_engine::oracle

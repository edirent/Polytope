#include "oracle/bundles/CombinatorialBundleGenerator.h"

#include "oracle/bundles/BundleHash.h"
#include "oracle/bundles/BundleValidator.h"
#include "oracle/payoff/PayoutRule.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trading_engine::oracle {
namespace {

struct VariableBinding {
    std::string variable_id;
    std::string market_id;
    std::string outcome_id;
    std::string asset_id;
};

[[nodiscard]] std::string lower_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

[[nodiscard]] bool is_yes_no_outcome(std::string_view outcome) {
    const auto lower = lower_copy(std::string{outcome});
    return lower == "yes" || lower == "no";
}

[[nodiscard]] std::string opposite_outcome(std::string_view outcome) {
    const auto lower = lower_copy(std::string{outcome});
    if (lower == "yes") {
        return "no";
    }
    if (lower == "no") {
        return "yes";
    }
    return {};
}

struct BindingIndex {
    std::unordered_map<std::string, VariableBinding> by_variable;
    std::unordered_map<std::string, std::unordered_map<std::string, VariableBinding>>
        by_market_outcome_lower;
    std::unordered_set<std::string> known_market_ids;
    std::unordered_set<std::string> known_asset_ids;
};

[[nodiscard]] BindingIndex build_index(
    const std::vector<RawMarketRecord>& markets
) {
    BindingIndex index;
    for (const auto& market : markets) {
        if (!market.market_id.empty()) {
            index.known_market_ids.insert(market.market_id);
        }
        const auto count = std::min(market.outcomes.size(), market.asset_ids.size());
        for (std::size_t i = 0; i < count; ++i) {
            const VariableBinding binding{
                .variable_id = market.market_id + ":" + market.outcomes[i],
                .market_id = market.market_id,
                .outcome_id = market.outcomes[i],
                .asset_id = market.asset_ids[i]
            };
            index.known_asset_ids.insert(binding.asset_id);
            index.by_variable.emplace(binding.variable_id, binding);
            index.by_market_outcome_lower[binding.market_id].emplace(
                lower_copy(binding.outcome_id),
                binding
            );
        }
    }
    return index;
}

[[nodiscard]] std::vector<VariableBinding> resolve_variables(
    const ValidatedRule& rule,
    const BindingIndex& index,
    CandidateBundleLoadResult* result
) {
    std::vector<VariableBinding> bindings;
    for (const auto& variable_id : rule.variable_ids) {
        const auto it = index.by_variable.find(variable_id);
        if (it == index.by_variable.end()) {
            result->errors.push_back(
                "rule " + rule.rule_id + ": unknown variable_id " + variable_id
            );
            continue;
        }
        bindings.push_back(it->second);
    }
    return bindings;
}

[[nodiscard]] std::uint32_t distinct_market_count(
    const std::vector<VariableBinding>& bindings
) {
    std::unordered_set<std::string> markets;
    for (const auto& binding : bindings) {
        markets.insert(binding.market_id);
    }
    return static_cast<std::uint32_t>(markets.size());
}

[[nodiscard]] bool has_too_many_legs(
    const ValidatedRule& rule,
    std::size_t count,
    CandidateBundleLoadResult* result
) {
    if (count <= kMaxBundleLegs) {
        return false;
    }
    result->warnings.push_back(
        "rule " + rule.rule_id + ": skipped because leg_count exceeds " +
        std::to_string(kMaxBundleLegs)
    );
    return true;
}

[[nodiscard]] bool is_trivial_single_market_rule(
    const ValidatedRule& rule,
    const std::vector<VariableBinding>& bindings,
    CandidateBundleLoadResult* result
) {
    if (distinct_market_count(bindings) > 1) {
        return false;
    }
    result->warnings.push_back(
        "rule " + rule.rule_id + ": skipped trivial single-market rule"
    );
    return true;
}

[[nodiscard]] CandidateBundle make_bundle(
    std::uint64_t bundle_id,
    const std::vector<VariableBinding>& bindings,
    std::int64_t guaranteed_payout_tick
) {
    CandidateBundle bundle;
    bundle.bundle_id = bundle_id;
    bundle.guaranteed_payout_tick = guaranteed_payout_tick;
    bundle.min_edge_tick = 0;
    bundle.required_true_mask = 0;
    bundle.required_false_mask = 0;
    bundle.invalid_mask = 0;
    bundle.leg_count = static_cast<std::uint16_t>(bindings.size());

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        bundle.legs[i].market_id = bindings[i].market_id;
        bundle.legs[i].asset_id = bindings[i].asset_id;
        bundle.legs[i].side = Side::Buy;
        bundle.legs[i].quantity_lots = 1;
        bundle.legs[i].max_price_tick = PAYOUT_ONE_TICK;
    }
    return bundle;
}

[[nodiscard]] std::vector<VariableBinding> complement_bindings(
    const ValidatedRule& rule,
    const std::vector<VariableBinding>& bindings,
    const BindingIndex& index,
    CandidateBundleLoadResult* result
) {
    std::vector<VariableBinding> complements;
    for (const auto& binding : bindings) {
        if (!is_yes_no_outcome(binding.outcome_id)) {
            result->warnings.push_back(
                "rule " + rule.rule_id +
                ": skipped complement basket for non-binary outcome " +
                binding.variable_id
            );
            return {};
        }

        const auto market_it =
            index.by_market_outcome_lower.find(binding.market_id);
        if (market_it == index.by_market_outcome_lower.end()) {
            result->warnings.push_back(
                "rule " + rule.rule_id +
                ": skipped complement basket because market is missing " +
                binding.market_id
            );
            return {};
        }

        const auto opposite = opposite_outcome(binding.outcome_id);
        const auto opposite_it = market_it->second.find(opposite);
        if (opposite_it == market_it->second.end()) {
            result->warnings.push_back(
                "rule " + rule.rule_id +
                ": skipped complement basket because opposite outcome is missing for " +
                binding.variable_id
            );
            return {};
        }
        complements.push_back(opposite_it->second);
    }
    return complements;
}

void append_exactly_one_bundles(
    const ValidatedRule& rule,
    const std::vector<VariableBinding>& bindings,
    const BindingIndex& index,
    std::uint64_t* next_bundle_id,
    CandidateBundleLoadResult* result
) {
    if (has_too_many_legs(rule, bindings.size(), result) ||
        is_trivial_single_market_rule(rule, bindings, result)) {
        return;
    }

    result->bundles.push_back(make_bundle(
        (*next_bundle_id)++,
        bindings,
        PAYOUT_ONE_TICK
    ));

    auto complements = complement_bindings(rule, bindings, index, result);
    if (complements.empty()) {
        return;
    }
    result->bundles.push_back(make_bundle(
        (*next_bundle_id)++,
        complements,
        static_cast<std::int64_t>(complements.size() - 1) * PAYOUT_ONE_TICK
    ));
}

void append_at_most_one_bundles(
    const ValidatedRule& rule,
    const std::vector<VariableBinding>& bindings,
    const BindingIndex& index,
    std::uint64_t* next_bundle_id,
    CandidateBundleLoadResult* result
) {
    if (has_too_many_legs(rule, bindings.size(), result) ||
        is_trivial_single_market_rule(rule, bindings, result)) {
        return;
    }

    auto complements = complement_bindings(rule, bindings, index, result);
    if (complements.empty()) {
        return;
    }
    result->bundles.push_back(make_bundle(
        (*next_bundle_id)++,
        complements,
        static_cast<std::int64_t>(complements.size() - 1) * PAYOUT_ONE_TICK
    ));
}

}  // namespace

CandidateBundleLoadResult CombinatorialBundleGenerator::generate_from_rulebook(
    const std::vector<RawMarketRecord>& markets,
    const Rulebook& rulebook
) const {
    CandidateBundleLoadResult result;
    const auto index = build_index(markets);
    std::uint64_t next_bundle_id = 1;

    for (const auto& rule : rulebook.rules()) {
        if (!rule.approved) {
            result.errors.push_back(
                "rule " + rule.rule_id + ": unapproved rule cannot generate bundles"
            );
            continue;
        }

        const auto bindings = resolve_variables(rule, index, &result);
        if (bindings.size() != rule.variable_ids.size()) {
            continue;
        }
        if (bindings.size() < 2) {
            result.warnings.push_back(
                "rule " + rule.rule_id + ": skipped because it has fewer than 2 variables"
            );
            continue;
        }

        switch (rule.type) {
            case RuleType::ExactlyOne:
                append_exactly_one_bundles(
                    rule,
                    bindings,
                    index,
                    &next_bundle_id,
                    &result
                );
                break;
            case RuleType::AtMostOne:
                append_at_most_one_bundles(
                    rule,
                    bindings,
                    index,
                    &next_bundle_id,
                    &result
                );
                break;
            default:
                result.warnings.push_back(
                    "rule " + rule.rule_id +
                    ": skipped unsupported rule type for combinatorial bundles"
                );
                break;
        }
    }

    if (!result.errors.empty()) {
        return result;
    }

    BundleValidator validator;
    const auto validation = validator.validate(
        result.bundles,
        index.known_market_ids,
        index.known_asset_ids
    );
    result.errors.insert(
        result.errors.end(),
        validation.errors.begin(),
        validation.errors.end()
    );
    if (!result.errors.empty()) {
        return result;
    }

    result.bundle_hash = hash_candidate_bundles(result.bundles);
    return result;
}

}  // namespace trading_engine::oracle

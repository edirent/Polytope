#include "oracle/compiler/MarketIntrinsicConstraintBuilder.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace trading_engine::oracle {
namespace {

[[nodiscard]] std::string lower_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

[[nodiscard]] bool is_yes_no_pair(
    const std::vector<std::string>& outcomes,
    std::size_t* yes_index,
    std::size_t* no_index
) {
    if (outcomes.size() != 2 || !yes_index || !no_index) {
        return false;
    }

    bool saw_yes = false;
    bool saw_no = false;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        const auto lower = lower_copy(outcomes[i]);
        if (lower == "yes") {
            saw_yes = true;
            *yes_index = i;
        } else if (lower == "no") {
            saw_no = true;
            *no_index = i;
        }
    }
    return saw_yes && saw_no;
}

[[nodiscard]] std::unordered_map<std::string, std::uint32_t> variable_index(
    const BooleanVariableRegistry& variables
) {
    std::unordered_map<std::string, std::uint32_t> out;
    for (const auto& variable : variables) {
        out.emplace(variable.variable_key, variable.var_id);
    }
    return out;
}

}  // namespace

std::vector<LinearBooleanConstraint> MarketIntrinsicConstraintBuilder::build(
    const MarketUniverse& universe,
    const BooleanVariableRegistry& variables
) const {
    std::vector<LinearBooleanConstraint> constraints;
    const auto index = variable_index(variables);

    std::vector<const RawMarketRecord*> markets;
    markets.reserve(universe.markets.size());
    for (const auto& market : universe.markets) {
        markets.push_back(&market);
    }
    std::sort(
        markets.begin(),
        markets.end(),
        [](const RawMarketRecord* lhs, const RawMarketRecord* rhs) {
            return lhs->market_id < rhs->market_id;
        }
    );

    for (const auto* market : markets) {
        if (!market || market->outcomes.size() != market->asset_ids.size()) {
            continue;
        }

        std::size_t yes_index = 0;
        std::size_t no_index = 0;
        if (!is_yes_no_pair(market->outcomes, &yes_index, &no_index)) {
            continue;
        }

        const auto yes_key = market->market_id + ":" + market->outcomes[yes_index];
        const auto no_key = market->market_id + ":" + market->outcomes[no_index];
        const auto yes_it = index.find(yes_key);
        const auto no_it = index.find(no_key);
        if (yes_it == index.end() || no_it == index.end()) {
            continue;
        }

        LinearBooleanConstraint constraint;
        constraint.var_ids = {yes_it->second, no_it->second};
        constraint.coeffs = {1, 1};
        constraint.op = ConstraintOp::Equal;
        constraint.rhs = 1;
        constraints.push_back(std::move(constraint));
    }

    return constraints;
}

}  // namespace trading_engine::oracle

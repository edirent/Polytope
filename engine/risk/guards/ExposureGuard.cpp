#include "engine/risk/guards/ExposureGuard.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] ExposureGuardResult reject(
    RiskDecisionType type,
    std::string reason,
    ExposureGuardResult result
) {
    result.pass = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

[[nodiscard]] std::unordered_map<std::string, std::int64_t> market_costs(
    const signal::OpportunityIntent& intent,
    const CostRevalidationResult& cost
) {
    std::unordered_map<std::string, std::int64_t> out;

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.market_id.empty()) {
            continue;
        }
        out[leg.market_id] += std::max<std::int64_t>(
            leg.estimated_cost_tick,
            0
        );
    }

    if (out.size() == 1) {
        out.begin()->second = std::max<std::int64_t>(
            cost.risk_total_cost_tick,
            0
        );
    }

    return out;
}

}  // namespace

ExposureGuardResult ExposureGuard::check(
    const RiskLedgerSnapshot& ledger,
    const signal::OpportunityIntent& intent,
    const CostRevalidationResult& cost,
    const RiskPolicySnapshot& policy
) const {
    ExposureGuardResult result;

    if (!cost.ok) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid cost revalidation result",
            result
        );
    }

    result.post_total_exposure_tick =
        ledger.total_reserved_exposure_tick +
        std::max<std::int64_t>(cost.risk_total_cost_tick, 0);

    if (policy.max_total_exposure_tick > 0 &&
        result.post_total_exposure_tick >
            policy.max_total_exposure_tick) {
        return reject(
            RiskDecisionType::RejectTotalExposureLimit,
            "total exposure limit exceeded",
            result
        );
    }

    if (policy.max_single_market_exposure_tick > 0) {
        for (const auto& [market_id, market_cost] :
             market_costs(intent, cost)) {
            auto current = std::int64_t{0};
            if (const auto it =
                    ledger.reserved_market_exposure_tick.find(market_id);
                it != ledger.reserved_market_exposure_tick.end()) {
                current = it->second;
            }

            const auto post_market_exposure = current + market_cost;
            if (post_market_exposure >
                policy.max_single_market_exposure_tick) {
                result.rejected_market_id = market_id;
                result.post_market_exposure_tick = post_market_exposure;
                return reject(
                    RiskDecisionType::RejectSingleMarketExposureLimit,
                    "single market exposure limit exceeded",
                    result
                );
            }
        }
    }

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk

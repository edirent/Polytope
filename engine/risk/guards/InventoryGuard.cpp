#include "engine/risk/guards/InventoryGuard.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] InventoryGuardResult reject(
    RiskDecisionType type,
    std::string reason,
    InventoryGuardResult result
) {
    result.pass = false;
    result.rejection = type;
    result.reason = std::move(reason);
    return result;
}

}  // namespace

InventoryGuardResult InventoryGuard::check(
    const RiskLedgerSnapshot& ledger,
    const signal::OpportunityIntent& intent,
    const CostRevalidationResult& cost,
    const RiskPolicySnapshot& policy
) const {
    InventoryGuardResult result;

    if (!cost.ok) {
        return reject(
            RiskDecisionType::RejectInternalError,
            "invalid cost revalidation result",
            result
        );
    }

    if (policy.max_inventory_lots_per_asset <= 0) {
        result.pass = true;
        result.rejection = RiskDecisionType::Approve;
        return result;
    }

    std::unordered_map<std::string, std::int64_t> incoming_lots;
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        if (leg.asset_id.empty()) {
            continue;
        }
        incoming_lots[leg.asset_id] += std::max<std::int64_t>(
            leg.quantity_lots,
            0
        );
    }

    for (const auto& [asset_id, lots] : incoming_lots) {
        auto current = std::int64_t{0};
        if (const auto it = ledger.reserved_asset_lots.find(asset_id);
            it != ledger.reserved_asset_lots.end()) {
            current = it->second;
        }

        const auto post_asset_lots = current + lots;
        if (post_asset_lots > policy.max_inventory_lots_per_asset) {
            result.rejected_asset_id = asset_id;
            result.post_asset_lots = post_asset_lots;
            return reject(
                RiskDecisionType::RejectInventoryLimit,
                "asset inventory limit exceeded",
                result
            );
        }
    }

    result.pass = true;
    result.rejection = RiskDecisionType::Approve;
    return result;
}

}  // namespace trading_engine::risk

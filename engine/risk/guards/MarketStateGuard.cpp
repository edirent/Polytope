#include "engine/risk/guards/MarketStateGuard.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] GuardResult reject(std::string reason) {
    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectBadMarketState;
    result.reject_flag = kRiskRejectFlagBadMarketState;
    result.reason = std::move(reason);
    return result;
}

}  // namespace

GuardResult MarketStateGuard::check(
    const state::MarketStateSnapshot* snapshot
) const {
    if (snapshot == nullptr || snapshot->entity_id.empty()) {
        return reject("snapshot missing");
    }
    if (!snapshot->live) {
        return reject("snapshot not live or market halted");
    }
    if (!snapshot->usable_for_depth) {
        return reject("snapshot not usable for depth");
    }
    if (snapshot->recovering) {
        return reject("snapshot recovering");
    }
    if (snapshot->crossed) {
        return reject("snapshot crossed");
    }
    if (snapshot->closed) {
        return reject("snapshot closed");
    }
    if (snapshot->resolved) {
        return reject("snapshot resolved");
    }

    return pass_guard();
}

GuardResult MarketStateGuard::check(
    const state::MarketDepthView* depth_view
) const {
    if (depth_view == nullptr) {
        return reject("depth view missing");
    }
    if (!depth_view->usable_for_depth) {
        return reject("depth view not usable for depth");
    }
    if (depth_view->recovering) {
        return reject("depth view recovering");
    }
    if (depth_view->crossed) {
        return reject("depth view crossed");
    }
    if (depth_view->closed) {
        return reject("depth view closed");
    }
    if (depth_view->resolved) {
        return reject("depth view resolved");
    }

    return pass_guard();
}

GuardResult MarketStateGuard::check(std::nullptr_t) const {
    return reject("snapshot missing");
}

}  // namespace trading_engine::risk

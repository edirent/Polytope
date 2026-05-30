#include "state/core/MarketStateEventFilter.h"

#include <unordered_set>
#include <utility>

namespace trading_engine::state {

namespace {

[[nodiscard]] std::string asset_id_for_event(
    const MarketStateEvent& event
) {
    if (!event.asset_id.empty()) {
        return event.asset_id;
    }
    if (!event.ws_event.asset_id.empty()) {
        return event.ws_event.asset_id;
    }
    if (!event.ws_event.entity_id.empty()) {
        return event.ws_event.entity_id;
    }
    return event.chain_fill.asset_id;
}

[[nodiscard]] std::string market_id_for_event(
    const MarketStateEvent& event
) {
    if (!event.market_id.empty()) {
        return event.market_id;
    }
    if (!event.ws_event.market_id.empty()) {
        return event.ws_event.market_id;
    }
    return event.chain_fill.market_id;
}

[[nodiscard]] bool contains(
    const std::unordered_set<std::string>& values,
    const std::string& value
) {
    return !value.empty() && values.find(value) != values.end();
}

[[nodiscard]] MarketStateEventFilterResult pass(
    std::string asset_id,
    std::string market_id
) {
    MarketStateEventFilterResult result;
    result.decision = MarketStateEventFilterDecision::Pass;
    result.reason = MarketStateEventFilterReason::None;
    result.asset_id = std::move(asset_id);
    result.market_id = std::move(market_id);
    return result;
}

[[nodiscard]] MarketStateEventFilterResult filtered(
    MarketStateEventFilterReason reason,
    std::string asset_id,
    std::string market_id
) {
    MarketStateEventFilterResult result;
    result.decision = MarketStateEventFilterDecision::Filter;
    result.reason = reason;
    result.asset_id = std::move(asset_id);
    result.market_id = std::move(market_id);
    return result;
}

}  // namespace

MarketStateEventFilter::MarketStateEventFilter(const StateUniverse& universe)
    : universe_(&universe) {}

MarketStateEventFilterResult MarketStateEventFilter::filter(
    const MarketStateEvent& event
) const {
    const std::string asset_id = asset_id_for_event(event);
    const std::string market_id = market_id_for_event(event);

    if (universe_ == nullptr) {
        return pass(asset_id, market_id);
    }

    switch (event.type) {
        case MarketStateEventType::WsHeartbeat:
            return universe_->allow_heartbeats
                ? pass(asset_id, market_id)
                : filtered(
                    MarketStateEventFilterReason::AssetNotInUniverse,
                    asset_id,
                    market_id
                );

        case MarketStateEventType::WsLifecycle:
            if (market_id.empty()) {
                return filtered(
                    MarketStateEventFilterReason::MissingMarketId,
                    asset_id,
                    market_id
                );
            }
            if (universe_->allow_market_lifecycle_for_active_markets &&
                contains(universe_->active_market_ids, market_id)) {
                return pass(asset_id, market_id);
            }
            return filtered(
                MarketStateEventFilterReason::MarketNotInUniverse,
                asset_id,
                market_id
            );

        case MarketStateEventType::WsBookSnapshot:
        case MarketStateEventType::WsBookDelta:
            if (asset_id.empty()) {
                return filtered(
                    MarketStateEventFilterReason::MissingAssetId,
                    asset_id,
                    market_id
                );
            }
            if (contains(universe_->active_asset_ids, asset_id)) {
                return pass(asset_id, market_id);
            }
            if (contains(universe_->active_market_ids, market_id)) {
                return filtered(
                    MarketStateEventFilterReason::PairedAssetNotInUniverse,
                    asset_id,
                    market_id
                );
            }
            return filtered(
                MarketStateEventFilterReason::AssetNotInUniverse,
                asset_id,
                market_id
            );

        case MarketStateEventType::ChainConfirmedFill:
        case MarketStateEventType::ChainRemovedFill:
            if (asset_id.empty()) {
                return filtered(
                    MarketStateEventFilterReason::MissingAssetId,
                    asset_id,
                    market_id
                );
            }
            if (contains(universe_->active_asset_ids, asset_id)) {
                return pass(asset_id, market_id);
            }
            return filtered(
                MarketStateEventFilterReason::AssetNotInUniverse,
                asset_id,
                market_id
            );

        case MarketStateEventType::ChainSettlement:
            if (market_id.empty()) {
                return filtered(
                    MarketStateEventFilterReason::MissingMarketId,
                    asset_id,
                    market_id
                );
            }
            if (contains(universe_->active_market_ids, market_id)) {
                return pass(asset_id, market_id);
            }
            return filtered(
                MarketStateEventFilterReason::MarketNotInUniverse,
                asset_id,
                market_id
            );

        case MarketStateEventType::DataQualityUpdate:
        default:
            return pass(asset_id, market_id);
    }
}

const char* to_string(
    MarketStateEventFilterDecision decision
) noexcept {
    switch (decision) {
        case MarketStateEventFilterDecision::Pass:
            return "Pass";
        case MarketStateEventFilterDecision::Filter:
            return "Filter";
    }
    return "Filter";
}

const char* to_string(
    MarketStateEventFilterReason reason
) noexcept {
    switch (reason) {
        case MarketStateEventFilterReason::None:
            return "None";
        case MarketStateEventFilterReason::PairedAssetNotInUniverse:
            return "PairedAssetNotInUniverse";
        case MarketStateEventFilterReason::AssetNotInUniverse:
            return "AssetNotInUniverse";
        case MarketStateEventFilterReason::MarketNotInUniverse:
            return "MarketNotInUniverse";
        case MarketStateEventFilterReason::MissingAssetId:
            return "MissingAssetId";
        case MarketStateEventFilterReason::MissingMarketId:
            return "MissingMarketId";
    }
    return "None";
}

}  // namespace trading_engine::state

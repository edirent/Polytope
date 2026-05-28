#include "state/core/MarketStateEventAdapter.h"

namespace trading_engine::state {

namespace {

using decode::NormalizedEvent;
using decode::NormalizedEventType;

[[nodiscard]] MarketStateEventType ws_event_type(
    NormalizedEventType event_type
) noexcept {
    switch (event_type) {
        case NormalizedEventType::Snapshot:
            return MarketStateEventType::WsBookSnapshot;
        case NormalizedEventType::Delta:
            return MarketStateEventType::WsBookDelta;
        case NormalizedEventType::Heartbeat:
            return MarketStateEventType::WsHeartbeat;
        case NormalizedEventType::LifecycleEvent:
            return MarketStateEventType::WsLifecycle;
        case NormalizedEventType::StatusChange:
        case NormalizedEventType::TradeEvent:
        case NormalizedEventType::DecodeError:
        case NormalizedEventType::Unknown:
        default:
            return MarketStateEventType::DataQualityUpdate;
    }
}

[[nodiscard]] MarketStateEvent from_normalized_event(
    const NormalizedEvent& event
) {
    MarketStateEvent out;
    out.type = ws_event_type(event.event_type);
    out.market_id = event.market_id;
    out.asset_id = event.asset_id;
    out.recv_monotonic_ns = event.recv_monotonic_ns;
    out.source_sequence = event.packet_id;
    out.ws_event = event;
    return out;
}

}  // namespace

std::vector<MarketStateEvent> from_normalized_batch(
    const decode::NormalizedEventBatch& batch
) {
    std::vector<MarketStateEvent> events;
    events.reserve(batch.events.size());

    for (const auto& event : batch.events) {
        events.push_back(from_normalized_event(event));
    }

    return events;
}

MarketStateEvent from_classified_fill(
    const chain_confirm::ClassifiedFillRecord& fill
) {
    MarketStateEvent out;
    if (fill.removed ||
        fill.classification ==
            chain_confirm::FillClassification::ChainRemoved) {
        out.type = MarketStateEventType::ChainRemovedFill;
    } else if (
        fill.classification ==
        chain_confirm::FillClassification::ChainConfirmed
    ) {
        out.type = MarketStateEventType::ChainConfirmedFill;
    } else {
        out.type = MarketStateEventType::DataQualityUpdate;
    }
    out.market_id = fill.market_id;
    out.asset_id = fill.asset_id;
    out.recv_monotonic_ns = fill.chain_seen_monotonic_ns;
    out.source_sequence = fill.source_sequence;
    out.chain_fill = fill;
    return out;
}

}  // namespace trading_engine::state

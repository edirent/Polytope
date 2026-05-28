#include "state/book/LOBWriter.h"

#include <utility>

namespace trading_engine::state {

namespace {

[[nodiscard]] BookApplyCode map_state_code(
    StateApplyCode code
) noexcept {
    if (code == StateApplyCode::IgnoredHeartbeat) {
        return BookApplyCode::IgnoredHeartbeat;
    }

    switch (code) {
        case StateApplyCode::Applied:
        case StateApplyCode::ClosedEntityIgnored:
        case StateApplyCode::IgnoredUnknown:
        case StateApplyCode::IgnoredTrade:
        case StateApplyCode::Noop:
            return BookApplyCode::Applied;

        case StateApplyCode::IgnoredHeartbeat:
            return BookApplyCode::IgnoredHeartbeat;

        case StateApplyCode::MissingEntityId:
        case StateApplyCode::DeltaBeforeSnapshot:
        case StateApplyCode::UnknownSide:
        case StateApplyCode::InvalidValue:
        default:
            return BookApplyCode::StateRejected;
    }
}

[[nodiscard]] BookApplyResult from_state_result(
    MarketStateEventType event_type,
    StateApplyResult state_result
) {
    BookApplyResult out;
    out.code = map_state_code(state_result.code);
    out.event_type = event_type;
    out.state_code = state_result.code;
    out.entity_id = std::move(state_result.entity_id);
    out.message = std::move(state_result.message);
    out.state_changed = state_result.state_changed;
    out.entity_hash = state_result.entity_hash;
    out.global_hash = state_result.global_hash;
    return out;
}

[[nodiscard]] BookApplyResult ignored_result(
    BookApplyCode code,
    MarketStateEventType event_type,
    std::string message,
    const EntityStateStore* store
) {
    BookApplyResult out;
    out.code = code;
    out.event_type = event_type;
    out.message = std::move(message);
    out.global_hash = store ? store->global_hash() : 0;
    return out;
}

}  // namespace

LOBWriter::LOBWriter(EntityStateStore* store)
    : store_(store) {}

BookApplyResult LOBWriter::apply(const MarketStateEvent& event) {
    if (!store_) {
        BookApplyResult out;
        out.code = BookApplyCode::MissingStore;
        out.event_type = event.type;
        out.message = "LOBWriter has no EntityStateStore";
        return out;
    }

    switch (event.type) {
        case MarketStateEventType::WsBookSnapshot:
        case MarketStateEventType::WsBookDelta:
        case MarketStateEventType::WsLifecycle:
        case MarketStateEventType::WsHeartbeat:
            return from_state_result(
                event.type,
                store_->apply(event.ws_event)
            );

        case MarketStateEventType::ChainConfirmedFill:
        case MarketStateEventType::ChainRemovedFill:
        case MarketStateEventType::ChainSettlement:
            return ignored_result(
                BookApplyCode::IgnoredChainEvent,
                event.type,
                "chain event ignored by LOBWriter",
                store_
            );

        case MarketStateEventType::DataQualityUpdate:
        default:
            return ignored_result(
                BookApplyCode::IgnoredDataQualityUpdate,
                event.type,
                "data quality event ignored by LOBWriter",
                store_
            );
    }
}

BookBatchApplyResult LOBWriter::apply_batch(
    std::span<const MarketStateEvent> events
) {
    BookBatchApplyResult out;
    out.events_seen = events.size();
    out.results.reserve(events.size());

    for (const auto& event : events) {
        BookApplyResult result = apply(event);

        if (result.ok()) {
            if (result.ignored()) {
                ++out.ignored;
            } else {
                ++out.applied;
            }
        } else {
            ++out.errors;
        }

        if (result.state_changed) {
            ++out.state_changed;
        }

        out.global_hash = result.global_hash;
        out.results.push_back(std::move(result));
    }

    return out;
}

std::string to_string(BookApplyCode code) {
    switch (code) {
        case BookApplyCode::Applied:
            return "Applied";
        case BookApplyCode::IgnoredHeartbeat:
            return "IgnoredHeartbeat";
        case BookApplyCode::IgnoredChainEvent:
            return "IgnoredChainEvent";
        case BookApplyCode::IgnoredDataQualityUpdate:
            return "IgnoredDataQualityUpdate";
        case BookApplyCode::MissingStore:
            return "MissingStore";
        case BookApplyCode::StateRejected:
        default:
            return "StateRejected";
    }
}

}  // namespace trading_engine::state

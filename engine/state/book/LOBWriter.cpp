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
    out.mutation = state_result.mutation;
    out.book_version = state_result.book_version;
    out.chain_version = state_result.chain_version;
    out.quality_version = state_result.quality_version;
    out.snapshot_version_hash = state_result.snapshot_version_hash;
    out.entity_hash = state_result.entity_hash;
    out.global_hash = state_result.global_hash;
    out.cheap_fingerprint = state_result.cheap_fingerprint;
    out.full_hash_computed = state_result.full_hash_computed;
    out.make_result_ns = state_result.make_result_ns;
    out.hash_entity_ns = state_result.hash_entity_ns;
    out.hash_global_ns = state_result.hash_global_ns;
    out.snapshot_build_ns = state_result.snapshot_build_ns;
    out.snapshot_publish_ns = state_result.snapshot_publish_ns;
    out.hash_cache_hits = state_result.hash_cache_hits;
    out.hash_cache_misses = state_result.hash_cache_misses;
    out.snapshot_published = state_result.snapshot_published;
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

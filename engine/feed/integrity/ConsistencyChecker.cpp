#include "feed/integrity/ConsistencyChecker.h"

#include "feed/raw_ingest/RawPacket.h"

#include <cmath>
#include <optional>
#include <utility>

namespace trading_engine::feed {

namespace {

std::uint64_t checked_now_ns() noexcept {
    return now_monotonic_ns();
}

ConsistencyResult result(
    ConsistencyCode code,
    std::string entity_id,
    std::string reason
) {
    return ConsistencyResult{
        .code = code,
        .entity_id = std::move(entity_id),
        .reason = std::move(reason),
        .checked_ns = checked_now_ns()
    };
}

std::string resolve_event_entity_id(const NormalizedEvent& event) {
    if (!event.entity_id.empty()) {
        return event.entity_id;
    }

    if (!event.asset_id.empty()) {
        return event.asset_id;
    }

    if (!event.condition_id.empty()) {
        return event.condition_id;
    }

    if (!event.market_id.empty()) {
        return event.market_id;
    }

    return {};
}

bool valid_price(double price) {
    return std::isfinite(price) && price >= 0.0 && price <= 1.0;
}

bool valid_size(double size) {
    return std::isfinite(size) && size >= 0.0;
}

bool valid_optional_price(const std::optional<double>& price) {
    return !price || valid_price(*price);
}

}  // namespace

ConsistencyResult ConsistencyChecker::check_event(
    const NormalizedEvent& event
) const noexcept {
    const std::string entity_id = resolve_event_entity_id(event);

    switch (event.event_type) {
        case NormalizedEventType::Heartbeat:
            return result(ConsistencyCode::Ok, {}, "heartbeat event");

        case NormalizedEventType::Unknown:
            return result(
                ConsistencyCode::UnknownEvent,
                entity_id,
                "unknown normalized event type"
            );

        case NormalizedEventType::TradeEvent:
            if (entity_id.empty()) {
                return result(
                    ConsistencyCode::MissingEntityId,
                    {},
                    "trade event missing entity id"
                );
            }
            return result(ConsistencyCode::Ok, entity_id, "event ok");

        case NormalizedEventType::Snapshot:
        case NormalizedEventType::Delta:
        case NormalizedEventType::StatusChange:
        case NormalizedEventType::LifecycleEvent:
            break;
    }

    if (entity_id.empty()) {
        return result(
            ConsistencyCode::MissingEntityId,
            {},
            "event missing entity id"
        );
    }

    for (const auto& level : event.bids) {
        if (!valid_price(level.price) || !valid_size(level.size)) {
            return result(
                ConsistencyCode::InvalidValue,
                entity_id,
                "snapshot bid has invalid price or size"
            );
        }
    }

    for (const auto& level : event.asks) {
        if (!valid_price(level.price) || !valid_size(level.size)) {
            return result(
                ConsistencyCode::InvalidValue,
                entity_id,
                "snapshot ask has invalid price or size"
            );
        }
    }

    for (const auto& change : event.changes) {
        if (change.side == NormalizedSide::Unknown) {
            return result(
                ConsistencyCode::InvalidValue,
                entity_id,
                "delta has unknown side"
            );
        }

        if (!valid_price(change.price) || !valid_size(change.size)) {
            return result(
                ConsistencyCode::InvalidValue,
                entity_id,
                "delta has invalid price or size"
            );
        }
    }

    if (!valid_optional_price(event.best_bid) ||
        !valid_optional_price(event.best_ask)) {
        return result(
            ConsistencyCode::InvalidValue,
            entity_id,
            "status event has invalid BBO price"
        );
    }

    if (event.best_bid && event.best_ask && *event.best_bid > *event.best_ask) {
        return result(
            ConsistencyCode::CrossedBook,
            entity_id,
            "status event best_bid is greater than best_ask"
        );
    }

    if (event.tick_size && (!std::isfinite(*event.tick_size) || *event.tick_size <= 0.0)) {
        return result(
            ConsistencyCode::InvalidValue,
            entity_id,
            "event has invalid tick size"
        );
    }

    return result(ConsistencyCode::Ok, entity_id, "event ok");
}

ConsistencyResult ConsistencyChecker::check_apply_result(
    const StateApplyResult& apply_result
) const noexcept {
    switch (apply_result.code) {
        case StateApplyCode::Applied:
        case StateApplyCode::IgnoredHeartbeat:
        case StateApplyCode::IgnoredTrade:
        case StateApplyCode::Noop:
            return result(
                ConsistencyCode::Ok,
                apply_result.entity_id,
                apply_result.message
            );

        case StateApplyCode::MissingEntityId:
            return result(
                ConsistencyCode::MissingEntityId,
                apply_result.entity_id,
                apply_result.message
            );

        case StateApplyCode::DeltaBeforeSnapshot:
            return result(
                ConsistencyCode::DeltaBeforeSnapshot,
                apply_result.entity_id,
                apply_result.message
            );

        case StateApplyCode::InvalidValue:
        case StateApplyCode::UnknownSide:
            return result(
                ConsistencyCode::InvalidValue,
                apply_result.entity_id,
                apply_result.message
            );

        case StateApplyCode::ClosedEntityIgnored:
            return result(
                ConsistencyCode::ClosedEntityMutation,
                apply_result.entity_id,
                apply_result.message
            );

        case StateApplyCode::IgnoredUnknown:
            return result(
                ConsistencyCode::UnknownEvent,
                apply_result.entity_id,
                apply_result.message
            );
    }

    return result(
        ConsistencyCode::InvalidValue,
        apply_result.entity_id,
        "unrecognized apply code"
    );
}

ConsistencyResult ConsistencyChecker::check_entity_state(
    const EntityState& entity
) const noexcept {
    if (entity.status == EntityStatus::Inconsistent) {
        if (entity.book.crossed) {
            return result(
                ConsistencyCode::CrossedBook,
                entity.entity_id,
                "entity status is inconsistent due to crossed book"
            );
        }

        return result(
            ConsistencyCode::InvalidValue,
            entity.entity_id,
            "entity status is inconsistent"
        );
    }

    if (entity.book.best_bid &&
        entity.book.best_ask &&
        *entity.book.best_bid > *entity.book.best_ask) {
        return result(
            ConsistencyCode::CrossedBook,
            entity.entity_id,
            "entity best_bid is greater than best_ask"
        );
    }

    if (entity.book.crossed) {
        return result(
            ConsistencyCode::CrossedBook,
            entity.entity_id,
            "entity book is crossed"
        );
    }

    if (entity.book.external_bbo_diverged) {
        return result(
            ConsistencyCode::ExternalBboDiverged,
            entity.entity_id,
            "external BBO diverged from local BBO"
        );
    }

    return result(ConsistencyCode::Ok, entity.entity_id, "entity state ok");
}

ConsistencyResult ConsistencyChecker::check(
    const NormalizedEvent& event,
    const StateApplyResult& apply_result,
    const EntityState* entity
) const noexcept {
    const auto event_result = check_event(event);
    if (!event_result.ok()) {
        return event_result;
    }

    const auto apply_check = check_apply_result(apply_result);
    if (!apply_check.ok()) {
        return apply_check;
    }

    if (entity) {
        return check_entity_state(*entity);
    }

    return result(ConsistencyCode::Ok, apply_result.entity_id, "consistent");
}

std::string to_string(ConsistencyCode code) {
    switch (code) {
        case ConsistencyCode::Ok:
            return "Ok";
        case ConsistencyCode::MissingEntityId:
            return "MissingEntityId";
        case ConsistencyCode::DeltaBeforeSnapshot:
            return "DeltaBeforeSnapshot";
        case ConsistencyCode::InvalidValue:
            return "InvalidValue";
        case ConsistencyCode::CrossedBook:
            return "CrossedBook";
        case ConsistencyCode::ExternalBboDiverged:
            return "ExternalBboDiverged";
        case ConsistencyCode::ClosedEntityMutation:
            return "ClosedEntityMutation";
        case ConsistencyCode::DecodeError:
            return "DecodeError";
        case ConsistencyCode::UnknownEvent:
            return "UnknownEvent";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::feed

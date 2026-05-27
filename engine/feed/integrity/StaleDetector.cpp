#include "feed/integrity/StaleDetector.h"

#include <utility>

namespace trading_engine::feed {

namespace {

std::uint64_t age_or_max(
    std::uint64_t now_ns,
    std::uint64_t last_seen_ns
) noexcept {
    if (last_seen_ns > now_ns) {
        return 0;
    }

    return now_ns - last_seen_ns;
}

StaleResult result(
    StaleLevel level,
    std::string entity_id,
    std::string reason,
    std::uint64_t age_ns
) {
    return StaleResult{
        .level = level,
        .entity_id = std::move(entity_id),
        .reason = std::move(reason),
        .age_ns = age_ns
    };
}

}  // namespace

StaleDetector::StaleDetector(
    std::uint64_t source_stale_timeout_ns,
    std::uint64_t entity_stale_timeout_ns
) noexcept
    : source_stale_timeout_ns_(source_stale_timeout_ns),
      entity_stale_timeout_ns_(entity_stale_timeout_ns) {}

void StaleDetector::configure_source_timeout(
    std::uint64_t timeout_ns
) noexcept {
    source_stale_timeout_ns_ = timeout_ns;
}

void StaleDetector::configure_entity_timeout(
    std::uint64_t timeout_ns
) noexcept {
    entity_stale_timeout_ns_ = timeout_ns;
}

StaleResult StaleDetector::check_source(
    std::uint64_t now_ns,
    std::uint64_t last_message_received_ns
) const noexcept {
    const auto age_ns = age_or_max(now_ns, last_message_received_ns);

    if (age_ns > source_stale_timeout_ns_) {
        return result(
            StaleLevel::SourceStale,
            {},
            "source has not received a message within stale timeout",
            age_ns
        );
    }

    return result(StaleLevel::Ok, {}, "source fresh", age_ns);
}

StaleResult StaleDetector::check_entity(
    std::uint64_t now_ns,
    const EntityState& entity
) const noexcept {
    if (entity.closed || entity.status == EntityStatus::Closed) {
        return result(
            StaleLevel::Ok,
            entity.entity_id,
            "closed entity ignored for stale detection",
            0
        );
    }

    if (!entity.initialized || entity.last_update_monotonic_ns == 0) {
        return result(
            StaleLevel::Ok,
            entity.entity_id,
            "uninitialized entity ignored for stale detection",
            0
        );
    }

    const auto age_ns = age_or_max(now_ns, entity.last_update_monotonic_ns);

    if (age_ns > entity_stale_timeout_ns_) {
        return result(
            StaleLevel::EntityStale,
            entity.entity_id,
            "entity has not updated within stale timeout",
            age_ns
        );
    }

    return result(StaleLevel::Ok, entity.entity_id, "entity fresh", age_ns);
}

std::uint64_t StaleDetector::source_stale_timeout_ns() const noexcept {
    return source_stale_timeout_ns_;
}

std::uint64_t StaleDetector::entity_stale_timeout_ns() const noexcept {
    return entity_stale_timeout_ns_;
}

std::string to_string(StaleLevel level) {
    switch (level) {
        case StaleLevel::Ok:
            return "Ok";
        case StaleLevel::SourceStale:
            return "SourceStale";
        case StaleLevel::EntityStale:
            return "EntityStale";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::feed

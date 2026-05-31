#include "state/EntityStateStore.h"
#include "state/StateHasher.h"

#include <chrono>
#include <cmath>
#include <utility>

namespace trading_engine::state {

namespace {

constexpr double kBboEpsilon = 1e-12;
constexpr std::uint64_t kCheapHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kCheapHashPrime = 1099511628211ULL;

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count()
    );
}

std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

StateRuntimeConfig legacy_replay_full_config() noexcept {
    StateRuntimeConfig config;
    config.hash_mode = StateHashMode::ReplayFull;
    config.publish_on_noop = true;
    config.publish_on_heartbeat = true;
    config.compute_full_hash_on_publish = true;
    return config;
}

void cheap_mix_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kCheapHashPrime;
}

void cheap_mix_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        cheap_mix_byte(
            hash,
            static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU)
        );
    }
}

void cheap_mix_string(std::uint64_t& hash, const std::string& value) noexcept {
    cheap_mix_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const unsigned char ch : value) {
        cheap_mix_byte(hash, ch);
    }
}

std::uint64_t status_flags_for(const EntityState* entity) noexcept {
    if (!entity) {
        return 0;
    }

    std::uint64_t flags = 0;
    flags |= static_cast<std::uint64_t>(entity->status) & 0xffU;
    flags |= entity->initialized ? (1ULL << 8U) : 0ULL;
    flags |= entity->recovering ? (1ULL << 9U) : 0ULL;
    flags |= entity->inconsistent ? (1ULL << 10U) : 0ULL;
    flags |= entity->closed ? (1ULL << 11U) : 0ULL;
    flags |= entity->book.crossed ? (1ULL << 12U) : 0ULL;
    flags |= entity->book.resolved ? (1ULL << 13U) : 0ULL;
    return flags;
}

std::uint64_t cheap_snapshot_fingerprint(
    const std::string& entity_id,
    std::uint64_t book_version,
    std::uint64_t chain_version,
    std::uint64_t quality_version,
    std::uint64_t last_ws_packet_id,
    std::uint64_t last_chain_block,
    std::uint64_t status_flags
) noexcept {
    std::uint64_t hash = kCheapHashOffset;
    cheap_mix_string(hash, entity_id);
    cheap_mix_u64(hash, book_version);
    cheap_mix_u64(hash, chain_version);
    cheap_mix_u64(hash, quality_version);
    cheap_mix_u64(hash, last_ws_packet_id);
    cheap_mix_u64(hash, last_chain_block);
    cheap_mix_u64(hash, status_flags);
    return hash;
}

StateMutationKind mutation_kind_for(StateApplyCode code) noexcept {
    switch (code) {
        case StateApplyCode::IgnoredHeartbeat:
            return StateMutationKind::Heartbeat;
        default:
            return StateMutationKind::None;
    }
}

/**
 * @brief Return true if price is valid for Polymarket probability-style prices.
 *
 * Polymarket CLOB prices are probability-like values in [0, 1].
 */
bool valid_price(double price) {
    return std::isfinite(price) && price >= 0.0 && price <= 1.0;
}

/**
 * @brief Return true if size is finite and non-negative.
 */
bool valid_size(double size) {
    return std::isfinite(size) && size >= 0.0;
}

/**
 * @brief Return true if tick size is finite and positive.
 */
bool valid_tick_size(double tick_size) {
    return std::isfinite(tick_size) && tick_size > 0.0;
}

/**
 * @brief Treat tiny floating point differences as equal for BBO comparison.
 */
bool almost_equal(double a, double b) {
    return std::fabs(a - b) <= kBboEpsilon;
}

/**
 * @brief Upsert or erase one price level.
 */
template <typename MapT>
void apply_level(MapT& side, double price, double size) {
    if (size == 0.0) {
        side.erase(price);
        return;
    }

    side[price] = size;
}

}  // namespace

StateApplyResult mark_book_snapshot(StateApplyResult result) {
    result.mutation.state_changed = result.state_changed;
    result.mutation.book_changed = result.state_changed;
    result.mutation.publish_required = result.state_changed;
    result.mutation.kind = result.state_changed
        ? StateMutationKind::BookSnapshot
        : StateMutationKind::None;
    return result;
}

StateApplyResult mark_book_delta(StateApplyResult result) {
    result.mutation.state_changed = result.state_changed;
    result.mutation.book_changed = result.state_changed;
    result.mutation.publish_required = result.state_changed;
    result.mutation.kind = result.state_changed
        ? StateMutationKind::BookDelta
        : StateMutationKind::None;
    return result;
}

StateApplyResult mark_lifecycle(StateApplyResult result) {
    result.mutation.state_changed = result.state_changed;
    result.mutation.lifecycle_changed = result.state_changed;
    result.mutation.publish_required = result.state_changed;
    result.mutation.kind = result.state_changed
        ? StateMutationKind::Lifecycle
        : StateMutationKind::None;
    return result;
}

EntityStateStore::EntityStateStore()
    : EntityStateStore(legacy_replay_full_config()) {}

EntityStateStore::EntityStateStore(StateRuntimeConfig runtime_config)
    : runtime_config_(runtime_config) {}

const StateRuntimeConfig& EntityStateStore::runtime_config() const noexcept {
    return runtime_config_;
}

bool StateApplyResult::ok() const noexcept {
    switch (code) {
        case StateApplyCode::Applied:
        case StateApplyCode::IgnoredHeartbeat:
        case StateApplyCode::IgnoredUnknown:
        case StateApplyCode::IgnoredTrade:
        case StateApplyCode::ClosedEntityIgnored:
        case StateApplyCode::Noop:
            return true;

        case StateApplyCode::MissingEntityId:
        case StateApplyCode::DeltaBeforeSnapshot:
        case StateApplyCode::UnknownSide:
        case StateApplyCode::InvalidValue:
        default:
            return false;
    }
}

StateApplyResult EntityStateStore::apply(const NormalizedEvent& event) {
    ++events_seen_;

    switch (event.event_type) {
        case NormalizedEventType::Snapshot:
            return apply_snapshot(event);

        case NormalizedEventType::Delta:
            return apply_delta(event);

        case NormalizedEventType::StatusChange:
            return apply_status_change(event);

        case NormalizedEventType::LifecycleEvent:
            return apply_lifecycle_event(event);

        case NormalizedEventType::TradeEvent:
            return apply_trade_event(event);

        case NormalizedEventType::Heartbeat:
            return apply_heartbeat(event);

        case NormalizedEventType::Unknown:
        case NormalizedEventType::DecodeError:
        default:
            return apply_unknown(event);
    }
}

void EntityStateStore::reset() {
    entities_.clear();
    hash_cache_.clear();

    events_seen_ = 0;
    snapshots_applied_ = 0;
    deltas_applied_ = 0;
    status_changes_applied_ = 0;
    lifecycle_events_applied_ = 0;
    heartbeats_ignored_ = 0;
    unknown_events_ignored_ = 0;
    errors_ = 0;
}

bool EntityStateStore::contains(const std::string& entity_id) const noexcept {
    return entities_.find(entity_id) != entities_.end();
}

const EntityState* EntityStateStore::get(
    const std::string& entity_id
) const noexcept {
    const auto it = entities_.find(entity_id);

    if (it == entities_.end()) {
        return nullptr;
    }

    return &it->second;
}

EntityStatus EntityStateStore::status(
    const std::string& entity_id
) const noexcept {
    const auto* entity = get(entity_id);

    if (!entity) {
        return EntityStatus::Uninitialized;
    }

    return entity->status;
}

bool EntityStateStore::initialized(
    const std::string& entity_id
) const noexcept {
    const auto* entity = get(entity_id);
    return entity && entity->initialized;
}

std::size_t EntityStateStore::entity_count() const noexcept {
    return entities_.size();
}

std::uint64_t EntityStateStore::state_hash(
    const std::string& entity_id
) const {
    return hash_cache_.entity_hash(entity_id, *this);
}

std::uint64_t EntityStateStore::global_hash() const {
    return hash_cache_.global_hash(*this);
}

std::uint64_t EntityStateStore::debug_recomputed_state_hash(
    const std::string& entity_id
) const noexcept {
    const auto* entity = get(entity_id);
    return entity ? StateHasher::hash_entity(*entity) : 0;
}

std::uint64_t EntityStateStore::debug_recomputed_global_hash() const noexcept {
    return StateHasher::hash_entity_map(entities_);
}

std::uint64_t EntityStateStore::events_seen() const noexcept {
    return events_seen_;
}

std::uint64_t EntityStateStore::snapshots_applied() const noexcept {
    return snapshots_applied_;
}

std::uint64_t EntityStateStore::deltas_applied() const noexcept {
    return deltas_applied_;
}

std::uint64_t EntityStateStore::status_changes_applied() const noexcept {
    return status_changes_applied_;
}

std::uint64_t EntityStateStore::lifecycle_events_applied() const noexcept {
    return lifecycle_events_applied_;
}

std::uint64_t EntityStateStore::heartbeats_ignored() const noexcept {
    return heartbeats_ignored_;
}

std::uint64_t EntityStateStore::unknown_events_ignored() const noexcept {
    return unknown_events_ignored_;
}

std::uint64_t EntityStateStore::errors() const noexcept {
    return errors_;
}

StateApplyResult EntityStateStore::apply_snapshot(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (entity_id.empty()) {
        ++errors_;
        return make_result(
            StateApplyCode::MissingEntityId,
            {},
            "snapshot event missing entity id",
            false
        );
    }

    EntityState& entity = get_or_create_entity(entity_id);

    if (entity.closed) {
        ++entity.ignored_count;

        return make_result(
            StateApplyCode::ClosedEntityIgnored,
            entity_id,
            "snapshot ignored because entity is closed",
            false
        );
    }

    entity.book.bids.clear();
    entity.book.asks.clear();

    bool invalid_level_seen = false;

    for (const auto& level : event.bids) {
        if (!valid_price(level.price) || !valid_size(level.size)) {
            invalid_level_seen = true;
            continue;
        }

        if (level.size > 0.0) {
            entity.book.bids[level.price] = level.size;
        }
    }

    for (const auto& level : event.asks) {
        if (!valid_price(level.price) || !valid_size(level.size)) {
            invalid_level_seen = true;
            continue;
        }

        if (level.size > 0.0) {
            entity.book.asks[level.price] = level.size;
        }
    }

    if (event.tick_size) {
        if (valid_tick_size(*event.tick_size)) {
            entity.book.tick_size = *event.tick_size;
        } else {
            invalid_level_seen = true;
        }
    }

    update_best_bid_ask(entity.book);

    entity.initialized = true;
    entity.recovering = false;
    entity.closed = false;

    entity.status = entity.book.crossed
        ? EntityStatus::Inconsistent
        : EntityStatus::Live;

    entity.inconsistent = entity.book.crossed || invalid_level_seen;

    ++entity.snapshot_count;
    entity.last_snapshot_packet_id = event.packet_id;

    mark_entity_packet(entity, event);

    ++snapshots_applied_;

    if (invalid_level_seen) {
        ++entity.error_count;
        ++errors_;

        return mark_book_snapshot(make_result(
            StateApplyCode::InvalidValue,
            entity_id,
            "snapshot contained invalid price/size/tick values",
            true
        ));
    }

    return mark_book_snapshot(make_result(
        StateApplyCode::Applied,
        entity_id,
        "snapshot applied",
        true
    ));
}

StateApplyResult EntityStateStore::apply_delta(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (entity_id.empty()) {
        ++errors_;
        return mark_book_delta(make_result(
            StateApplyCode::MissingEntityId,
            {},
            "delta event missing entity id",
            false
        ));
    }

    EntityState& entity = get_or_create_entity(entity_id);

    if (entity.closed) {
        ++entity.ignored_count;

        return mark_book_delta(make_result(
            StateApplyCode::ClosedEntityIgnored,
            entity_id,
            "delta ignored because entity is closed",
            false
        ));
    }

    if (!entity.initialized) {
        entity.status = EntityStatus::Recovering;
        entity.recovering = true;

        ++entity.error_count;
        ++errors_;

        mark_entity_packet(entity, event);

        return mark_book_delta(make_result(
            StateApplyCode::DeltaBeforeSnapshot,
            entity_id,
            "delta arrived before snapshot",
            true
        ));
    }

    bool changed = false;
    bool bad_side_seen = false;
    bool invalid_value_seen = false;

    for (const auto& change : event.changes) {
        if (!valid_price(change.price) || !valid_size(change.size)) {
            invalid_value_seen = true;
            continue;
        }

        switch (change.side) {
            case NormalizedSide::Bid:
                apply_level(entity.book.bids, change.price, change.size);
                changed = true;
                break;

            case NormalizedSide::Ask:
                apply_level(entity.book.asks, change.price, change.size);
                changed = true;
                break;

            case NormalizedSide::Unknown:
            default:
                bad_side_seen = true;
                break;
        }
    }

    if (changed) {
        update_best_bid_ask(entity.book);
        ++entity.delta_count;
        ++deltas_applied_;

        entity.status = entity.book.crossed
            ? EntityStatus::Inconsistent
            : EntityStatus::Live;

        entity.inconsistent = entity.book.crossed;
        entity.recovering = false;

        mark_entity_packet(entity, event);
    }

    if (bad_side_seen || invalid_value_seen) {
        ++entity.error_count;
        ++errors_;

        if (bad_side_seen) {
            return mark_book_delta(make_result(
                StateApplyCode::UnknownSide,
                entity_id,
                "delta contained unknown side",
                changed
            ));
        }

        return mark_book_delta(make_result(
            StateApplyCode::InvalidValue,
            entity_id,
            "delta contained invalid price or size",
            changed
        ));
    }

    if (!changed) {
        ++entity.ignored_count;

        return mark_book_delta(make_result(
            StateApplyCode::Noop,
            entity_id,
            "delta had no valid changes",
            false
        ));
    }

    return mark_book_delta(make_result(
        StateApplyCode::Applied,
        entity_id,
        "delta applied",
        true
    ));
}

StateApplyResult EntityStateStore::apply_status_change(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (entity_id.empty()) {
        ++errors_;
        return mark_book_delta(make_result(
            StateApplyCode::MissingEntityId,
            {},
            "status change missing entity id",
            false
        ));
    }

    EntityState& entity = get_or_create_entity(entity_id);

    if (entity.closed) {
        ++entity.ignored_count;

        return mark_book_delta(make_result(
            StateApplyCode::ClosedEntityIgnored,
            entity_id,
            "status change ignored because entity is closed",
            false
        ));
    }

    bool changed = false;
    bool invalid_seen = false;

    if (event.tick_size) {
        if (valid_tick_size(*event.tick_size)) {
            entity.book.tick_size = *event.tick_size;
            changed = true;
        } else {
            invalid_seen = true;
        }
    }

    if (event.best_bid) {
        if (valid_price(*event.best_bid)) {
            entity.book.external_best_bid = *event.best_bid;
            changed = true;
        } else {
            invalid_seen = true;
        }
    }

    if (event.best_ask) {
        if (valid_price(*event.best_ask)) {
            entity.book.external_best_ask = *event.best_ask;
            changed = true;
        } else {
            invalid_seen = true;
        }
    }

    entity.book.external_bbo_diverged = false;

    if (entity.initialized &&
        entity.book.external_best_bid &&
        entity.book.best_bid &&
        !almost_equal(*entity.book.external_best_bid, *entity.book.best_bid)) {
        entity.book.external_bbo_diverged = true;
    }

    if (entity.initialized &&
        entity.book.external_best_ask &&
        entity.book.best_ask &&
        !almost_equal(*entity.book.external_best_ask, *entity.book.best_ask)) {
        entity.book.external_bbo_diverged = true;
    }

    if (!entity.initialized) {
        entity.status = EntityStatus::Recovering;
        entity.recovering = true;
    }

    ++entity.status_change_count;
    ++status_changes_applied_;

    mark_entity_packet(entity, event);

    if (invalid_seen) {
        ++entity.error_count;
        ++errors_;

        return mark_book_delta(make_result(
            StateApplyCode::InvalidValue,
            entity_id,
            "status change contained invalid values",
            changed
        ));
    }

    return mark_book_delta(make_result(
        changed ? StateApplyCode::Applied : StateApplyCode::Noop,
        entity_id,
        changed ? "status change applied" : "status change had no state fields",
        changed
    ));
}

StateApplyResult EntityStateStore::apply_lifecycle_event(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (entity_id.empty()) {
        ++errors_;
        return mark_lifecycle(make_result(
            StateApplyCode::MissingEntityId,
            {},
            "lifecycle event missing entity id",
            false
        ));
    }

    EntityState& entity = get_or_create_entity(entity_id);

    if (event.raw_type == "market_resolved") {
        entity.closed = true;
        entity.status = EntityStatus::Closed;
        entity.recovering = false;

        entity.book.resolved = true;
        entity.book.winning_asset_id = event.winning_asset_id;
    } else if (event.raw_type == "market_closed") {
        entity.closed = true;
        entity.status = EntityStatus::Closed;
        entity.recovering = false;
    }

    ++entity.lifecycle_count;
    ++lifecycle_events_applied_;

    mark_entity_packet(entity, event);

    return mark_lifecycle(make_result(
        StateApplyCode::Applied,
        entity_id,
        "lifecycle event applied",
        true
    ));
}

StateApplyResult EntityStateStore::apply_trade_event(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (!entity_id.empty()) {
        EntityState& entity = get_or_create_entity(entity_id);
        ++entity.ignored_count;
        mark_entity_packet(entity, event);
    }

    return make_result(
        StateApplyCode::IgnoredTrade,
        entity_id,
        "trade event ignored by EntityStateStore MVP",
        false
    );
}

StateApplyResult EntityStateStore::apply_heartbeat(
    const NormalizedEvent& event
) {
    (void)event;

    ++heartbeats_ignored_;

    return make_result(
        StateApplyCode::IgnoredHeartbeat,
        {},
        "heartbeat ignored by entity state",
        false
    );
}

StateApplyResult EntityStateStore::apply_unknown(
    const NormalizedEvent& event
) {
    const std::string entity_id = resolve_entity_id(event);

    if (!entity_id.empty()) {
        EntityState& entity = get_or_create_entity(entity_id);
        ++entity.ignored_count;
        mark_entity_packet(entity, event);
    }

    ++unknown_events_ignored_;

    return make_result(
        StateApplyCode::IgnoredUnknown,
        entity_id,
        "unknown event ignored by entity state",
        false
    );
}

EntityState& EntityStateStore::get_or_create_entity(
    const std::string& entity_id
) {
    auto [it, inserted] = entities_.try_emplace(entity_id);

    EntityState& entity = it->second;

    if (inserted) {
        entity.entity_id = entity_id;
    }

    return entity;
}

std::string EntityStateStore::resolve_entity_id(
    const NormalizedEvent& event
) const {
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

void EntityStateStore::update_best_bid_ask(OrderBookState& book) const noexcept {
    if (book.bids.empty()) {
        book.best_bid.reset();
    } else {
        book.best_bid = book.bids.begin()->first;
    }

    if (book.asks.empty()) {
        book.best_ask.reset();
    } else {
        book.best_ask = book.asks.begin()->first;
    }

    book.crossed =
        book.best_bid &&
        book.best_ask &&
        *book.best_bid > *book.best_ask;
}

void EntityStateStore::mark_entity_packet(
    EntityState& entity,
    const NormalizedEvent& event
) {
    if (!event.market_id.empty()) {
        entity.market_id = event.market_id;
    }

    if (entity.first_packet_id == 0) {
        entity.first_packet_id = event.packet_id;
    }

    entity.last_packet_id = event.packet_id;
    entity.last_update_monotonic_ns = event.recv_monotonic_ns;
}

StateApplyResult EntityStateStore::make_result(
    StateApplyCode code,
    std::string entity_id,
    std::string message,
    bool state_changed
) const {
    const auto make_result_start_ns = now_ns();
    StateApplyResult result;
    const auto finish = [&]() {
        result.make_result_ns = checked_sub(now_ns(), make_result_start_ns);
        return result;
    };

    result.code = code;
    result.entity_id = std::move(entity_id);
    result.message = std::move(message);
    result.state_changed = state_changed;
    result.mutation.state_changed = state_changed;
    result.mutation.heartbeat_seen = code == StateApplyCode::IgnoredHeartbeat;
    result.mutation.publish_required = false;
    result.mutation.kind = mutation_kind_for(code);
    std::uint64_t last_ws_packet_id = 0;
    std::uint64_t status_flags = 0;
    if (!result.entity_id.empty()) {
        const auto* entity = get(result.entity_id);
        if (entity) {
            result.book_version = entity->last_packet_id;
            last_ws_packet_id = entity->last_packet_id;
            status_flags = status_flags_for(entity);
        }
    }

    result.snapshot_version_hash = cheap_snapshot_fingerprint(
        result.entity_id,
        result.book_version,
        result.chain_version,
        result.quality_version,
        last_ws_packet_id,
        0,
        status_flags
    );
    result.cheap_fingerprint = result.snapshot_version_hash;

    if (result.mutation.state_changed && !result.entity_id.empty()) {
        hash_cache_.mark_dirty(result.entity_id);
    }

    if (!result.mutation.state_changed ||
        runtime_config_.hash_mode == StateHashMode::HotPathLight) {
        return finish();
    }

    (void)hash_cache_.take_access_stats();
    if (!result.entity_id.empty()) {
        const auto hash_start_ns = now_ns();
        result.entity_hash = state_hash(result.entity_id);
        result.hash_entity_ns = checked_sub(now_ns(), hash_start_ns);
    }

    const auto global_hash_start_ns = now_ns();
    result.global_hash = global_hash();
    result.hash_global_ns = checked_sub(now_ns(), global_hash_start_ns);
    result.full_hash_computed = true;

    const auto cache_stats = hash_cache_.take_access_stats();
    result.hash_cache_hits = cache_stats.hits;
    result.hash_cache_misses = cache_stats.misses;

    return finish();
}

std::string to_string(EntityStatus status) {
    switch (status) {
        case EntityStatus::Uninitialized:
            return "Uninitialized";
        case EntityStatus::Live:
            return "Live";
        case EntityStatus::Recovering:
            return "Recovering";
        case EntityStatus::Inconsistent:
            return "Inconsistent";
        case EntityStatus::Stale:
            return "Stale";
        case EntityStatus::Closed:
            return "Closed";
        default:
            return "Unknown";
    }
}

std::string to_string(StateApplyCode code) {
    switch (code) {
        case StateApplyCode::Applied:
            return "Applied";
        case StateApplyCode::IgnoredHeartbeat:
            return "IgnoredHeartbeat";
        case StateApplyCode::IgnoredUnknown:
            return "IgnoredUnknown";
        case StateApplyCode::IgnoredTrade:
            return "IgnoredTrade";
        case StateApplyCode::MissingEntityId:
            return "MissingEntityId";
        case StateApplyCode::DeltaBeforeSnapshot:
            return "DeltaBeforeSnapshot";
        case StateApplyCode::ClosedEntityIgnored:
            return "ClosedEntityIgnored";
        case StateApplyCode::UnknownSide:
            return "UnknownSide";
        case StateApplyCode::InvalidValue:
            return "InvalidValue";
        case StateApplyCode::Noop:
            return "Noop";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state

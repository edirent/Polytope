#pragma once

#include "feed/decode/EventNormalizer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::feed {

/**
 * @brief Lifecycle state of one logical market/entity.
 *
 * For Polymarket Market channel, the entity is usually an asset_id.
 */
enum class EntityStatus {
    /**
     * @brief Entity exists only as an empty shell.
     *
     * No book snapshot has been applied yet.
     */
    Uninitialized = 0,

    /**
     * @brief Entity has received a valid snapshot and can accept deltas.
     */
    Live,

    /**
     * @brief Entity cannot be trusted and needs a new snapshot.
     *
     * Typical cause:
     *
     *     Delta arrived before Snapshot.
     */
    Recovering,

    /**
     * @brief Entity has internal inconsistency.
     *
     * Example:
     *
     *     invalid price,
     *     invalid side,
     *     crossed book.
     */
    Inconsistent,

    /**
     * @brief Entity is stale.
     *
     * Stale detection is usually handled by a separate detector, but the state
     * enum supports it.
     */
    Stale,

    /**
     * @brief Entity is closed/resolved.
     *
     * Normal book updates should no longer mutate it.
     */
    Closed
};

/**
 * @brief Result code after trying to apply one NormalizedEvent.
 */
enum class StateApplyCode {
    Applied = 0,

    /**
     * @brief Event intentionally ignored because it should not change state.
     */
    IgnoredHeartbeat,
    IgnoredUnknown,
    IgnoredTrade,

    /**
     * @brief Event had no usable entity id.
     */
    MissingEntityId,

    /**
     * @brief Delta arrived before the entity had a snapshot.
     */
    DeltaBeforeSnapshot,

    /**
     * @brief Event tried to mutate an entity that is already closed.
     */
    ClosedEntityIgnored,

    /**
     * @brief Event contained an invalid side.
     */
    UnknownSide,

    /**
     * @brief Event contained invalid price/size/tick data.
     */
    InvalidValue,

    /**
     * @brief Event was valid but did not change state.
     */
    Noop
};

/**
 * @brief Result returned by EntityStateStore::apply().
 */
struct StateApplyResult {
    StateApplyCode code{StateApplyCode::Noop};

    /**
     * @brief Entity affected by this event, if any.
     */
    std::string entity_id;

    /**
     * @brief Human-readable reason. Useful for tests/tools.
     */
    std::string message;

    /**
     * @brief Whether the state changed.
     */
    bool state_changed{false};

    /**
     * @brief Hash of the affected entity after apply.
     *
     * 0 if no entity was affected.
     */
    std::uint64_t entity_hash{0};

    /**
     * @brief Hash of all entities after apply.
     */
    std::uint64_t global_hash{0};

    /**
     * @brief True if this result is acceptable and does not indicate a state error.
     */
    [[nodiscard]] bool ok() const noexcept;
};

/**
 * @brief Deterministic order book state for one entity.
 *
 * Use std::map, not unordered_map, because replay trace hashing must be
 * deterministic.
 */
struct OrderBookState {
    /**
     * @brief Bid side, sorted high-to-low.
     */
    std::map<double, double, std::greater<double>> bids;

    /**
     * @brief Ask side, sorted low-to-high.
     */
    std::map<double, double> asks;

    /**
     * @brief Local best bid/ask computed from book levels.
     */
    std::optional<double> best_bid;
    std::optional<double> best_ask;

    /**
     * @brief External best bid/ask from best_bid_ask event, if received.
     *
     * These should not blindly overwrite local book-derived BBO.
     * They are reference/status fields.
     */
    std::optional<double> external_best_bid;
    std::optional<double> external_best_ask;

    /**
     * @brief Tick size if known.
     */
    std::optional<double> tick_size;

    /**
     * @brief True if local best_bid > local best_ask.
     */
    bool crossed{false};

    /**
     * @brief True if external BBO disagrees with local book-derived BBO.
     */
    bool external_bbo_diverged{false};

    /**
     * @brief Resolution state.
     */
    bool resolved{false};
    std::string winning_asset_id;
};

/**
 * @brief State and counters for one entity.
 */
struct EntityState {
    std::string entity_id;

    EntityStatus status{EntityStatus::Uninitialized};

    bool initialized{false};
    bool recovering{false};
    bool inconsistent{false};
    bool closed{false};

    std::uint64_t snapshot_count{0};
    std::uint64_t delta_count{0};
    std::uint64_t status_change_count{0};
    std::uint64_t lifecycle_count{0};
    std::uint64_t ignored_count{0};
    std::uint64_t error_count{0};

    std::uint64_t first_packet_id{0};
    std::uint64_t last_packet_id{0};
    std::uint64_t last_snapshot_packet_id{0};

    /**
     * @brief Receive monotonic timestamp of last state-changing event.
     *
     * This is not included in state hash.
     */
    std::uint64_t last_update_monotonic_ns{0};

    OrderBookState book;
};

/**
 * @brief Maintains normalized entity/order-book state.
 *
 * This class consumes NormalizedEvent objects only.
 *
 * It does not:
 *
 * - read RawPacket,
 * - parse JSON,
 * - write logs,
 * - reconnect WebSocket,
 * - decide trading signals.
 */
class EntityStateStore {
public:
    EntityStateStore() = default;

    /**
     * @brief Apply one normalized event to the store.
     */
    StateApplyResult apply(const NormalizedEvent& event);

    /**
     * @brief Clear all state and counters.
     */
    void reset();

    /**
     * @brief Return true if entity exists.
     */
    [[nodiscard]] bool contains(const std::string& entity_id) const noexcept;

    /**
     * @brief Return pointer to entity state, or nullptr if missing.
     */
    [[nodiscard]] const EntityState* get(
        const std::string& entity_id
    ) const noexcept;

    /**
     * @brief Return current status of an entity.
     *
     * Missing entity returns Uninitialized.
     */
    [[nodiscard]] EntityStatus status(
        const std::string& entity_id
    ) const noexcept;

    /**
     * @brief Return true if entity exists and has been initialized by snapshot.
     */
    [[nodiscard]] bool initialized(
        const std::string& entity_id
    ) const noexcept;

    /**
     * @brief Return number of entities.
     */
    [[nodiscard]] std::size_t entity_count() const noexcept;

    /**
     * @brief Return deterministic hash for one entity.
     *
     * Missing entity returns 0.
     */
    [[nodiscard]] std::uint64_t state_hash(
        const std::string& entity_id
    ) const noexcept;

    /**
     * @brief Return deterministic hash for the entire store.
     */
    [[nodiscard]] std::uint64_t global_hash() const noexcept;

    [[nodiscard]] std::uint64_t events_seen() const noexcept;
    [[nodiscard]] std::uint64_t snapshots_applied() const noexcept;
    [[nodiscard]] std::uint64_t deltas_applied() const noexcept;
    [[nodiscard]] std::uint64_t status_changes_applied() const noexcept;
    [[nodiscard]] std::uint64_t lifecycle_events_applied() const noexcept;
    [[nodiscard]] std::uint64_t heartbeats_ignored() const noexcept;
    [[nodiscard]] std::uint64_t unknown_events_ignored() const noexcept;
    [[nodiscard]] std::uint64_t errors() const noexcept;

private:
    StateApplyResult apply_snapshot(const NormalizedEvent& event);
    StateApplyResult apply_delta(const NormalizedEvent& event);
    StateApplyResult apply_status_change(const NormalizedEvent& event);
    StateApplyResult apply_lifecycle_event(const NormalizedEvent& event);
    StateApplyResult apply_trade_event(const NormalizedEvent& event);
    StateApplyResult apply_heartbeat(const NormalizedEvent& event);
    StateApplyResult apply_unknown(const NormalizedEvent& event);

    [[nodiscard]] EntityState& get_or_create_entity(
        const std::string& entity_id
    );

    [[nodiscard]] std::string resolve_entity_id(
        const NormalizedEvent& event
    ) const;

    void update_best_bid_ask(OrderBookState& book) const noexcept;
    void mark_entity_packet(EntityState& entity, const NormalizedEvent& event);

    [[nodiscard]] StateApplyResult make_result(
        StateApplyCode code,
        std::string entity_id,
        std::string message,
        bool state_changed
    ) const noexcept;

private:
    /**
     * @brief std::map gives deterministic entity iteration order for hashing.
     */
    std::map<std::string, EntityState> entities_;

    std::uint64_t events_seen_{0};
    std::uint64_t snapshots_applied_{0};
    std::uint64_t deltas_applied_{0};
    std::uint64_t status_changes_applied_{0};
    std::uint64_t lifecycle_events_applied_{0};
    std::uint64_t heartbeats_ignored_{0};
    std::uint64_t unknown_events_ignored_{0};
    std::uint64_t errors_{0};
};

[[nodiscard]] std::string to_string(EntityStatus status);
[[nodiscard]] std::string to_string(StateApplyCode code);

}  // namespace trading_engine::feed
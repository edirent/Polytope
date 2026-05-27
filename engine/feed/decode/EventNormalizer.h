#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace trading_engine::feed {

/**
 * @brief Normalized internal event category.
 *
 * This is deliberately much smaller than Polymarket's raw event space.
 *
 * The point of the normalizer is to convert venue-specific raw event names
 * into stable internal event categories consumed by downstream layers.
 */
enum class NormalizedEventType {
    Unknown = 0,

    /**
     * @brief Full state snapshot.
     *
     * Polymarket mapping:
     *
     *     book -> Snapshot
     */
    Snapshot,

    /**
     * @brief Incremental state update.
     *
     * Polymarket mapping:
     *
     *     price_change -> Delta
     */
    Delta,

    /**
     * @brief Metadata or external state update.
     *
     * Polymarket mapping:
     *
     *     best_bid_ask     -> StatusChange
     *     tick_size_change -> StatusChange
     */
    StatusChange,

    /**
     * @brief Market lifecycle update.
     *
     * Polymarket mapping:
     *
     *     new_market      -> LifecycleEvent
     *     market_resolved -> LifecycleEvent
     */
    LifecycleEvent,

    /**
     * @brief Trade-related event.
     *
     * Polymarket mapping:
     *
     *     last_trade_price -> TradeEvent
     */
    TradeEvent,

    /**
     * @brief Heartbeat / control event.
     *
     * Polymarket examples:
     *
     *     PONG
     *     pong
     *     "PONG"
     */
    Heartbeat
};

/**
 * @brief Normalized book side.
 */
enum class NormalizedSide {
    Unknown = 0,
    Bid,
    Ask
};

/**
 * @brief One price level in an order book snapshot.
 */
struct BookLevel {
    double price{0.0};
    double size{0.0};
};

/**
 * @brief One incremental price level update.
 *
 * For Polymarket price_change:
 *
 *     size == 0 means remove this price level.
 *     size > 0 means insert/update this price level.
 */
struct PriceLevelChange {
    NormalizedSide side{NormalizedSide::Unknown};
    double price{0.0};
    double size{0.0};
};

/**
 * @brief Internal normalized event.
 *
 * This is the output of EventNormalizer.
 *
 * The first version intentionally stores all possible lightweight payload
 * fields in one struct instead of using std::variant. That keeps the MVP easy
 * to inspect and test.
 *
 * Downstream State layer should switch based on event_type.
 */
struct NormalizedEvent {
    /**
     * @brief Raw packet identity copied from RawPacketHeader.
     */
    std::uint64_t packet_id{0};

    /**
     * @brief Original receive timestamps copied from RawPacketHeader.
     */
    std::uint64_t recv_wall_ns{0};
    std::uint64_t recv_monotonic_ns{0};

    /**
     * @brief Source id copied from RawPacketHeader.
     */
    SourceId source_id{SourceId::Unknown};

    /**
     * @brief Internal event category.
     */
    NormalizedEventType event_type{NormalizedEventType::Unknown};

    /**
     * @brief Venue-specific raw type string.
     *
     * Examples:
     *
     *     "book"
     *     "price_change"
     *     "best_bid_ask"
     *     "market_resolved"
     *     "PONG"
     */
    std::string raw_type;

    /**
     * @brief Primary entity id for downstream state.
     *
     * For Polymarket Market channel this should usually be asset_id.
     *
     * Some events may instead use condition_id or market_id if asset_id is not
     * available.
     */
    std::string entity_id;

    /**
     * @brief Optional Polymarket identifiers.
     */
    std::string asset_id;
    std::string market_id;
    std::string condition_id;

    /**
     * @brief Event timestamp if provided by source.
     *
     * This is source-provided event time, not local receive time.
     */
    std::uint64_t event_ts{0};

    /**
     * @brief Snapshot payload.
     *
     * Used by Snapshot/book events.
     */
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;

    /**
     * @brief Delta payload.
     *
     * Used by Delta/price_change events.
     */
    std::vector<PriceLevelChange> changes;

    /**
     * @brief Best bid/ask status fields.
     *
     * Used by best_bid_ask events if present.
     */
    std::optional<double> best_bid;
    std::optional<double> best_ask;

    /**
     * @brief Tick size if present.
     */
    std::optional<double> tick_size;

    /**
     * @brief Market resolution payload if present.
     */
    std::string winning_asset_id;

    /**
     * @brief Optional warning marker.
     *
     * This is not a fatal decode error. It means normalization succeeded but
     * something was incomplete or unusual.
     */
    std::vector<std::string> warnings;
};

/**
 * @brief Result of normalizing one RawPacket.
 *
 * A single RawPacket can produce multiple NormalizedEvent objects because real
 * Polymarket payloads may be array-wrapped:
 *
 *     [{"type":"book", ...}]
 *
 * Future payloads may contain arrays with more than one object.
 */
struct NormalizationResult {
    std::vector<NormalizedEvent> events;
    std::vector<std::string> warnings;

    /**
     * @brief Fatal error, if normalization could not proceed.
     *
     * Unknown event type is not fatal.
     */
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

/**
 * @brief Convert decoded JSON/control payload into internal normalized events.
 *
 * EventNormalizer does venue-specific semantic mapping.
 *
 * It does NOT:
 *
 * - parse raw JSON text,
 * - write raw logs,
 * - update order books,
 * - perform consistency checks,
 * - reconnect sources.
 *
 * It only maps decoded payloads into internal event envelopes.
 */
class EventNormalizer {
public:
    using Json = boost::json::value;

    /**
     * @brief Normalize a JSON payload from one RawPacket.
     *
     * Supports both:
     *
     * - object payload:
     *       {"type":"book", ...}
     *
     * - array wrapper:
     *       [{"type":"book", ...}]
     *
     * @param packet RawPacket metadata source.
     * @param json Decoded JSON object or array.
     *
     * @return NormalizationResult containing zero or more events.
     */
    [[nodiscard]] NormalizationResult normalize_json(
        const RawPacket& packet,
        const Json& json
    ) const;

    /**
     * @brief Normalize a non-JSON control payload.
     *
     * Examples:
     *
     *     PONG
     *     pong
     *     "PONG"
     *
     * @param packet RawPacket metadata source.
     * @param payload Original non-JSON/control payload.
     *
     * @return Heartbeat event if recognized; Unknown otherwise.
     */
    [[nodiscard]] NormalizationResult normalize_control(
        const RawPacket& packet,
        const std::string& payload
    ) const;

private:
    [[nodiscard]] NormalizedEvent make_base_event(
        const RawPacket& packet
    ) const;

    [[nodiscard]] NormalizedEvent normalize_one_object(
        const RawPacket& packet,
        const Json& object
    ) const;

    [[nodiscard]] NormalizedEvent normalize_book(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_price_change(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_best_bid_ask(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_tick_size_change(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_lifecycle(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_trade(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_unknown(
        const RawPacket& packet,
        const Json& object,
        std::string raw_type
    ) const;
};

/**
 * @brief Convert event type to string for tests/tools.
 */
[[nodiscard]] std::string to_string(NormalizedEventType type);

/**
 * @brief Convert side to string for tests/tools.
 */
[[nodiscard]] std::string to_string(NormalizedSide side);

}  // namespace trading_engine::feed

#pragma once

#include "DecodeTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::decode {

/**
 * @brief Normalized internal event category.
 *
 * This is deliberately much smaller than Polymarket's raw event space.
 */
enum class NormalizedEventType {
    Unknown = 0,
    Snapshot,
    Delta,
    StatusChange,
    LifecycleEvent,
    TradeEvent,
    Heartbeat,
    DecodeError
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
 */
struct PriceLevelChange {
    NormalizedSide side{NormalizedSide::Unknown};
    double price{0.0};
    double size{0.0};
};

/**
 * @brief Internal normalized event emitted by the Decode layer.
 */
struct NormalizedEvent {
    std::uint64_t packet_id{0};

    std::uint64_t recv_wall_ns{0};
    std::uint64_t recv_monotonic_ns{0};

    SourceId source_id{SourceId::Unknown};

    NormalizedEventType event_type{NormalizedEventType::Unknown};

    std::string raw_type;
    std::string entity_id;

    std::string asset_id;
    std::string market_id;
    std::string condition_id;

    std::uint64_t event_ts{0};

    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;

    std::vector<PriceLevelChange> changes;

    std::optional<double> best_bid;
    std::optional<double> best_ask;

    std::optional<double> tick_size;

    std::string winning_asset_id;

    std::vector<std::string> warnings;
};

[[nodiscard]] inline std::string to_string(NormalizedEventType type) {
    switch (type) {
        case NormalizedEventType::Snapshot:
            return "Snapshot";
        case NormalizedEventType::Delta:
            return "Delta";
        case NormalizedEventType::StatusChange:
            return "StatusChange";
        case NormalizedEventType::LifecycleEvent:
            return "LifecycleEvent";
        case NormalizedEventType::TradeEvent:
            return "TradeEvent";
        case NormalizedEventType::Heartbeat:
            return "Heartbeat";
        case NormalizedEventType::DecodeError:
            return "DecodeError";
        case NormalizedEventType::Unknown:
        default:
            return "Unknown";
    }
}

[[nodiscard]] inline std::string to_string(NormalizedSide side) {
    switch (side) {
        case NormalizedSide::Bid:
            return "Bid";
        case NormalizedSide::Ask:
            return "Ask";
        case NormalizedSide::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::decode

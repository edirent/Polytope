#include "state/shard/LOBShard.h"

#include "state/chain/ConfirmedTradeState.h"
#include "state/quality/DataQualityGate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace trading_engine::state {

namespace {

[[nodiscard]] StateApplyResult make_noop_result(
    std::string entity_id,
    std::string message,
    bool state_changed,
    std::uint64_t global_hash
) {
    StateApplyResult out;
    out.code = StateApplyCode::Noop;
    out.entity_id = std::move(entity_id);
    out.message = std::move(message);
    out.state_changed = state_changed;
    out.global_hash = global_hash;
    return out;
}

[[nodiscard]] StateApplyResult from_book_result(
    const BookApplyResult& result
) {
    StateApplyResult out;
    out.code = result.state_code;
    out.entity_id = result.entity_id;
    out.message = result.message;
    out.state_changed = result.state_changed;
    out.entity_hash = result.entity_hash;
    out.global_hash = result.global_hash;
    return out;
}

[[nodiscard]] std::int64_t price_to_tick(
    const EntityState& entity,
    double price
) noexcept {
    if (entity.book.tick_size && *entity.book.tick_size > 0.0) {
        return static_cast<std::int64_t>(
            std::llround(price / *entity.book.tick_size)
        );
    }

    return static_cast<std::int64_t>(
        std::llround(price * static_cast<double>(kDefaultPriceScale))
    );
}

[[nodiscard]] PriceLevel make_price_level(
    const EntityState& entity,
    double price,
    double size
) noexcept {
    PriceLevel level;
    level.price_tick = price_to_tick(entity, price);
    level.price = price;
    level.size = size;
    return level;
}

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

}  // namespace

LOBShard::LOBShard(std::uint32_t shard_id)
    : shard_id_(shard_id),
      lob_writer_(&book_store_) {}

std::uint32_t LOBShard::shard_id() const noexcept {
    return shard_id_;
}

StateApplyResult LOBShard::apply(const MarketStateEvent& event) {
    StateApplyResult result;

    switch (event.type) {
        case MarketStateEventType::WsBookSnapshot:
        case MarketStateEventType::WsBookDelta:
        case MarketStateEventType::WsHeartbeat:
            result = apply_book_event(event);
            break;

        case MarketStateEventType::WsLifecycle:
            result = apply_book_event(event);
            apply_settlement_event(event);
            break;

        case MarketStateEventType::ChainConfirmedFill:
        case MarketStateEventType::ChainRemovedFill:
            result = apply_chain_event(event);
            break;

        case MarketStateEventType::ChainSettlement:
            result = apply_settlement_event(event);
            break;

        case MarketStateEventType::DataQualityUpdate:
        default:
            result = make_noop_result(
                asset_id_for_event(event),
                "event ignored by LOBShard",
                false,
                book_store_.global_hash()
            );
            break;
    }

    const auto publish_start_ns = now_ns();
    publish_for_event(event);
    const auto publish_end_ns = now_ns();
    result.snapshot_publish_ns = checked_sub(
        publish_end_ns,
        publish_start_ns
    );
    return result;
}

StateQueryResult<MarketStateSnapshot> LOBShard::snapshot(
    const std::string& asset_id
) const {
    return snapshot_publisher_.read(asset_id);
}

const ConfirmedTradeState* LOBShard::confirmed_trade_state(
    const std::string& asset_id
) const noexcept {
    return chain_writer_.get(asset_id);
}

const SettlementState* LOBShard::settlement_state(
    const std::string& market_id
) const noexcept {
    return settlement_writer_.get(market_id);
}

const BookQualityState& LOBShard::quality() const noexcept {
    return quality_;
}

std::uint64_t LOBShard::book_hash() const noexcept {
    return book_store_.global_hash();
}

StateApplyResult LOBShard::apply_book_event(
    const MarketStateEvent& event
) {
    return from_book_result(lob_writer_.apply(event));
}

StateApplyResult LOBShard::apply_chain_event(
    const MarketStateEvent& event
) {
    const ChainApplyResult result = chain_writer_.apply(event);
    return make_noop_result(
        result.entity_id,
        result.message,
        result.state_changed,
        book_store_.global_hash()
    );
}

StateApplyResult LOBShard::apply_settlement_event(
    const MarketStateEvent& event
) {
    SettlementApplyResult result;
    const std::string market_id = market_id_for_event(event);

    if (event.type == MarketStateEventType::WsLifecycle &&
        !event.ws_event.winning_asset_id.empty()) {
        result = settlement_writer_.mark_resolved(
            market_id,
            event.ws_event.winning_asset_id,
            event.ws_event.packet_id
        );
    } else if (event.type == MarketStateEventType::WsLifecycle) {
        result = settlement_writer_.mark_closed(
            market_id,
            event.ws_event.packet_id
        );
    } else {
        result = settlement_writer_.mark_closed(
            market_id,
            event.source_sequence
        );
    }

    return make_noop_result(
        market_id,
        result.message,
        result.state_changed,
        book_store_.global_hash()
    );
}

void LOBShard::publish_for_event(const MarketStateEvent& event) {
    const std::string asset_id = asset_id_for_event(event);
    if (asset_id.empty()) {
        return;
    }

    refresh_quality(asset_id);
    publish_asset_snapshot(asset_id);
}

void LOBShard::publish_asset_snapshot(const std::string& asset_id) {
    const EntityState* entity = book_store_.get(asset_id);
    if (!entity) {
        return;
    }

    MarketStateSnapshot snapshot;
    snapshot.entity_id = entity->entity_id;
    snapshot.market_id = entity->market_id;
    snapshot.version = entity->last_packet_id;
    snapshot.live = entity->status == EntityStatus::Live;
    snapshot.recovering = entity->recovering ||
        entity->status == EntityStatus::Recovering;
    snapshot.closed = entity->closed || entity->status == EntityStatus::Closed;
    snapshot.resolved = entity->book.resolved;
    snapshot.crossed = entity->book.crossed;
    snapshot.has_bid = entity->book.best_bid.has_value();
    snapshot.has_ask = entity->book.best_ask.has_value();

    if (entity->book.best_bid) {
        snapshot.best_bid_tick = price_to_tick(*entity, *entity->book.best_bid);
    }

    if (entity->book.best_ask) {
        snapshot.best_ask_tick = price_to_tick(*entity, *entity->book.best_ask);
    }

    std::uint32_t copied = 0;
    for (const auto& [price, size] : entity->book.bids) {
        if (copied >= kMaxSnapshotDepth) {
            break;
        }
        snapshot.bids[copied] = make_price_level(*entity, price, size);
        ++copied;
    }
    snapshot.bid_count = copied;

    copied = 0;
    for (const auto& [price, size] : entity->book.asks) {
        if (copied >= kMaxSnapshotDepth) {
            break;
        }
        snapshot.asks[copied] = make_price_level(*entity, price, size);
        ++copied;
    }
    snapshot.ask_count = copied;

    snapshot.winning_asset_id = entity->book.winning_asset_id;
    snapshot.state_hash = book_store_.state_hash(asset_id);

    if (const auto* chain = chain_writer_.get(asset_id)) {
        snapshot.has_confirmed_trade = chain->has_last_trade;
        snapshot.last_trade_price_tick = chain->last_trade_price_tick;
        snapshot.last_trade_size_lots = chain->last_trade_size_lots;
        snapshot.last_taker_side = chain->last_taker_side;
        snapshot.confirmed_buy_lots_2s = chain->confirmed_buy_lots_2s;
        snapshot.confirmed_sell_lots_2s = chain->confirmed_sell_lots_2s;
        snapshot.confirmed_buy_lots_10s = chain->confirmed_buy_lots_10s;
        snapshot.confirmed_sell_lots_10s = chain->confirmed_sell_lots_10s;
    }

    snapshot.quality = quality_.quality;
    snapshot.usable_for_depth = quality_.usable_for_depth;
    snapshot.usable_for_signal = quality_.usable_for_signal;

    snapshot_publisher_.publish(snapshot);
}

void LOBShard::refresh_quality(const std::string& asset_id) {
    DataQualityInput input;
    input.entity = book_store_.get(asset_id);
    input.confirmed_trade_state = chain_writer_.get(asset_id);

    if (input.entity) {
        input.now_ns = input.entity->last_update_monotonic_ns;
    }
    if (input.confirmed_trade_state &&
        input.confirmed_trade_state->last_chain_seen_ns > input.now_ns) {
        input.now_ns = input.confirmed_trade_state->last_chain_seen_ns;
    }

    DataQualityGate gate;
    quality_ = gate.evaluate(input);
}

std::string LOBShard::asset_id_for_event(
    const MarketStateEvent& event
) const {
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

std::string LOBShard::market_id_for_event(
    const MarketStateEvent& event
) const {
    if (!event.market_id.empty()) {
        return event.market_id;
    }
    if (!event.ws_event.market_id.empty()) {
        return event.ws_event.market_id;
    }
    return event.chain_fill.market_id;
}

}  // namespace trading_engine::state

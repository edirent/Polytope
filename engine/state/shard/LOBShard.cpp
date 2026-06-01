#include "state/shard/LOBShard.h"

#include "state/chain/ConfirmedTradeState.h"
#include "state/quality/DataQualityGate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace trading_engine::state {

namespace {

constexpr std::uint64_t kLightHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kLightHashPrime = 1099511628211ULL;

void mix_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= kLightHashPrime;
    }
}

void mix_string(std::uint64_t& hash, const std::string& value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= kLightHashPrime;
    }
}

std::uint64_t status_flags_for(
    const EntityState* entity,
    const BookQualityState& quality
) noexcept {
    std::uint64_t flags = 0;
    if (entity) {
        flags |= static_cast<std::uint64_t>(entity->status) & 0xffU;
        flags |= entity->initialized ? (1ULL << 8U) : 0ULL;
        flags |= entity->recovering ? (1ULL << 9U) : 0ULL;
        flags |= entity->inconsistent ? (1ULL << 10U) : 0ULL;
        flags |= entity->closed ? (1ULL << 11U) : 0ULL;
        flags |= entity->book.crossed ? (1ULL << 12U) : 0ULL;
        flags |= entity->book.resolved ? (1ULL << 13U) : 0ULL;
    }
    flags |= (static_cast<std::uint64_t>(quality.quality) & 0xffU) << 16U;
    flags |= quality.ws_live ? (1ULL << 24U) : 0ULL;
    flags |= quality.chain_live ? (1ULL << 25U) : 0ULL;
    flags |= quality.usable_for_depth ? (1ULL << 26U) : 0ULL;
    flags |= quality.usable_for_signal ? (1ULL << 27U) : 0ULL;
    return flags;
}

std::uint64_t snapshot_version_hash_for(
    const std::string& entity_id,
    std::uint64_t book_version,
    std::uint64_t chain_version,
    std::uint64_t quality_version,
    std::uint64_t last_ws_packet_id,
    std::uint64_t last_chain_block,
    std::uint64_t status_flags
) noexcept {
    std::uint64_t hash = kLightHashOffset;
    mix_string(hash, entity_id);
    mix_u64(hash, book_version);
    mix_u64(hash, chain_version);
    mix_u64(hash, quality_version);
    mix_u64(hash, last_ws_packet_id);
    mix_u64(hash, last_chain_block);
    mix_u64(hash, status_flags);
    return hash;
}

bool quality_equal(
    const BookQualityState& left,
    const BookQualityState& right
) noexcept {
    return left.quality == right.quality &&
           left.ws_live == right.ws_live &&
           left.chain_live == right.chain_live &&
           left.last_ws_recv_ns == right.last_ws_recv_ns &&
           left.last_chain_seen_ns == right.last_chain_seen_ns &&
           left.ws_decode_errors_recent == right.ws_decode_errors_recent &&
           left.state_errors_recent == right.state_errors_recent &&
           left.chain_decode_errors_recent == right.chain_decode_errors_recent &&
           left.chain_ws_mismatch_count_recent ==
               right.chain_ws_mismatch_count_recent &&
           left.usable_for_depth == right.usable_for_depth &&
           left.usable_for_signal == right.usable_for_signal;
}

[[nodiscard]] StateApplyResult make_noop_result(
    std::string entity_id,
    std::string message,
    bool state_changed
) {
    StateApplyResult out;
    out.code = StateApplyCode::Noop;
    out.entity_id = std::move(entity_id);
    out.message = std::move(message);
    out.state_changed = state_changed;
    out.mutation.state_changed = state_changed;
    out.mutation.publish_required = state_changed;
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
    out.mutation = result.mutation;
    out.book_version = result.book_version;
    out.chain_version = result.chain_version;
    out.quality_version = result.quality_version;
    out.snapshot_version_hash = result.snapshot_version_hash;
    out.entity_hash = result.entity_hash;
    out.global_hash = result.global_hash;
    out.cheap_fingerprint = result.cheap_fingerprint;
    out.full_hash_computed = result.full_hash_computed;
    out.make_result_ns = result.make_result_ns;
    out.hash_entity_ns = result.hash_entity_ns;
    out.hash_global_ns = result.hash_global_ns;
    out.snapshot_build_ns = result.snapshot_build_ns;
    out.snapshot_publish_ns = result.snapshot_publish_ns;
    out.hash_cache_hits = result.hash_cache_hits;
    out.hash_cache_misses = result.hash_cache_misses;
    out.snapshot_published = result.snapshot_published;
    return out;
}

[[nodiscard]] std::int64_t price_to_tick(
    const EntityState& entity,
    double price
) noexcept {
    (void)entity;
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

LOBShard::LOBShard(
    std::uint32_t shard_id,
    StateRuntimeConfig runtime_config
) : shard_id_(shard_id),
      runtime_config_(runtime_config),
      book_store_(runtime_config_),
      lob_writer_(&book_store_) {}

std::uint32_t LOBShard::shard_id() const noexcept {
    return shard_id_;
}

const StateRuntimeConfig& LOBShard::runtime_config() const noexcept {
    return runtime_config_;
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
            if (const auto settlement = apply_settlement_event(event);
                settlement.state_changed) {
                result.state_changed = true;
                result.mutation.state_changed = true;
                result.mutation.lifecycle_changed = true;
                result.mutation.publish_required = true;
                result.mutation.kind = StateMutationKind::Lifecycle;
            }
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
                false
            );
            break;
    }

    const std::string asset_id = asset_id_for_event(event);
    if (!asset_id.empty()) {
        const BookQualityState before_quality = quality_;
        refresh_quality(asset_id);
        if (!quality_equal(before_quality, quality_)) {
            ++quality_version_;
            result.quality_version = quality_version_;
            result.mutation.quality_changed = true;
            result.mutation.state_changed = true;
            result.state_changed = true;
            if (result.mutation.kind == StateMutationKind::None) {
                result.mutation.kind = StateMutationKind::Quality;
            }
            result.mutation.publish_required = true;
        } else {
            result.quality_version = quality_version_;
        }
    }

    const EntityState* entity = asset_id.empty()
        ? nullptr
        : book_store_.get(asset_id);
    const ConfirmedTradeState* chain_state = asset_id.empty()
        ? nullptr
        : chain_writer_.get(asset_id);

    result.snapshot_version_hash = snapshot_version_hash_for(
        !asset_id.empty() ? asset_id : result.entity_id,
        result.book_version,
        result.chain_version,
        result.quality_version,
        entity ? entity->last_packet_id : result.book_version,
        chain_state ? chain_state->last_block_number : 0,
        status_flags_for(entity, quality_)
    );
    result.cheap_fingerprint = result.snapshot_version_hash;

    if (!asset_id.empty() && result.full_hash_computed &&
        result.entity_hash != 0) {
        cached_legacy_book_hash_[asset_id] = result.entity_hash;
    }

    if (should_publish_after_apply(event, result)) {
        const auto build_start_ns = now_ns();
        const auto context = build_snapshot_context(event, result);
        const auto build_end_ns = now_ns();
        result.snapshot_build_ns = checked_sub(
            build_end_ns,
            build_start_ns
        );

        if (context.asset_id.empty()) {
            return result;
        }

        const auto publish_start_ns = now_ns();
        result.snapshot_published = publish_asset_snapshot(context);
        const auto publish_end_ns = now_ns();
        if (result.snapshot_published) {
            result.snapshot_publish_ns = checked_sub(
                publish_end_ns,
                publish_start_ns
            );
        }
    }
    return result;
}

StateQueryResult<MarketStateSnapshot> LOBShard::snapshot(
    const std::string& asset_id
) const {
    return snapshot_publisher_.read(asset_id);
}

std::uint16_t LOBShard::snapshots(
    std::span<const std::string* const> asset_ids,
    MarketStateSnapshot* out,
    std::uint16_t max_out
) const {
    return snapshot_publisher_.read_many(asset_ids, out, max_out);
}

std::uint16_t LOBShard::depth_views(
    std::span<const std::string* const> asset_ids,
    std::span<const std::uint32_t> asset_indices,
    MarketDepthView* out,
    std::uint16_t max_out
) const {
    return snapshot_publisher_.read_depth_many(
        asset_ids,
        asset_indices,
        out,
        max_out
    );
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

std::uint64_t LOBShard::book_hash() const {
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
    StateApplyResult out = make_noop_result(
        result.entity_id,
        result.message,
        result.state_changed
    );
    out.chain_version = chain_writer_.version(result.entity_id);
    out.mutation.state_changed = result.state_changed;
    out.mutation.chain_changed = result.state_changed;
    out.mutation.publish_required = result.state_changed;
    out.mutation.kind =
        event.type == MarketStateEventType::ChainRemovedFill
            ? StateMutationKind::ChainRemovedFill
            : StateMutationKind::ChainFill;
    return out;
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
        result.state_changed
    );
}

bool LOBShard::should_publish_after_apply(
    const MarketStateEvent& event,
    const StateApplyResult& result
) const noexcept {
    (void)event;
    return result.mutation.publish_required;
}

SnapshotBuildContext LOBShard::build_snapshot_context(
    const MarketStateEvent& event,
    const StateApplyResult& result
) const {
    SnapshotBuildContext context;
    context.asset_id = asset_id_for_event(event);
    if (context.asset_id.empty()) {
        context.asset_id = result.entity_id;
    }

    context.book_version = result.book_version;
    context.chain_version = result.chain_version;
    context.quality_version = result.quality_version;
    context.snapshot_version_hash = result.snapshot_version_hash;

    if (result.full_hash_computed && result.entity_hash != 0) {
        context.full_entity_hash = result.entity_hash;
        return context;
    }

    const auto cached = cached_legacy_book_hash_.find(context.asset_id);
    if (cached != cached_legacy_book_hash_.end()) {
        context.full_entity_hash = cached->second;
    }

    return context;
}

bool LOBShard::publish_asset_snapshot(const SnapshotBuildContext& context) {
    const EntityState* entity = book_store_.get(context.asset_id);
    if (!entity) {
        return false;
    }

    MarketStateSnapshot snapshot;
    snapshot.entity_id = entity->entity_id;
    snapshot.market_id = entity->market_id;
    snapshot.version = entity->last_packet_id;
    snapshot.last_book_update_ns = entity->last_update_monotonic_ns;
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
    snapshot.snapshot_version_hash = context.snapshot_version_hash;
    snapshot.state_hash = context.full_entity_hash.value_or(0);

    if (const auto* chain = chain_writer_.get(context.asset_id)) {
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
    return true;
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

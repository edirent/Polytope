#include "state/chain/ChainStateWriter.h"

#include "chain_confirm/ClassifiedFillRecord.h"

#include <algorithm>
#include <utility>

namespace trading_engine::state {

namespace {

constexpr std::uint64_t kTwoSecondsNs = 2'000'000'000ULL;
constexpr std::uint64_t kTenSecondsNs = 10'000'000'000ULL;

[[nodiscard]] AggressorSide map_direction(
    chain_confirm::ConfirmedDirection direction
) noexcept {
    switch (direction) {
        case chain_confirm::ConfirmedDirection::BuyAggressor:
            return AggressorSide::Buy;
        case chain_confirm::ConfirmedDirection::SellAggressor:
            return AggressorSide::Sell;
        case chain_confirm::ConfirmedDirection::Unknown:
        default:
            return AggressorSide::Unknown;
    }
}

[[nodiscard]] bool is_ambiguous(
    const chain_confirm::ClassifiedFillRecord& fill
) noexcept {
    return fill.classification ==
               chain_confirm::FillClassification::AmbiguousFill ||
           fill.mapping_status ==
               chain_confirm::FillMappingStatus::AmbiguousFill;
}

[[nodiscard]] bool has_chain_fill(
    const chain_confirm::ClassifiedFillRecord& fill
) noexcept {
    return !fill.fill_id.empty() ||
           fill.classification !=
               chain_confirm::FillClassification::Unknown ||
           fill.mapping_status !=
               chain_confirm::FillMappingStatus::UnmappedFill;
}

}  // namespace

ChainApplyResult ChainStateWriter::apply(const MarketStateEvent& event) {
    if (event.type == MarketStateEventType::ChainRemovedFill ||
        event.chain_fill.removed ||
        event.chain_fill.classification ==
            chain_confirm::FillClassification::ChainRemoved) {
        return apply_removed(event);
    }

    if (event.type == MarketStateEventType::ChainConfirmedFill ||
        has_chain_fill(event.chain_fill)) {
        return apply_confirmed(event);
    }

    ChainApplyResult out;
    out.code = ChainApplyCode::IgnoredNonChainEvent;
    out.message = "non-chain event ignored by ChainStateWriter";
    return out;
}

const ConfirmedTradeState* ChainStateWriter::get(
    const std::string& entity_id
) const noexcept {
    const auto it = states_.find(entity_id);
    if (it == states_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool ChainStateWriter::contains(const std::string& entity_id) const noexcept {
    return states_.find(entity_id) != states_.end();
}

std::uint64_t ChainStateWriter::version(
    const std::string& entity_id
) const noexcept {
    const auto* state = get(entity_id);
    return state ? state->version : 0;
}

ChainApplyResult ChainStateWriter::apply_confirmed(
    const MarketStateEvent& event
) {
    const auto& fill = event.chain_fill;
    const std::string entity_id = resolve_entity_id(event);

    ChainApplyResult out;
    out.entity_id = entity_id;
    out.fill_id = fill.fill_id;

    if (entity_id.empty()) {
        out.code = ChainApplyCode::MissingEntityId;
        out.message = "chain fill missing asset_id and market_id";
        return out;
    }

    ConfirmedTradeState& state = states_[entity_id];
    state.last_block_number = std::max(
        state.last_block_number,
        fill.block_number
    );
    state.last_chain_seen_ns = fill.chain_seen_monotonic_ns;

    if (!fill.fill_id.empty()) {
        const auto existing = fills_.find(fill.fill_id);
        if (existing != fills_.end() && !existing->second.removed) {
            ++state.version;
            refresh_windows(entity_id, fill.chain_seen_monotonic_ns);
            out.code = ChainApplyCode::DuplicateFill;
            out.state_changed = true;
            out.message = "duplicate chain fill ignored";
            return out;
        }
    }

    if (is_ambiguous(fill)) {
        ++state.version;
        ++state.ambiguous_fill_count_recent;
        refresh_windows(entity_id, fill.chain_seen_monotonic_ns);
        out.code = ChainApplyCode::AmbiguousFill;
        out.state_changed = true;
        out.message = "ambiguous chain fill counted without direction";
        return out;
    }

    const AggressorSide side = map_direction(fill.direction);
    if (side == AggressorSide::Unknown) {
        ++state.version;
        ++state.unknown_fill_count_recent;
        refresh_windows(entity_id, fill.chain_seen_monotonic_ns);
        out.code = ChainApplyCode::UnknownDirection;
        out.state_changed = true;
        out.message = "unknown chain fill direction counted without volume";
        return out;
    }

    ++state.version;
    state.has_last_trade = true;
    state.last_trade_price_tick = fill.price_tick;
    state.last_trade_size_lots = fill.size_lots;
    state.last_taker_side = side;

    if (!fill.fill_id.empty()) {
        fills_[fill.fill_id] = AppliedFill{
            entity_id,
            side,
            fill.size_lots,
            fill.chain_seen_monotonic_ns,
            true,
            false
        };
        windows_[entity_id].add(
            fill.fill_id,
            fill.chain_seen_monotonic_ns,
            side,
            fill.size_lots
        );
    }

    refresh_windows(entity_id, fill.chain_seen_monotonic_ns);

    out.code = ChainApplyCode::Applied;
    out.state_changed = true;
    out.message = "chain fill applied to confirmed trade state";
    return out;
}

ChainApplyResult ChainStateWriter::apply_removed(
    const MarketStateEvent& event
) {
    const auto& fill = event.chain_fill;
    const std::string entity_id = resolve_entity_id(event);

    ChainApplyResult out;
    out.code = ChainApplyCode::RemovedFill;
    out.entity_id = entity_id;
    out.fill_id = fill.fill_id;
    out.state_changed = true;
    out.message = "chain fill removed by reorg";

    if (entity_id.empty()) {
        out.code = ChainApplyCode::MissingEntityId;
        out.state_changed = false;
        out.message = "removed chain fill missing asset_id and market_id";
        return out;
    }

    ConfirmedTradeState& state = states_[entity_id];
    ++state.version;
    ++state.removed_fill_count_recent;
    state.last_block_number = std::max(
        state.last_block_number,
        fill.block_number
    );
    state.last_chain_seen_ns = fill.chain_seen_monotonic_ns;

    const auto it = fills_.find(fill.fill_id);
    if (it != fills_.end() && !it->second.removed) {
        it->second.removed = true;
        if (it->second.counted_volume) {
            const bool removed_from_window =
                windows_[it->second.entity_id].mark_removed(fill.fill_id);
            (void)removed_from_window;
        }
    }

    refresh_windows(entity_id, fill.chain_seen_monotonic_ns);
    return out;
}

std::string ChainStateWriter::resolve_entity_id(
    const MarketStateEvent& event
) const {
    if (!event.chain_fill.asset_id.empty()) {
        return event.chain_fill.asset_id;
    }
    if (!event.asset_id.empty()) {
        return event.asset_id;
    }
    if (!event.chain_fill.market_id.empty()) {
        return event.chain_fill.market_id;
    }
    return event.market_id;
}

void ChainStateWriter::refresh_windows(
    const std::string& entity_id,
    std::uint64_t now_ns
) {
    ConfirmedTradeState& state = states_[entity_id];
    const ConfirmedTradeWindow& window = windows_[entity_id];

    const auto two_seconds = window.totals(now_ns, kTwoSecondsNs);
    const auto ten_seconds = window.totals(now_ns, kTenSecondsNs);

    state.confirmed_buy_lots_2s = two_seconds.buy_lots;
    state.confirmed_sell_lots_2s = two_seconds.sell_lots;
    state.confirmed_buy_lots_10s = ten_seconds.buy_lots;
    state.confirmed_sell_lots_10s = ten_seconds.sell_lots;
}

const char* to_string(ChainApplyCode code) noexcept {
    switch (code) {
        case ChainApplyCode::Applied:
            return "Applied";
        case ChainApplyCode::IgnoredNonChainEvent:
            return "IgnoredNonChainEvent";
        case ChainApplyCode::MissingEntityId:
            return "MissingEntityId";
        case ChainApplyCode::UnknownDirection:
            return "UnknownDirection";
        case ChainApplyCode::AmbiguousFill:
            return "AmbiguousFill";
        case ChainApplyCode::RemovedFill:
            return "RemovedFill";
        case ChainApplyCode::DuplicateFill:
            return "DuplicateFill";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state

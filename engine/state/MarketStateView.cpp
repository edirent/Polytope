#include "state/MarketStateView.h"

#include <algorithm>
#include <utility>

namespace trading_engine::state {

namespace {

template <typename T>
StateQueryResult<T> error_result(
    std::string entity_id,
    std::uint64_t version,
    StateQueryError error
) {
    StateQueryResult<T> result;
    result.ok = false;
    result.error = error;
    result.entity_id = std::move(entity_id);
    result.version = version;
    return result;
}

template <typename T>
StateQueryResult<T> ok_result(
    std::string entity_id,
    std::uint64_t version,
    T value
) {
    StateQueryResult<T> result;
    result.ok = true;
    result.error = StateQueryError::None;
    result.entity_id = std::move(entity_id);
    result.version = version;
    result.value = std::move(value);
    return result;
}

[[nodiscard]] bool has_book_levels(
    const MarketStateSnapshot& snapshot
) noexcept {
    return snapshot.bid_count > 0 || snapshot.ask_count > 0;
}

}  // namespace

MarketStateView::MarketStateView(
    const MarketStateStore& store
) noexcept
    : store_(store) {}

bool MarketStateView::exists(const std::string& entity_id) const {
    return store_.exists(entity_id);
}

bool MarketStateView::exists(std::uint64_t entity_id) const {
    return exists(to_entity_id(entity_id));
}

StateQueryResult<PriceLevel> MarketStateView::get_best_bid(
    const std::string& entity_id
) const {
    const auto snapshot = store_.get_snapshot(entity_id);
    if (!snapshot.ok) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            snapshot.error
        );
    }

    const auto executable = executable_error(snapshot.value);
    if (executable != StateQueryError::None) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            executable
        );
    }

    if (!snapshot.value.has_bid || snapshot.value.bid_count == 0) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            StateQueryError::MissingBid
        );
    }

    return ok_result(
        entity_id,
        snapshot.version,
        snapshot.value.bids[0]
    );
}

StateQueryResult<PriceLevel> MarketStateView::get_best_bid(
    std::uint64_t entity_id
) const {
    return get_best_bid(to_entity_id(entity_id));
}

StateQueryResult<PriceLevel> MarketStateView::get_best_ask(
    const std::string& entity_id
) const {
    const auto snapshot = store_.get_snapshot(entity_id);
    if (!snapshot.ok) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            snapshot.error
        );
    }

    const auto executable = executable_error(snapshot.value);
    if (executable != StateQueryError::None) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            executable
        );
    }

    if (!snapshot.value.has_ask || snapshot.value.ask_count == 0) {
        return error_result<PriceLevel>(
            entity_id,
            snapshot.version,
            StateQueryError::MissingAsk
        );
    }

    return ok_result(
        entity_id,
        snapshot.version,
        snapshot.value.asks[0]
    );
}

StateQueryResult<PriceLevel> MarketStateView::get_best_ask(
    std::uint64_t entity_id
) const {
    return get_best_ask(to_entity_id(entity_id));
}

StateQueryResult<BestBidAsk> MarketStateView::get_bbo(
    const std::string& entity_id
) const {
    const auto snapshot = store_.get_snapshot(entity_id);
    if (!snapshot.ok) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            snapshot.error
        );
    }

    const auto executable = executable_error(snapshot.value);
    if (executable != StateQueryError::None) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            executable
        );
    }

    if (!has_book_levels(snapshot.value)) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            StateQueryError::EmptyBook
        );
    }

    if (!snapshot.value.has_bid || snapshot.value.bid_count == 0) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            StateQueryError::MissingBid
        );
    }

    if (!snapshot.value.has_ask || snapshot.value.ask_count == 0) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            StateQueryError::MissingAsk
        );
    }

    if (snapshot.value.best_bid_tick > snapshot.value.best_ask_tick) {
        return error_result<BestBidAsk>(
            entity_id,
            snapshot.version,
            StateQueryError::CrossedBook
        );
    }

    BestBidAsk bbo;
    bbo.bid = snapshot.value.bids[0];
    bbo.ask = snapshot.value.asks[0];

    return ok_result(entity_id, snapshot.version, bbo);
}

StateQueryResult<BestBidAsk> MarketStateView::get_bbo(
    std::uint64_t entity_id
) const {
    return get_bbo(to_entity_id(entity_id));
}

StateQueryResult<std::int64_t> MarketStateView::get_mid_tick(
    const std::string& entity_id
) const {
    const auto bbo = get_bbo(entity_id);
    if (!bbo.ok) {
        return error_result<std::int64_t>(
            bbo.entity_id,
            bbo.version,
            bbo.error
        );
    }

    const std::int64_t mid =
        (bbo.value.bid.price_tick + bbo.value.ask.price_tick) / 2;

    return ok_result(entity_id, bbo.version, mid);
}

StateQueryResult<std::int64_t> MarketStateView::get_mid_tick(
    std::uint64_t entity_id
) const {
    return get_mid_tick(to_entity_id(entity_id));
}

StateQueryResult<std::int64_t> MarketStateView::get_spread_tick(
    const std::string& entity_id
) const {
    const auto bbo = get_bbo(entity_id);
    if (!bbo.ok) {
        return error_result<std::int64_t>(
            bbo.entity_id,
            bbo.version,
            bbo.error
        );
    }

    const std::int64_t spread =
        bbo.value.ask.price_tick - bbo.value.bid.price_tick;

    if (spread < 0) {
        return error_result<std::int64_t>(
            entity_id,
            bbo.version,
            StateQueryError::CrossedBook
        );
    }

    return ok_result(entity_id, bbo.version, spread);
}

StateQueryResult<std::int64_t> MarketStateView::get_spread_tick(
    std::uint64_t entity_id
) const {
    return get_spread_tick(to_entity_id(entity_id));
}

StateQueryResult<MarketStateSnapshot> MarketStateView::get_snapshot(
    const std::string& entity_id,
    std::uint32_t max_depth
) const {
    if (max_depth == 0 || max_depth > kMaxSnapshotDepth) {
        return error_result<MarketStateSnapshot>(
            entity_id,
            0,
            StateQueryError::InvalidDepth
        );
    }

    auto snapshot = store_.get_snapshot(entity_id);
    if (!snapshot.ok) {
        return snapshot;
    }

    snapshot.value.bid_count = std::min(snapshot.value.bid_count, max_depth);
    snapshot.value.ask_count = std::min(snapshot.value.ask_count, max_depth);
    snapshot.version = snapshot.value.version;
    return snapshot;
}

StateQueryResult<MarketStateSnapshot> MarketStateView::get_snapshot(
    std::uint64_t entity_id,
    std::uint32_t max_depth
) const {
    return get_snapshot(to_entity_id(entity_id), max_depth);
}

std::uint64_t MarketStateView::state_hash(
    const std::string& entity_id
) const noexcept {
    return store_.state_hash(entity_id);
}

std::uint64_t MarketStateView::state_hash(std::uint64_t entity_id) const {
    return state_hash(to_entity_id(entity_id));
}

std::uint64_t MarketStateView::global_hash() const noexcept {
    return store_.global_hash();
}

StateQueryError MarketStateView::executable_error(
    const MarketStateSnapshot& snapshot
) const noexcept {
    if (snapshot.resolved) {
        return StateQueryError::Resolved;
    }

    if (snapshot.closed) {
        return StateQueryError::Closed;
    }

    if (snapshot.recovering) {
        return StateQueryError::Recovering;
    }

    if (snapshot.crossed ||
        (snapshot.has_bid && snapshot.has_ask &&
         snapshot.best_bid_tick > snapshot.best_ask_tick)) {
        return StateQueryError::CrossedBook;
    }

    return StateQueryError::None;
}

std::string to_string(StateQueryError error) {
    switch (error) {
        case StateQueryError::None:
            return "None";
        case StateQueryError::MissingEntity:
            return "MissingEntity";
        case StateQueryError::Recovering:
            return "Recovering";
        case StateQueryError::Closed:
            return "Closed";
        case StateQueryError::Resolved:
            return "Resolved";
        case StateQueryError::EmptyBook:
            return "EmptyBook";
        case StateQueryError::MissingBid:
            return "MissingBid";
        case StateQueryError::MissingAsk:
            return "MissingAsk";
        case StateQueryError::CrossedBook:
            return "CrossedBook";
        case StateQueryError::InvalidDepth:
            return "InvalidDepth";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::state

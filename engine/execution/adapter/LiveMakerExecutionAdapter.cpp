#include "engine/execution/adapter/LiveMakerExecutionAdapter.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace trading_engine::execution {

namespace {

OrderSide order_side_from_quote_side(QuoteSide side) noexcept {
    return side == QuoteSide::Bid ? OrderSide::Buy : OrderSide::Sell;
}

const char* side_suffix(QuoteSide side) noexcept {
    return side == QuoteSide::Bid ? "bid" : "ask";
}

std::uint64_t child_id_for_side(
    std::uint64_t quote_id,
    QuoteSide side
) noexcept {
    return quote_id ^ (side == QuoteSide::Bid ? 0x9e3779b185ebca87ULL
                                              : 0xc2b2ae3d27d4eb4fULL);
}

std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );
    return value;
}

bool resting_status(const std::string& status) {
    return lower_ascii(status) == "live";
}

MakerSubmitResult disabled_submit(std::uint64_t quote_id) {
    return {
        .ok = false,
        .quote_id = quote_id,
        .error = "live maker execution adapter is disabled"
    };
}

}  // namespace

LiveMakerExecutionAdapter::LiveMakerExecutionAdapter(
    LiveExecutionConfig config,
    ILiveOrderSigner* signer,
    ILiveOrderTransport* transport
) : config_(std::move(config)),
    signer_(signer),
    transport_(transport) {}

bool LiveMakerExecutionAdapter::side_allowed(OrderSide side) const noexcept {
    return (side == OrderSide::Buy && config_.allow_buy_orders) ||
        (side == OrderSide::Sell && config_.allow_sell_orders);
}

bool LiveMakerExecutionAdapter::notional_allowed(
    std::int64_t quantity_lots,
    std::int64_t price_tick
) const noexcept {
    if (config_.max_child_notional_tick <= 0) {
        return true;
    }
    if (quantity_lots <= 0 || price_tick <= 0) {
        return false;
    }
    const auto notional =
        static_cast<__int128>(quantity_lots) *
        static_cast<__int128>(price_tick);
    return notional <=
        static_cast<__int128>(config_.max_child_notional_tick);
}

LiveOrderRequest LiveMakerExecutionAdapter::make_request(
    const risk::ApprovedQuote& quote,
    const risk::QuoteLeg& leg,
    QuoteSide side,
    std::uint64_t quote_id
) const {
    return {
        .parent_id = quote.quote_group_id,
        .child_id = child_id_for_side(quote_id, side),
        .client_order_id = std::string{"maker-"} +
            std::to_string(quote.approved_quote_id) +
            "-" +
            side_suffix(side),
        .market_id = leg.market_id,
        .asset_id = leg.asset_id,
        .side = order_side_from_quote_side(side),
        .quantity_lots = leg.quantity_lots,
        .price_tick = leg.price_tick,
        .created_ts_ns = quote.approved_ts_ns,
        .expire_after_ns = quote.expires_at_ns,
        .order_type = config_.default_order_type,
        .post_only = config_.maker_post_only
    };
}

MakerSubmitResult LiveMakerExecutionAdapter::submit_leg(
    const risk::ApprovedQuote& quote,
    const risk::QuoteLeg& leg,
    QuoteSide side,
    std::uint64_t quote_id,
    std::uint64_t
) {
    const auto order_side = order_side_from_quote_side(side);
    if (!side_allowed(order_side)) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker order side disabled"
        };
    }
    if (leg.market_id.empty() || leg.asset_id.empty()) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker quote leg missing market or asset"
        };
    }
    if (leg.quantity_lots <= 0 || leg.price_tick <= 0) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker quote leg has invalid size or price"
        };
    }
    if (!notional_allowed(leg.quantity_lots, leg.price_tick)) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker quote leg notional limit exceeded"
        };
    }

    const auto signed_order =
        signer_->sign_order(make_request(quote, leg, side, quote_id));
    if (!signed_order.ok) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = signed_order.error.empty() ? "live maker signing failed"
                                                : signed_order.error
        };
    }

    const auto transport_result =
        transport_->submit_order(signed_order.order.request_body_json);
    if (!transport_result.ok) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = transport_result.error.empty() ? "live maker submit failed"
                                                    : transport_result.error
        };
    }
    if (config_.maker_require_resting_ack &&
        !resting_status(transport_result.venue_status)) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker order did not rest on book"
        };
    }

    auto venue_order_id = transport_result.venue_order_id.empty()
        ? signed_order.order.venue_order_id_hint
        : transport_result.venue_order_id;
    if (venue_order_id.empty()) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "live maker submit missing venue_order_id"
        };
    }

    accepted_orders_by_group_[quote.quote_group_id].push_back({
        .quote_id = quote_id,
        .side = side,
        .venue_order_id = std::move(venue_order_id)
    });

    return {
        .ok = true,
        .quote_id = quote_id
    };
}

MakerCancelResult LiveMakerExecutionAdapter::cancel_existing_group(
    std::uint64_t quote_group_id
) {
    if (transport_ == nullptr) {
        return {
            .ok = false,
            .error = "missing live order transport"
        };
    }

    const auto it = accepted_orders_by_group_.find(quote_group_id);
    if (it == accepted_orders_by_group_.end() || it->second.empty()) {
        return {
            .ok = false,
            .error = "quote group not active"
        };
    }

    std::uint64_t canceled = 0;
    std::uint64_t quote_id = 0;
    std::string first_error;
    for (const auto& order : it->second) {
        quote_id = order.quote_id;
        const auto result = transport_->cancel_order(order.venue_order_id);
        if (result.ok) {
            ++canceled;
            continue;
        }
        if (first_error.empty()) {
            first_error = result.error.empty() ? "live maker cancel failed"
                                               : result.error;
        }
    }

    if (canceled == it->second.size()) {
        accepted_orders_by_group_.erase(it);
        return {
            .ok = true,
            .quote_id = quote_id
        };
    }

    return {
        .ok = false,
        .quote_id = quote_id,
        .error = first_error.empty() ? "live maker cancel failed"
                                     : first_error
    };
}

MakerSubmitResult LiveMakerExecutionAdapter::submit_approved_quote(
    const risk::ApprovedQuote& quote,
    std::uint64_t now_ns
) {
    const auto quote_id = compute_paper_maker_quote_id(quote);
    if (!config_.enabled) {
        return disabled_submit(quote_id);
    }
    if (signer_ == nullptr) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "missing live order signer"
        };
    }
    if (transport_ == nullptr) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "missing live order transport"
        };
    }
    if (!quote.has_bid && !quote.has_ask) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "approved quote has no active sides"
        };
    }
    if (quote.expires_at_ns != 0 && quote.expires_at_ns <= now_ns) {
        return {
            .ok = false,
            .quote_id = quote_id,
            .error = "approved quote expired"
        };
    }
    if (quote.idempotency_hash != 0 &&
        submitted_idempotency_hashes_.contains(quote.idempotency_hash)) {
        return {
            .ok = true,
            .duplicate_ignored = true,
            .quote_id = quote_id
        };
    }

    MakerSubmitResult result;
    result.quote_id = quote_id;

    if (accepted_orders_by_group_.contains(quote.quote_group_id)) {
        const auto cancel = cancel_existing_group(quote.quote_group_id);
        if (!cancel.ok) {
            result.error = "failed to replace active live quote: " +
                cancel.error;
            return result;
        }
        result.replaced = true;
    }

    std::uint32_t leg_count = 0;
    if (quote.has_bid) {
        ++leg_count;
    }
    if (quote.has_ask) {
        ++leg_count;
    }
    if (leg_count == 0 || leg_count > config_.max_quote_orders_per_group) {
        result.error = "invalid live maker quote leg count";
        return result;
    }

    if (quote.has_bid) {
        const auto bid = submit_leg(
            quote,
            quote.bid,
            QuoteSide::Bid,
            quote_id,
            now_ns
        );
        if (!bid.ok) {
            result.error = bid.error;
            (void)cancel_existing_group(quote.quote_group_id);
            return result;
        }
    }
    if (quote.has_ask) {
        const auto ask = submit_leg(
            quote,
            quote.ask,
            QuoteSide::Ask,
            quote_id,
            now_ns
        );
        if (!ask.ok) {
            result.error = ask.error;
            (void)cancel_existing_group(quote.quote_group_id);
            return result;
        }
    }

    if (quote.idempotency_hash != 0) {
        submitted_idempotency_hashes_.insert(quote.idempotency_hash);
    }
    result.ok = true;
    return result;
}

MakerCancelResult LiveMakerExecutionAdapter::cancel_quote_group(
    std::uint64_t quote_group_id,
    std::uint64_t
) {
    if (!config_.enabled) {
        return {
            .ok = false,
            .error = "live maker execution adapter is disabled"
        };
    }
    return cancel_existing_group(quote_group_id);
}

}  // namespace trading_engine::execution

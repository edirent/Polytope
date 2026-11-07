#include "replay/market_making/LeadLagReplay.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace trading_engine::replay::lead_lag {
namespace {
std::int64_t fee(std::int64_t price, std::int64_t qty, std::int64_t bps) {
    return (price * qty * bps + 9'999) / 10'000;
}
void apply_fill(Ledger& ledger, strategy::Channel c, std::int64_t price,
                std::int64_t qty, std::int64_t charge) {
    const auto ti = strategy::LeadLagEngine::token(c);
    if (strategy::LeadLagEngine::is_bid(c)) {
        ledger.lots[ti] += qty; ledger.cash_tick -= price * qty + charge;
    } else {
        ledger.lots[ti] -= qty; ledger.cash_tick += price * qty - charge;
    }
}
strategy::Channel channel_of(const mm::QuoteIntent& q, const strategy::Market& m) {
    return static_cast<strategy::Channel>((q.asset_index == m.asset_indices[0] ? 0 : 2) + (q.has_bid ? 0 : 1));
}
}

LeadLagReplay::LeadLagReplay(strategy::LeadLagEngine engine, Ledger initial,
                            LatencyConfig latency, risk::QuoteRiskPolicy policy)
    : engine_(std::move(engine)), latency_(latency), policy_(policy),
      initial_(initial), actual_(initial), known_(initial) {
    if (latency.submit_ms < 0 || latency.cancel_ms < 0 || latency.fill_notice_ms < 0 ||
        latency.terminal_reduce_ms < 0 || !std::isfinite(latency.queue_factor) || latency.queue_factor < 0 ||
        latency.queue_factor > 1'000 || latency.maker_fee_bps < 0 || latency.maker_fee_bps > 10'000 ||
        latency.taker_fee_bps < 0 || latency.taker_fee_bps > 10'000 || latency.operating_cost_tick < 0 ||
        initial.cash_tick < 0 || initial.cash_tick > 1'000'000'000'000'000LL ||
        initial.lots[0] < 0 || initial.lots[1] < 0 ||
        initial.lots[0] > engine_.config().max_token_lots || initial.lots[1] > engine_.config().max_token_lots ||
        initial.lots[0] != initial.lots[1])
        throw std::invalid_argument("invalid replay ledger or latency configuration");
    policy_.min_replace_interval_ns = 0;
    policy_.max_quote_age_ns = static_cast<std::uint64_t>(engine_.config().quote_ttl_ms) * 1'000'000ULL;
    policy_.max_book_age_ns = static_cast<std::uint64_t>(engine_.config().max_data_age_ms) * 1'000'000ULL;
    policy_.max_quote_qty_lots = engine_.config().size_lots;
    policy_.max_asset_inventory_lots = engine_.config().max_token_lots;
    policy_.min_edge_to_fair_tick = engine_.config().buffer_tick;
}

void LeadLagReplay::schedule(std::int64_t ms, int priority, std::function<void()> action) {
    if (ms < 0 || (started_ && ms < now_ms_)) throw std::invalid_argument("event scheduled in the past");
    events_.push({ms, priority, sequence_++, std::move(action)});
}
void LeadLagReplay::add_agg_trade(const strategy::AggTrade& t) {
    if (started_) throw std::logic_error("replay input already frozen");
    schedule(t.recv_ms, 1, [this, t] { engine_.on_agg_trade(t); });
}
void LeadLagReplay::add_books(const state::MarketDepthView& up, const state::MarketDepthView& down,
                             std::int64_t event_ms, std::int64_t recv_ms) {
    if (started_ || event_ms < 0 || recv_ms < event_ms) throw std::invalid_argument("invalid book input");
    schedule(recv_ms, 1, [this, up, down, event_ms, recv_ms] {
        if (event_ms < mark_source_ms_) return;
        mark_source_ms_ = event_ms;
        visible_books_ = {up, down}; engine_.on_books(up, down, recv_ms); update_marks();
    });
}
void LeadLagReplay::add_public_trade(const PublicTrade& t) {
    if (started_ || t.event_ms < 0 || t.recv_ms < t.event_ms || t.token < 0 || t.token > 1 ||
        t.quantity_lots <= 0 || t.quantity_lots > 1'000'000 || t.price_tick <= 0 || t.price_tick >= mm::kPriceOneTick)
        throw std::invalid_argument("invalid public trade input");
    // Public trade happens at source time even when its feed arrives later.
    schedule(t.event_ms, 0, [this, t] { trade(t); });
    add_decision_time(t.recv_ms);
}
void LeadLagReplay::add_decision_time(std::int64_t ms) {
    if (started_) throw std::logic_error("replay input already frozen");
    schedule(ms, 5, [this] { decide(); });
}
void LeadLagReplay::add_terminal_reduction(std::int64_t ms) {
    if (started_) throw std::logic_error("replay input already frozen");
    schedule(ms, 5, [this] {
        terminal_requested_ = true;
        for (const auto& [id, _] : orders_) request_cancel(id);
        schedule(now_ms_ + latency_.terminal_reduce_ms, 4, [this] { terminal_reduce(); });
    });
}

void LeadLagReplay::request_cancel(std::uint64_t id) {
    auto& o = orders_.at(id);
    if (o.reservation.cancel_pending || o.reservation.reserved_lots == 0) return;
    o.reservation.cancel_pending = true;
    ++summary_.cancel_requests;
    if (o.reservation.acknowledged) {
        o.cancel_scheduled = true;
        schedule(now_ms_ + latency_.cancel_ms, 3, [this, id] { cancel_effective(id); });
    }
    // Unacknowledged placement must first become effective; its cancellation
    // latency starts then, so a late order is never deleted before acceptance.
}
void LeadLagReplay::cancel_effective(std::uint64_t id) {
    auto& o = orders_.at(id);
    o.active = false;
    // Keep reservations for fills whose notification has not arrived yet.
    o.reservation.reserved_lots -= o.actual_remaining;
    o.actual_remaining = 0;
}

void LeadLagReplay::decide() {
    if (now_ms_ > engine_.market().expiry_ms) return;
    if (terminal_requested_) {
        for (const auto& [id, _] : orders_) request_cancel(id);
        return;
    }
    strategy::Input input;
    input.now_ms = now_ms_; input.known_lots = known_.lots; input.known_cash_tick = known_.cash_tick;
    // Reserve fee too. Pending sells cannot fund buys before notification.
    for (const auto& [_, o] : orders_) if (o.reservation.reserved_lots > 0) {
        input.outstanding.push_back(o.reservation);
        if (strategy::LeadLagEngine::is_bid(o.reservation.channel))
            input.known_cash_tick -= fee(o.reservation.price_tick, o.reservation.reserved_lots, latency_.maker_fee_bps);
    }
    auto d = engine_.decide(input);
    for (const auto& c : d.cancels) request_cancel(c.active_quote_id);
    for (const auto& q : d.quotes) {
        const auto c = channel_of(q, engine_.market());
        const int ti = strategy::LeadLagEngine::token(c);
        const auto& leg = q.has_bid ? q.bid : q.ask;
        std::int64_t reserved_cash = 0;
        for (const auto& [_, o] : orders_) if (strategy::LeadLagEngine::is_bid(o.reservation.channel))
            reserved_cash += o.reservation.price_tick * o.reservation.reserved_lots +
                fee(o.reservation.price_tick, o.reservation.reserved_lots, latency_.maker_fee_bps);
        if (q.has_bid && leg.price_tick * leg.quantity_lots + fee(leg.price_tick, leg.quantity_lots,
            latency_.maker_fee_bps) > known_.cash_tick - reserved_cash) {
            ++summary_.rejected;
            ++summary_.rejection_counts["insufficient_reserved_cash"];
            continue;
        }
        const auto checked = risk_.evaluate(risk::QuoteRiskInput{
            .quote = &q, .depth = &engine_.depth(c), .policy = &policy_,
            .current_position_lots = known_.lots[ti],
            .current_canonical_yes_position_lots = known_.lots[0] - known_.lots[1],
            .projected_canonical_yes_position_lots = known_.lots[0] - known_.lots[1] +
                strategy::LeadLagEngine::risk_sign(c) * leg.quantity_lots,
            .now_ns = static_cast<std::uint64_t>(now_ms_) * 1'000'000ULL
        });
        if (!checked.approved_quote) {
            ++summary_.rejected;
            ++summary_.rejection_counts[risk::quote_risk_decision_type_name(checked.decision.decision)];
            continue;
        }
        double displayed = 0;
        const auto& depth = engine_.depth(c);
        const auto& levels = q.has_bid ? depth.bids : depth.asks;
        const auto count = q.has_bid ? depth.bid_count : depth.ask_count;
        for (std::uint16_t j = 0; j < count; ++j)
            if (levels[j].price_tick == leg.price_tick) displayed += levels[j].size;
        Order o;
        o.quote = q; o.actual_remaining = leg.quantity_lots;
        o.queue_ahead = displayed * latency_.queue_factor;
        o.reservation = {c, q.quote_intent_id, q.quote_group_id, leg.price_tick, leg.quantity_lots,
            false, false, static_cast<std::int64_t>(q.expires_at_ns / 1'000'000ULL)};
        const auto id = q.quote_intent_id;
        if (!orders_.emplace(id, o).second) continue;
        ++summary_.submitted;
        schedule(now_ms_ + latency_.submit_ms, 2, [this, id] {
            auto& order = orders_.at(id);
            order.reservation.acknowledged = true;
            order.active = true;
            if (order.reservation.cancel_pending && !order.cancel_scheduled) {
                order.cancel_scheduled = true;
                schedule(now_ms_ + latency_.cancel_ms, 3, [this, id] { cancel_effective(id); });
            }
            // Revalidate after same-millisecond cancels and fill notices.
            schedule(now_ms_, 5, [this] { decide(); });
        });
        // Intent TTL is a cancellation deadline, not a proven exchange GTD.
        // Expiry must pay the same cancel latency as a strategy cancellation.
        schedule(o.reservation.expires_ms, 3, [this, id] { request_cancel(id); });
    }
    decisions_.push_back(std::move(d));
}

void LeadLagReplay::fill(Order& o, std::int64_t qty) {
    const auto c = o.reservation.channel;
    const auto price = o.reservation.price_tick;
    const auto charge = fee(price, qty, latency_.maker_fee_bps);
    apply_fill(actual_, c, price, qty, charge);
    if (actual_.cash_tick < 0 || actual_.lots[0] < 0 || actual_.lots[1] < 0)
        throw std::runtime_error("replay resource reservation violated");
    o.actual_remaining -= qty;
    if (o.actual_remaining == 0) o.active = false;
    fills_.push_back({now_ms_, c, price, qty, false, {}, o.reservation.cancel_pending});
    const auto id = o.reservation.quote_id;
    schedule(now_ms_ + latency_.fill_notice_ms, 4, [this, id, c, price, qty, charge] {
        apply_fill(known_, c, price, qty, charge);
        orders_.at(id).reservation.reserved_lots -= qty;
        schedule(now_ms_, 5, [this] { decide(); });
    });
}

void LeadLagReplay::trade(const PublicTrade& t) {
    std::vector<Order*> eligible;
    for (auto& [_, o] : orders_) {
        const auto c = o.reservation.channel;
        const bool bid = strategy::LeadLagEngine::is_bid(c);
        if (o.active && strategy::LeadLagEngine::token(c) == t.token && bid != t.taker_buy &&
            (bid ? t.price_tick <= o.reservation.price_tick : t.price_tick >= o.reservation.price_tick))
            eligible.push_back(&o);
    }
    std::sort(eligible.begin(), eligible.end(), [](const Order* a, const Order* b) {
        return a->quote.created_ts_ns < b->quote.created_ts_ns;
    });
    std::int64_t left = t.quantity_lots;
    for (auto* o : eligible) {
        const double consumed = std::min(o->queue_ahead, static_cast<double>(left));
        o->queue_ahead -= consumed;
        left -= static_cast<std::int64_t>(std::ceil(consumed));
        const auto qty = std::min(left, o->actual_remaining);
        if (qty > 0) { fill(*o, qty); left -= qty; }
        if (left <= 0) break;
    }
}

void LeadLagReplay::update_marks() {
    const auto& b = visible_books_;
    for (const auto& d : b) if (!d.usable_for_depth || d.crossed || d.recovering || d.closed || d.resolved ||
        d.bid_count == 0 || d.ask_count == 0 || d.bids[0].price_tick >= d.asks[0].price_tick ||
        now_ms_ - static_cast<std::int64_t>(d.last_ws_recv_ns / 1'000'000ULL) > engine_.config().max_data_age_ms) return;
    const auto anchor = (b[0].bids[0].price_tick + b[0].asks[0].price_tick + 2 * mm::kPriceOneTick -
        b[1].bids[0].price_tick - b[1].asks[0].price_tick) / (4.0 * mm::kPriceOneTick);
    const std::array<int, 3> horizons{100, 1'000, 5'000};
    for (auto& f : fills_) for (int h = 0; h < 3; ++h) {
        // A delayed book must have been generated after the markout horizon;
        // receive-time delay alone cannot manufacture a future price label.
        const auto source = static_cast<std::uint64_t>(mark_source_ms_);
        if (!f.markout[h] && source >= static_cast<std::uint64_t>(f.event_ms + horizons[h])) {
            const double up_price = strategy::LeadLagEngine::token(f.channel) == 0
                ? f.price_tick / static_cast<double>(mm::kPriceOneTick)
                : 1 - f.price_tick / static_cast<double>(mm::kPriceOneTick);
            f.markout[h] = strategy::LeadLagEngine::risk_sign(f.channel) * (anchor - up_price);
        }
    }
}

void LeadLagReplay::terminal_reduce() {
    // No hidden fill knowledge and no simultaneous maker exposure during an
    // active reduction. If notifications/cancels lag, skip this exit attempt.
    if (known_.lots != actual_.lots || known_.cash_tick != actual_.cash_tick) return;
    for (const auto& [_, o] : orders_) if (o.reservation.reserved_lots > 0) return;
    if (now_ms_ >= engine_.market().expiry_ms) return;
    const auto net = known_.lots[0] - known_.lots[1];
    if (net == 0) return;
    const int ti = net > 0 ? 0 : 1;
    const auto& d = visible_books_[ti];
    if (!d.usable_for_depth || d.recovering || d.crossed || d.closed || d.resolved || d.bid_count == 0 ||
        now_ms_ - static_cast<std::int64_t>(d.last_ws_recv_ns / 1'000'000ULL) > engine_.config().max_data_age_ms) return;
    auto left = std::abs(net);
    for (std::uint16_t i = 0; i < d.bid_count && left > 0; ++i) {
        if (!std::isfinite(d.bids[i].size) || d.bids[i].size <= 0 || d.bids[i].price_tick <= 0) continue;
        const auto qty = std::min(left, static_cast<std::int64_t>(std::floor(d.bids[i].size)));
        if (qty <= 0) continue;
        const auto c = ti == 0 ? strategy::Channel::SellUp : strategy::Channel::SellDown;
        const auto price = d.bids[i].price_tick;
        const auto charge = fee(price, qty, latency_.taker_fee_bps);
        apply_fill(actual_, c, price, qty, charge);
        fills_.push_back({now_ms_, c, price, qty, true, {}});
        schedule(now_ms_ + latency_.fill_notice_ms, 4, [this, c, price, qty, charge] {
            apply_fill(known_, c, price, qty, charge);
        });
        left -= qty;
    }
}

Summary LeadLagReplay::run(std::optional<bool> up_result) {
    if (started_) throw std::logic_error("replay can only run once");
    started_ = true;
    // Expiry cancels also run when the input ends before market close.
    schedule(engine_.market().expiry_ms, 5, [this] { decide(); });
    while (!events_.empty()) {
        auto e = events_.top(); events_.pop(); now_ms_ = e.ms; e.apply();
    }
    summary_.actual = actual_; summary_.known = known_;
    for (const auto& [_, order] : orders_)
        summary_.final_reserved_lots += order.reservation.reserved_lots;
    // Initial paired inventory has a cost: use its sum of UP-equivalent marks
    // at par (one dollar per complete set), supplied as initial cash + q_min.
    // Require paired seed so this cost is exact without an invented entry mark.
    const auto initial_equity = initial_.cash_tick + initial_.lots[0] * mm::kPriceOneTick;
    const auto cash = actual_.cash_tick - initial_equity - latency_.operating_cost_tick;
    summary_.pnl_min_tick = cash + std::min(actual_.lots[0], actual_.lots[1]) * mm::kPriceOneTick;
    summary_.pnl_max_tick = cash + std::max(actual_.lots[0], actual_.lots[1]) * mm::kPriceOneTick;
    if (up_result) summary_.settlement_pnl_tick = cash + actual_.lots[*up_result ? 0 : 1] * mm::kPriceOneTick;
    auto hash = mm::fnv1a_mix(14695981039346656037ULL, summary_.submitted);
    for (const auto& d : decisions_) {
        hash = mm::fnv1a_mix(hash, static_cast<std::uint8_t>(d.regime));
        hash = mm::fnv1a_mix(hash, std::bit_cast<std::uint64_t>(d.alpha));
        for (const auto& q : d.quotes) hash = mm::fnv1a_mix(hash, q.quote_intent_id);
        for (const auto& c : d.cancels) hash = mm::fnv1a_mix(hash, c.cancel_intent_id);
    }
    for (const auto& f : fills_) {
        hash = mm::fnv1a_mix(hash, f.event_ms); hash = mm::fnv1a_mix(hash, f.price_tick);
        hash = mm::fnv1a_mix(hash, f.quantity_lots); hash = mm::fnv1a_mix(hash, static_cast<std::uint8_t>(f.channel));
        for (int h = 0; h < 3; ++h) if (f.markout[h] && !f.terminal) {
            summary_.weighted_markout[h] += *f.markout[h] * f.quantity_lots;
            summary_.markout_quantity[h] += f.quantity_lots;
        }
    }
    for (int h = 0; h < 3; ++h) if (summary_.markout_quantity[h] > 0)
        summary_.weighted_markout[h] /= summary_.markout_quantity[h];
    hash = mm::fnv1a_mix(hash, summary_.pnl_min_tick); hash = mm::fnv1a_mix(hash, summary_.pnl_max_tick);
    summary_.output_hash = hash;
    return summary_;
}
}

#include "engine/strategy/market_making/products/btc15m_lead_lag/LeadLagEngine.h"

#include <boost/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace trading_engine::strategy::market_making::products::btc15m_lead_lag {
namespace {
constexpr double kScale = static_cast<double>(kPriceOneTick);
double midpoint(const state::MarketDepthView& d) {
    return (d.bids[0].price_tick + d.asks[0].price_tick) / (2 * kScale);
}
bool healthy(const state::MarketDepthView& d, std::int64_t now, std::int64_t age) {
    return d.usable_for_depth && !d.recovering && !d.crossed && !d.closed &&
        !d.resolved && d.bid_count > 0 && d.ask_count > 0 &&
        d.bid_count <= state::kMaxSnapshotDepth && d.ask_count <= state::kMaxSnapshotDepth &&
        d.bids[0].price_tick > 0 && d.asks[0].price_tick < kPriceOneTick &&
        d.bids[0].price_tick < d.asks[0].price_tick &&
        std::isfinite(d.bids[0].size) && std::isfinite(d.asks[0].size) &&
        d.bids[0].size > 0 && d.asks[0].size > 0 &&
        d.last_ws_recv_ns <= static_cast<std::uint64_t>(now) * 1'000'000ULL &&
        now - static_cast<std::int64_t>(d.last_ws_recv_ns / 1'000'000ULL) <= age;
}
double normal_quantile(double p) {
    double lo = -8, hi = 8;
    for (int i = 0; i < 64; ++i) {
        const double z = (lo + hi) / 2;
        if (0.5 * std::erfc(-z / std::sqrt(2.0)) < p) lo = z;
        else hi = z;
    }
    return (lo + hi) / 2;
}
std::int64_t tick(double p) {
    return static_cast<std::int64_t>(std::llround(std::clamp(p, 0.001, 0.999) * kScale));
}
Features read_array(const boost::json::value& v) {
    const auto& a = v.as_array();
    if (a.size() != kFeatureNames.size()) throw std::runtime_error("expected 13 model values");
    Features x{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = a[i].to_number<double>();
        if (!std::isfinite(x[i])) throw std::runtime_error("nonfinite model value");
    }
    return x;
}
RidgeModel read_model(const boost::json::value& v) {
    const auto& o = v.as_object();
    RidgeModel m;
    m.mean = read_array(o.at("mean"));
    m.scale = read_array(o.at("scale"));
    m.coefficients = read_array(o.at("coefficients"));
    m.intercept = o.at("intercept").to_number<double>();
    if (!std::isfinite(m.intercept) ||
        std::any_of(m.scale.begin(), m.scale.end(), [](double s) { return s <= 0; }))
        throw std::runtime_error("invalid model normalization");
    return m;
}
}

double RidgeModel::predict(const Features& x) const {
    double y = intercept;
    for (std::size_t i = 0; i < x.size(); ++i) y += coefficients[i] * (x[i] - mean[i]) / scale[i];
    return y;
}

FrozenModel FrozenModel::load(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot open frozen model: " + path);
    const std::string raw{std::istreambuf_iterator<char>(stream), {}};
    const auto parsed = boost::json::parse(raw);
    const auto& o = parsed.as_object();
    if (o.at("schema").as_string() != "btc15m-lead-lag-v1" ||
        o.at("symbol").as_string() != "BTCUSDT" ||
        o.at("duration_ms").to_number<std::int64_t>() != 900'000 ||
        o.at("preprocessing").as_string() != "recv-clock-rv1s-60s-v1")
        throw std::runtime_error("incompatible model scope or preprocessing");
    const auto& names = o.at("feature_names").as_array();
    if (names.size() != kFeatureNames.size()) throw std::runtime_error("model feature count mismatch");
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i].as_string() != kFeatureNames[i]) throw std::runtime_error("model feature order mismatch");
    FrozenModel m;
    m.plus = read_model(o.at("plus"));
    m.minus = read_model(o.at("minus"));
    m.trained_until_ms = o.at("trained_until_ms").to_number<std::int64_t>();
    m.synthetic = o.at("synthetic").as_bool();
    m.pm_only = o.at("pm_only").as_bool();
    m.external_feature_clip = o.at("external_feature_clip").to_number<double>();
    if (m.trained_until_ms < 0 || !std::isfinite(m.external_feature_clip) || m.external_feature_clip <= 0)
        throw std::runtime_error("invalid model metadata");
    if (m.pm_only) {
        for (std::size_t i = 0; i < 5; ++i)
            if (m.plus.coefficients[i] != 0 || m.minus.coefficients[i] != 0)
                throw std::runtime_error("PM-only artifact contains external coefficients");
    }
    m.artifact_hash = stable_hash_string(14695981039346656037ULL, raw);
    return m;
}

LeadLagEngine::LeadLagEngine(Market market, Config config, std::optional<FrozenModel> model)
    : market_(std::move(market)), config_(config), model_(std::move(model)) {
    if (market_.symbol != "BTCUSDT" || market_.start_ms < 0 ||
        market_.expiry_ms - market_.start_ms != 900'000 || market_.market_id.empty() ||
        market_.token_ids[0].empty() || market_.token_ids[1].empty() ||
        market_.token_ids[0] == market_.token_ids[1] || market_.asset_indices[0] == market_.asset_indices[1])
        throw std::invalid_argument("strategy requires a BTC 15m UP/DOWN pair");
    if (config_.grid_tick <= 0 || kPriceOneTick % config_.grid_tick != 0 ||
        config_.grid_tick >= kPriceOneTick / 2 || config_.buffer_tick < 0 ||
        config_.inventory_skew_tick_per_lot < 0 || config_.size_lots <= 0 ||
        config_.min_size_lots <= 0 || config_.min_size_lots > config_.size_lots ||
        config_.max_net_lots <= 0 || config_.max_token_lots <= 0 ||
        config_.max_token_lots > 1'000'000 || config_.max_net_lots > 1'000'000 ||
        config_.inventory_skew_tick_per_lot > kPriceOneTick ||
        config_.queue_tolerance_tick < 0 || config_.max_data_age_ms <= 0 ||
        config_.quiet_recovery_ms < 0 || config_.quote_ttl_ms <= 0 ||
        config_.calm_tick <= 0 || config_.shock_tick <= config_.calm_tick ||
        config_.halt_tick <= config_.shock_tick)
        throw std::invalid_argument("invalid lead-lag configuration");
    if (model_ && model_->trained_until_ms > market_.start_ms)
        throw std::invalid_argument("model training overlaps replay market");
}

int LeadLagEngine::risk_sign(Channel c) { return c == Channel::BuyUp || c == Channel::SellDown ? 1 : -1; }
int LeadLagEngine::token(Channel c) { return static_cast<int>(c) / 2; }
bool LeadLagEngine::is_bid(Channel c) { return c == Channel::BuyUp || c == Channel::BuyDown; }
const state::MarketDepthView& LeadLagEngine::depth(Channel c) const { return books_[token(c)]; }

void LeadLagEngine::on_agg_trade(const AggTrade& t) {
    if (t.event_ms < 0 || t.recv_ms < t.event_ms || t.recv_ms < latest_recv_ms_ ||
        !std::isfinite(t.price) || t.price <= 0 || !std::isfinite(t.quantity) || t.quantity <= 0)
        throw std::invalid_argument("invalid or receive-out-of-order aggTrade");
    trades_.push_back(t);
    latest_recv_ms_ = t.recv_ms;
    if (t.event_ms >= latest_source_ms_) {
        latest_source_ms_ = t.event_ms;
        btc_history_.push_back({t.recv_ms, std::log(t.price)});
    }
    while (btc_history_.size() > 1 && btc_history_[1].ms < t.recv_ms - 65'000) btc_history_.pop_front();
    while (!trades_.empty() && trades_.front().recv_ms < t.recv_ms - 250) trades_.pop_front();
}

void LeadLagEngine::on_books(const state::MarketDepthView& up, const state::MarketDepthView& down,
                             std::int64_t recv_ms) {
    if (recv_ms < 0 || recv_ms < book_recv_ms_ ||
        up.asset_index != market_.asset_indices[0] || down.asset_index != market_.asset_indices[1] ||
        up.last_ws_recv_ns > static_cast<std::uint64_t>(recv_ms) * 1'000'000ULL ||
        down.last_ws_recv_ns > static_cast<std::uint64_t>(recv_ms) * 1'000'000ULL)
        throw std::invalid_argument("invalid paired book receive time or asset mapping");
    books_ = {up, down};
    book_recv_ms_ = recv_ms;
    if (healthy(up, recv_ms, config_.max_data_age_ms) && healthy(down, recv_ms, config_.max_data_age_ms)) {
        pm_history_.push_back({recv_ms, (midpoint(up) + 1 - midpoint(down)) / 2});
        while (pm_history_.size() > 1 && pm_history_[1].ms < recv_ms - 6'000) pm_history_.pop_front();
    }
}

std::optional<double> LeadLagEngine::lagged(const std::deque<Point>& h, std::int64_t ms) const {
    const auto it = std::upper_bound(h.begin(), h.end(), ms,
        [](std::int64_t time, const Point& p) { return time < p.ms; });
    if (it == h.begin()) return std::nullopt;
    const auto& p = *std::prev(it);
    if (ms - p.ms > config_.max_data_age_ms) return std::nullopt;
    return p.value;
}

std::optional<Features> LeadLagEngine::features(int ti, std::int64_t now) const {
    Features x{};
    const double u = midpoint(books_[0]), d = 1 - midpoint(books_[1]);
    const double anchor = (u + d) / 2;
    if (!model_->pm_only || config_.pm_only_require_external_readiness) {
        const auto current = lagged(btc_history_, now);
        if (!current) return std::nullopt;
        double sum = 0, sum_sq = 0;
        int count = 0;
        // Non-overlapping one-second returns; sigma units are per sqrt(second).
        for (int i = 0; i < 60; ++i) {
            const auto a = lagged(btc_history_, now - i * 1'000);
            const auto b = lagged(btc_history_, now - (i + 1) * 1'000);
            if (!a || !b) break;
            const double r = *a - *b;
            sum += r; sum_sq += r * r; ++count;
        }
        if (count < 10) return std::nullopt;
        const double variance = (sum_sq - sum * sum / count) / (count - 1);
        if (!std::isfinite(variance) || variance <= 1e-16) return std::nullopt;
        const double z = normal_quantile(std::clamp(anchor, 0.001, 0.999));
        const double g = std::exp(-z * z / 2) / std::sqrt(2 * std::acos(-1.0)) /
            (std::sqrt(variance) * std::sqrt((market_.expiry_ms - now) / 1000.0));
        const std::array<int, 4> horizons{100, 250, 1'000, 5'000};
        for (std::size_t i = 0; i < horizons.size(); ++i) {
            const auto old = lagged(btc_history_, now - horizons[i]);
            if (!old) return std::nullopt;
            x[i] = std::clamp(g * (*current - *old), -model_->external_feature_clip, model_->external_feature_clip);
        }
        double signed_volume = 0, volume = 0;
        for (const auto& t : trades_) if (t.recv_ms > now - 250 && t.recv_ms <= now) {
            signed_volume += (t.buyer_is_maker ? -1 : 1) * t.quantity;
            volume += t.quantity;
        }
        x[4] = volume > 0 ? signed_volume / volume : 0;
    }
    const std::array<int, 3> horizons{500, 1'000, 5'000};
    for (std::size_t i = 0; i < horizons.size(); ++i) {
        const auto old = lagged(pm_history_, now - horizons[i]);
        if (!old) return std::nullopt;
        x[5 + i] = anchor - *old;
    }
    x[8] = u - d;
    x[9] = (books_[ti].asks[0].price_tick - books_[ti].bids[0].price_tick) / kScale;
    x[10] = anchor;
    x[11] = (market_.expiry_ms - now) / 1000.0;
    x[12] = (ti == 0 ? u : d) - anchor;
    return x;
}

void LeadLagEngine::update_regime(double alpha, std::int64_t now) {
    const auto a = std::abs(alpha) * kScale;
    if (a >= config_.shock_tick) {
        recovering_ = true;
        quiet_since_ms_.reset();
        regime_ = a >= config_.halt_tick ? Regime::Halt : Regime::Shock;
    } else if (recovering_) {
        if (a < config_.calm_tick) {
            if (!quiet_since_ms_) quiet_since_ms_ = now;
            if (now - *quiet_since_ms_ >= config_.quiet_recovery_ms) recovering_ = false;
        } else quiet_since_ms_.reset();
        regime_ = recovering_ ? Regime::Wait : Regime::Calm;
    } else regime_ = a < config_.calm_tick ? Regime::Calm : Regime::Shift;
}

Decision LeadLagEngine::decide(const Input& input) {
    if (input.now_ms < 0 || input.now_ms < last_decision_ms_) throw std::invalid_argument("decision clock moved backwards");
    const auto now = input.now_ms;
    const bool gap = last_decision_ms_ >= 0 && now - last_decision_ms_ > config_.max_data_age_ms;
    last_decision_ms_ = now;
    if (gap) quiet_since_ms_.reset();
    Decision result;
    result.now_ms = now;
    result.known_lots = input.known_lots;
    result.known_cash_tick = input.known_cash_tick;
    auto cancel = [&](const OutstandingOrder& order) {
        if (order.cancel_pending || order.reserved_lots <= 0) return;
        CancelQuoteIntent c;
        c.quote_group_id = order.group_id;
        c.active_quote_id = order.quote_id;
        c.asset_index = market_.asset_indices[token(order.channel)];
        c.reason = CancelReason::RiskDegraded;
        c.created_ts_ns = static_cast<std::uint64_t>(now) * 1'000'000ULL;
        c.idempotency_hash = compute_cancel_quote_intent_hash(c);
        c.cancel_intent_id = c.idempotency_hash;
        result.cancels.push_back(c);
    };
    auto halt = [&](const char* reason) {
        result.reason = reason;
        regime_ = Regime::Halt;
        recovering_ = true;
        quiet_since_ms_.reset();
        for (const auto& o : input.outstanding) cancel(o);
    };
    if (!model_) halt("missing_frozen_model");
    else if (now < market_.start_ms || now >= market_.expiry_ms) halt("outside_market_window");
    else if (latest_recv_ms_ > now || book_recv_ms_ > now) halt("future_data_not_visible");
    else if (!healthy(books_[0], now, config_.max_data_age_ms) ||
             !healthy(books_[1], now, config_.max_data_age_ms)) halt("invalid_or_stale_pair");
    else if ((!model_->pm_only || config_.pm_only_require_external_readiness) &&
             (btc_history_.empty() || now - btc_history_.back().ms > config_.max_data_age_ms)) halt("stale_external_price");
    else {
        const auto up = features(0, now), down = features(1, now);
        if (!up || !down) halt("feature_warmup_or_missing_history");
        else {
            result.features = {*up, *up, *down, *down};
            result.anchor = (*up)[10];
            for (int i = 0; i < 4; ++i) {
                const auto c = static_cast<Channel>(i);
                result.delta[i] = (risk_sign(c) > 0 ? model_->plus : model_->minus).predict(result.features[i]);
                result.alpha += result.delta[i] / 4;
            }
            if (!std::isfinite(result.alpha) || std::any_of(result.delta.begin(), result.delta.end(),
                [](double x) { return !std::isfinite(x); })) halt("nonfinite_prediction");
            else {
                result.ready = true;
                update_regime(result.alpha, now);
            }
        }
    }
    result.regime = regime_;
    if (!result.ready) return result;
    const auto net = input.known_lots[0] - input.known_lots[1];
    if (input.known_lots[0] < 0 || input.known_lots[1] < 0 ||
        input.known_lots[0] > config_.max_token_lots || input.known_lots[1] > config_.max_token_lots ||
        std::abs(net) > config_.max_net_lots || input.known_cash_tick < 0) {
        halt("inventory_or_cash_out_of_bounds");
        result.ready = false; result.regime = regime_;
        return result;
    }
    std::array<std::int64_t, 2> buy_reserved{}, sell_reserved{};
    std::int64_t cash_reserved = 0, plus_reserved = 0, minus_reserved = 0;
    auto reserve = [&](Channel c, std::int64_t price, std::int64_t qty) {
        const int ti = token(c);
        if (is_bid(c)) { buy_reserved[ti] += qty; cash_reserved += price * qty; }
        else sell_reserved[ti] += qty;
        (risk_sign(c) > 0 ? plus_reserved : minus_reserved) += qty;
    };
    for (const auto& o : input.outstanding) {
        if (o.reserved_lots < 0 || o.reserved_lots > config_.max_token_lots ||
            o.price_tick <= 0 || o.price_tick >= kPriceOneTick)
            throw std::invalid_argument("invalid outstanding reservation");
        reserve(o.channel, o.price_tick, o.reserved_lots);
    }
    // Deterministic shared reduction budget stops sell-UP + buy-DOWN from
    // jointly crossing zero in the shock-entry inventory-management candidate.
    std::int64_t reduction_reserved = net > 0 ? minus_reserved : plus_reserved;
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<Channel>(i);
        const int ti = token(c), sign = risk_sign(c);
        const bool bid = is_bid(c), reduces = net * sign < 0;
        bool allowed = regime_ != Regime::Halt && regime_ != Regime::Wait &&
            (regime_ != Regime::Shock || result.alpha * sign >= 0);
        if (config_.candidate == Candidate::ShockEntry && !reduces && regime_ != Regime::Shock) allowed = false;
        const auto raw_up = tick(result.anchor + result.delta[i]);
        const auto shifted_up = std::clamp<std::int64_t>(
            raw_up - config_.inventory_skew_tick_per_lot * net, 1'000, 999'000);
        const auto raw = ti == 0 ? raw_up : kPriceOneTick - raw_up;
        const auto shifted = ti == 0 ? shifted_up : kPriceOneTick - shifted_up;
        const auto candidate = bid
            ? std::min(books_[ti].bids[0].price_tick,
                ((shifted - config_.buffer_tick) / config_.grid_tick) * config_.grid_tick)
            : std::max(books_[ti].asks[0].price_tick,
                ((shifted + config_.buffer_tick + config_.grid_tick - 1) / config_.grid_tick) * config_.grid_tick);
        // Touch may use a different grid; never emit an off-grid order.
        const auto price = bid ? (candidate / config_.grid_tick) * config_.grid_tick
            : ((candidate + config_.grid_tick - 1) / config_.grid_tick) * config_.grid_tick;
        allowed = allowed && price >= config_.grid_tick && price <= kPriceOneTick - config_.grid_tick;
        bool outstanding = false;
        for (const auto& o : input.outstanding) if (o.channel == c && o.reserved_lots > 0) {
            outstanding = true;
            const bool safe = bid
                ? shifted - o.price_tick >= config_.buffer_tick && raw - o.price_tick >= config_.buffer_tick
                : o.price_tick - shifted >= config_.buffer_tick && o.price_tick - raw >= config_.buffer_tick;
            if (!allowed || !safe || std::abs(o.price_tick - price) > config_.queue_tolerance_tick || now >= o.expires_ms)
                cancel(o);
        }
        // Includes pending acknowledgements and pending cancels. A replacement
        // waits for explicit release of its predecessor's reservation.
        if (!allowed || outstanding) continue;
        auto qty = config_.size_lots;
        qty = std::min(qty, sign > 0 ? config_.max_net_lots - net - plus_reserved
                                      : config_.max_net_lots + net - minus_reserved);
        if (bid) {
            qty = std::min({qty, config_.max_token_lots - input.known_lots[ti] - buy_reserved[ti],
                std::max<std::int64_t>(0, input.known_cash_tick - cash_reserved) / price});
        } else qty = std::min(qty, input.known_lots[ti] - sell_reserved[ti]);
        if (config_.candidate == Candidate::ShockEntry && reduces)
            qty = std::min(qty, std::max<std::int64_t>(0, std::abs(net) - reduction_reserved));
        if (qty < config_.min_size_lots) continue;
        // Require displayed depth at the actual quote price. Behind-touch
        // levels absent from the snapshot cannot provide a queue estimate.
        const auto& levels = bid ? books_[ti].bids : books_[ti].asks;
        const auto count = bid ? books_[ti].bid_count : books_[ti].ask_count;
        bool has_depth = false;
        for (std::uint16_t j = 0; j < count; ++j)
            has_depth |= levels[j].price_tick == price && std::isfinite(levels[j].size) && levels[j].size > 0;
        if (!has_depth) continue;
        QuoteIntent q;
        q.type = QuoteIntentType::PlaceQuote;
        q.strategy_id = config_.strategy_id;
        // One group per channel; cancelling one side never removes its sibling.
        q.quote_group_id = fnv1a_mix(stable_hash_string(config_.strategy_id, market_.market_id), i + 1);
        q.market_id = market_.market_id; q.asset_id = market_.token_ids[ti];
        q.market_index = market_.market_index; q.asset_index = market_.asset_indices[ti];
        // Continuous quoting may cross zero. Such an order contains opening
        // risk and must not claim reduce-only exemptions at the risk boundary.
        const bool pure_reduction = reduces && qty <= std::abs(net);
        q.risk_mode = pure_reduction ? QuoteIntentRiskMode::ReduceOnly
            : (reduces ? QuoteIntentRiskMode::Mixed : QuoteIntentRiskMode::Opening);
        q.has_bid = bid; q.has_ask = !bid;
        // Per-leg fair is the inventory-shifted channel fair, so the generic
        // risk evaluator checks the intended order edge without a bypass.
        q.fair_value_tick = shifted; q.asset_fair_tick = raw;
        q.canonical_yes_fair_tick = raw_up;
        q.canonical_yes_position_lots = net;
        q.current_position_lots = input.known_lots[ti];
        q.target_position_lots = pure_reduction
            ? input.known_lots[ti] + (bid ? std::abs(net) : -std::abs(net))
            : input.known_lots[ti];
        q.inventory_skew_tick = raw - shifted; q.half_spread_tick = config_.buffer_tick;
        q.product_reward_size_lift_allowed = false;
        q.product_reward_spread_compression_allowed = false;
        q.created_ts_ns = static_cast<std::uint64_t>(now) * 1'000'000ULL;
        q.expires_at_ns = static_cast<std::uint64_t>(std::min(now + config_.quote_ttl_ms, market_.expiry_ms)) * 1'000'000ULL;
        q.snapshot_version_hash = books_[ti].snapshot_version_hash;
        q.oracle_artifact_hash = model_->artifact_hash;
        q.reason = "btc15m_lead_lag_channel_value";
        auto& leg = bid ? q.bid : q.ask;
        leg.market_id = q.market_id; leg.asset_id = q.asset_id;
        leg.market_index = q.market_index; leg.asset_index = q.asset_index;
        leg.side = bid ? QuoteSide::Bid : QuoteSide::Ask;
        leg.price_tick = price; leg.quantity_lots = qty; leg.fair_value_tick = shifted;
        leg.edge_to_fair_tick = bid ? shifted - price : price - shifted;
        leg.risk_reducing = pure_reduction; leg.book_version = books_[ti].book_version;
        leg.snapshot_version_hash = q.snapshot_version_hash;
        q.idempotency_hash = compute_quote_intent_hash(q); q.quote_intent_id = q.idempotency_hash;
        result.quotes.push_back(q);
        reserve(c, price, qty);
        if (reduces) reduction_reserved += qty;
    }
    result.reason = result.quotes.empty() ? "holding_or_no_eligible_channel" : "channel_quotes";
    return result;
}

const char* regime_name(Regime r) {
    switch (r) {
        case Regime::Calm: return "CALM";
        case Regime::Shift: return "SHIFT";
        case Regime::Shock: return "SHOCK";
        case Regime::Halt: return "HALT";
        case Regime::Wait: return "WAIT";
    }
    return "HALT";
}
}

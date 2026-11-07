#include "replay/market_making/LeadLagReplay.h"

#include <boost/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace json = boost::json;
namespace ll = trading_engine::replay::lead_lag;
namespace s = ll::strategy;

namespace {
json::value read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return json::parse(std::string{std::istreambuf_iterator<char>(f), {}});
}
std::int64_t integer(const json::object& o, const char* name) { return o.at(name).to_number<std::int64_t>(); }
trading_engine::state::MarketDepthView book(const json::object& o, std::uint32_t asset,
                                           std::int64_t recv, std::uint64_t version) {
    trading_engine::state::MarketDepthView b;
    b.asset_index = asset; b.book_version = version;
    b.snapshot_version_hash = ll::mm::fnv1a_mix(version, asset);
    const auto token_recv = o.if_contains("recv_ms") ? integer(o, "recv_ms") : recv;
    if (token_recv < 0 || token_recv > recv) throw std::runtime_error("invalid token receive clock");
    b.last_ws_recv_ns = static_cast<std::uint64_t>(token_recv) * 1'000'000ULL;
    b.usable_for_depth = o.at("usable").as_bool();
    auto levels = [](const json::array& a, auto& output, std::uint16_t& count) {
        if (a.size() > output.size()) throw std::runtime_error("too many book levels");
        count = static_cast<std::uint16_t>(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto& row = a[i].as_array();
            if (row.size() != 2) throw std::runtime_error("depth rows require [price_tick, quantity]");
            output[i].price_tick = row[0].to_number<std::int64_t>();
            output[i].size = row[1].to_number<double>();
            if (output[i].price_tick <= 0 || output[i].price_tick >= ll::mm::kPriceOneTick ||
                !std::isfinite(output[i].size) || output[i].size < 0)
                throw std::runtime_error("invalid depth level");
        }
    };
    levels(o.at("bids").as_array(), b.bids, b.bid_count);
    levels(o.at("asks").as_array(), b.asks, b.ask_count);
    for (std::uint16_t i = 1; i < b.bid_count; ++i)
        if (b.bids[i].price_tick >= b.bids[i - 1].price_tick) throw std::runtime_error("unsorted bids");
    for (std::uint16_t i = 1; i < b.ask_count; ++i)
        if (b.asks[i].price_tick <= b.asks[i - 1].price_tick) throw std::runtime_error("unsorted asks");
    b.crossed = b.bid_count && b.ask_count && b.bids[0].price_tick >= b.asks[0].price_tick;
    return b;
}
}

int main(int argc, char** argv) {
    try {
        std::string config_path, events_path, model_path, trace_path;
        bool allow_synthetic = false;
        std::optional<bool> outcome;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--allow-synthetic-model") { allow_synthetic = true; continue; }
            if (a == "--help") {
                std::cout << "--config FILE --events JSONL [--model FILE] [--trace JSONL] "
                             "[--settlement up|down] [--allow-synthetic-model]\n";
                return 0;
            }
            if (++i == argc) throw std::runtime_error("missing value for " + a);
            if (a == "--config") config_path = argv[i];
            else if (a == "--events") events_path = argv[i];
            else if (a == "--model") model_path = argv[i];
            else if (a == "--trace") trace_path = argv[i];
            else if (a == "--settlement") {
                const std::string v = argv[i];
                if (v != "up" && v != "down") throw std::runtime_error("settlement must be up or down");
                outcome = v == "up";
            } else throw std::runtime_error("unknown option " + a);
        }
        if (config_path.empty() || events_path.empty()) throw std::runtime_error("--config and --events required");
        const auto config = read_json(config_path);
        const auto& o = config.as_object();
        if (o.at("strategy").as_string() != "btc15m_lead_lag") throw std::runtime_error("wrong strategy config");
        const auto& market = o.at("market").as_object();
        s::Market m;
        m.market_id = std::string(market.at("market_id").as_string());
        m.token_ids = {std::string(market.at("up_token").as_string()), std::string(market.at("down_token").as_string())};
        m.start_ms = integer(market, "start_ms"); m.expiry_ms = integer(market, "expiry_ms");
        m.symbol = std::string(market.at("symbol").as_string());
        s::Config c;
        const auto& q = o.at("quote").as_object();
        const auto candidate = o.at("candidate").as_string();
        if (candidate != "continuous" && candidate != "shock_entry") throw std::runtime_error("unknown candidate");
        c.candidate = candidate == "shock_entry" ? s::Candidate::ShockEntry : s::Candidate::Continuous;
        c.grid_tick = integer(q, "grid_tick"); c.buffer_tick = integer(q, "buffer_tick");
        c.inventory_skew_tick_per_lot = integer(q, "inventory_skew_tick_per_lot");
        c.queue_tolerance_tick = integer(q, "queue_tolerance_tick");
        c.size_lots = integer(q, "size_lots"); c.max_net_lots = integer(q, "max_net_lots");
        if (q.if_contains("min_size_lots")) c.min_size_lots = integer(q, "min_size_lots");
        if (o.if_contains("pm_only_require_external_readiness"))
            c.pm_only_require_external_readiness = o.at("pm_only_require_external_readiness").as_bool();
        c.max_token_lots = integer(q, "max_token_lots"); c.max_data_age_ms = integer(q, "max_data_age_ms");
        c.quote_ttl_ms = integer(q, "ttl_ms");
        const auto& state = o.at("regime").as_object();
        c.calm_tick = integer(state, "calm_tick"); c.shock_tick = integer(state, "shock_tick");
        c.halt_tick = integer(state, "halt_tick"); c.quiet_recovery_ms = integer(state, "quiet_recovery_ms");
        ll::LatencyConfig latency;
        const auto& delays = o.at("latency_assumptions_ms").as_object();
        latency.submit_ms = integer(delays, "submit"); latency.cancel_ms = integer(delays, "cancel");
        latency.fill_notice_ms = integer(delays, "fill_notice"); latency.terminal_reduce_ms = integer(delays, "terminal_reduce");
        const auto& accounting = o.at("accounting").as_object();
        latency.queue_factor = o.at("queue_factor").to_number<double>();
        latency.maker_fee_bps = integer(accounting, "maker_fee_bps");
        latency.taker_fee_bps = integer(accounting, "taker_fee_bps");
        latency.operating_cost_tick = integer(accounting, "operating_cost_tick");
        const auto pairs = integer(accounting, "initial_pairs");
        ll::Ledger seed{integer(accounting, "initial_cash_tick"), {pairs, pairs}};
        std::optional<s::FrozenModel> frozen;
        if (model_path.empty() && !o.at("model_path").is_null()) model_path = std::string(o.at("model_path").as_string());
        if (!model_path.empty()) {
            frozen = s::FrozenModel::load(model_path);
            if (frozen->synthetic && !allow_synthetic) throw std::runtime_error("synthetic fixture requires --allow-synthetic-model");
        }
        ll::LeadLagReplay replay(s::LeadLagEngine(m, c, frozen), seed, latency);
        std::ifstream events(events_path);
        if (!events) throw std::runtime_error("cannot open events " + events_path);
        std::string line;
        std::uint64_t version = 0;
        const auto btc_delay_override = o.if_contains("btc_receive_delay_ms")
            ? std::optional<std::int64_t>(integer(o, "btc_receive_delay_ms")) : std::nullopt;
        if (btc_delay_override && (*btc_delay_override < 0 || *btc_delay_override > 5'000))
            throw std::runtime_error("invalid BTC receive delay assumption");
        while (std::getline(events, line)) {
            if (line.empty()) continue;
            const auto parsed = json::parse(line);
            const auto& e = parsed.as_object();
            const auto kind = e.at("type").as_string();
            const auto recv = integer(e, "recv_ms");
            if (kind == "agg_trade") replay.add_agg_trade(s::AggTrade{
                integer(e, "event_ms"), btc_delay_override ? integer(e, "event_ms") + *btc_delay_override : recv, e.at("price").to_number<double>(),
                e.at("quantity").to_number<double>(), e.at("buyer_is_maker").as_bool()});
            else if (kind == "books") {
                ++version;
                replay.add_books(book(e.at("up").as_object(), 0, recv, version),
                    book(e.at("down").as_object(), 1, recv, version), integer(e, "event_ms"), recv);
            } else if (kind == "public_trade") {
                const auto token = e.at("token").as_string();
                if (token != "up" && token != "down") throw std::runtime_error("public trade token must be up/down");
                replay.add_public_trade({integer(e, "event_ms"), recv, token == "up" ? 0 : 1,
                    e.at("taker_buy").as_bool(), integer(e, "price_tick"), integer(e, "quantity_lots")});
            } else throw std::runtime_error("unknown event type");
        }
        const auto interval = integer(o, "decision_interval_ms");
        if (interval <= 0) throw std::runtime_error("invalid decision interval");
        for (auto ms = m.start_ms; ms < m.expiry_ms; ms += interval) replay.add_decision_time(ms);
        replay.add_decision_time(m.expiry_ms); // cancellation still pays its execution latency
        if (!o.at("terminal_reduce_request_ms").is_null()) replay.add_terminal_reduction(integer(o, "terminal_reduce_request_ms"));
        const auto summary = replay.run(outcome);
        std::ofstream trace;
        if (!trace_path.empty()) {
            trace.open(trace_path);
            if (!trace) throw std::runtime_error("cannot write trace " + trace_path);
            for (const auto& d : replay.decisions()) {
                json::array deltas, placements, cancels, features;
                for (const auto v : d.delta) deltas.push_back(v);
                for (const auto& x : d.features) {
                    json::array row;
                    for (const auto v : x) row.push_back(v);
                    features.push_back(row);
                }
                for (const auto& quote : d.quotes) {
                    const auto& leg = quote.has_bid ? quote.bid : quote.ask;
                    placements.push_back(json::object{{"quote_id", quote.quote_intent_id}, {"asset", quote.asset_id},
                        {"side", quote.has_bid ? "bid" : "ask"}, {"price_tick", leg.price_tick}, {"qty", leg.quantity_lots}});
                }
                for (const auto& cancel : d.cancels) cancels.push_back(cancel.active_quote_id);
                trace << json::serialize(json::object{{"now_ms", d.now_ms}, {"regime", s::regime_name(d.regime)}, {"ready", d.ready},
                    {"known_up_lots", d.known_lots[0]}, {"known_down_lots", d.known_lots[1]},
                    {"known_cash_tick", d.known_cash_tick}, {"features", features},
                    {"reason", d.reason}, {"anchor", d.anchor}, {"alpha", d.alpha}, {"delta", deltas},
                    {"proposed_quotes", placements}, {"cancel_requests", cancels}}) << '\n';
            }
            for (const auto& f : replay.fills()) {
                json::array markouts;
                for (const auto& mo : f.markout) markouts.push_back(mo ? json::value(*mo) : json::value(nullptr));
                trace << json::serialize(json::object{{"type", "fill"}, {"event_ms", f.event_ms},
                    {"channel", static_cast<int>(f.channel)}, {"price_tick", f.price_tick},
                    {"quantity_lots", f.quantity_lots}, {"terminal", f.terminal}, {"cancel_pending", f.cancel_pending}, {"markout", markouts}}) << '\n';
            }
        }
        json::array marks, quantities;
        json::object rejections;
        for (const auto& [reason, count] : summary.rejection_counts) rejections[reason] = count;
        for (int i = 0; i < 3; ++i) {
            marks.push_back(summary.markout_quantity[i] ? json::value(summary.weighted_markout[i]) : json::value(nullptr));
            quantities.push_back(summary.markout_quantity[i]);
        }
        std::int64_t cancel_pending_fills = 0, cancel_pending_lots = 0, post_expiry_fills = 0, post_expiry_lots = 0;
        for (const auto& f : replay.fills()) {
            if (f.cancel_pending) { ++cancel_pending_fills; cancel_pending_lots += f.quantity_lots; }
            if (f.event_ms >= m.expiry_ms) { ++post_expiry_fills; post_expiry_lots += f.quantity_lots; }
        }
        std::cout << json::serialize(json::object{
            {"strategy", "btc15m_lead_lag"}, {"model_loaded", frozen.has_value()},
            {"synthetic", frozen && frozen->synthetic}, {"submitted", summary.submitted},
            {"risk_rejected", summary.rejected}, {"cancel_requests", summary.cancel_requests},
            {"rejection_counts", rejections}, {"final_reserved_lots", summary.final_reserved_lots},
            {"fills", replay.fills().size()}, {"cash_tick", summary.actual.cash_tick},
            {"cancel_pending_fills", cancel_pending_fills}, {"cancel_pending_fill_lots", cancel_pending_lots},
            {"post_expiry_fills", post_expiry_fills}, {"post_expiry_fill_lots", post_expiry_lots},
            {"up_lots", summary.actual.lots[0]}, {"down_lots", summary.actual.lots[1]},
            {"known_cash_tick", summary.known.cash_tick},
            {"known_up_lots", summary.known.lots[0]}, {"known_down_lots", summary.known.lots[1]},
            {"pnl_min_tick", summary.pnl_min_tick}, {"pnl_max_tick", summary.pnl_max_tick},
            {"settlement_pnl_tick", summary.settlement_pnl_tick ? json::value(*summary.settlement_pnl_tick) : json::value(nullptr)},
            {"quantity_weighted_markout", marks}, {"markout_quantity", quantities},
            {"output_hash", summary.output_hash}, {"latency_is_assumed", true}, {"rebates_tick", 0}
        }) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "btc15m lead-lag replay: " << e.what() << '\n';
        return 1;
    }
}

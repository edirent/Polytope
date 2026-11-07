#include "replay/market_making/LeadLagReplay.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace ll = trading_engine::replay::lead_lag;
namespace s = ll::strategy;
namespace mm = ll::mm;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F f, const char* message) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
s::Market market() { return {"btc15m-test", {"up", "down"}, {0, 1}, 0, 100'000, 1'000'000, "BTCUSDT"}; }
s::FrozenModel model() {
    s::FrozenModel m;
    m.plus.scale.fill(1); m.minus.scale.fill(1);
    m.synthetic = true; m.artifact_hash = 42;
    return m;
}
trading_engine::state::MarketDepthView book(int ti, std::int64_t ms, double anchor = .5, double size = 10) {
    trading_engine::state::MarketDepthView b;
    const double center = ti == 0 ? anchor : 1 - anchor;
    b.asset_index = ti; b.usable_for_depth = true;
    b.last_ws_recv_ns = static_cast<std::uint64_t>(ms) * 1'000'000ULL;
    b.book_version = ms; b.snapshot_version_hash = ms + ti;
    b.bid_count = b.ask_count = 1;
    b.bids[0].price_tick = std::llround((center - .01) * mm::kPriceOneTick);
    b.asks[0].price_tick = std::llround((center + .01) * mm::kPriceOneTick);
    b.bids[0].size = b.asks[0].size = size;
    return b;
}
s::AggTrade btc(std::int64_t ms) {
    return {ms, ms, 70'000 * std::exp(.0002 * std::sin(ms / 310.0)), 1, false};
}
void update(s::LeadLagEngine& e, std::int64_t ms, double anchor = .5) {
    e.on_agg_trade(btc(ms)); e.on_books(book(0, ms, anchor), book(1, ms, anchor), ms);
}
void warm(s::LeadLagEngine& e) { for (auto ms = 80'000; ms <= 100'000; ms += 100) update(e, ms); }
s::Input input(std::int64_t ms = 100'000) { return {ms, {25, 25}, 100'000'000, {}}; }
s::OutstandingOrder old(s::Channel c, std::int64_t price = 510'000) {
    return {c, 10 + static_cast<unsigned>(c), 20 + static_cast<unsigned>(c), price, 5, true, false, 110'000};
}

void scope_and_artifact() {
    auto m = market(); m.symbol = "ETHUSDT";
    rejects([&] { s::LeadLagEngine e(m); }, "ETH must be rejected");
    m = market(); m.expiry_ms = m.start_ms + 300'000;
    rejects([&] { s::LeadLagEngine e(m); }, "5m must be rejected");
    auto future = model(); future.trained_until_ms = 100'001;
    rejects([&] { s::LeadLagEngine e(market(), {}, future); }, "future-trained artifact rejected");
    const auto artifact = s::FrozenModel::load(std::string(POLYTOPE_SOURCE_DIR) + "/tests/fixtures/market_making/btc15m_synthetic_model.json");
    check(artifact.synthetic && artifact.artifact_hash != 0, "load synthetic provenance/hash");
    check(std::abs(artifact.plus.predict(s::Features{}) - artifact.plus.intercept) < 1e-12, "normalization loaded");
    s::LeadLagEngine missing(market()); warm(missing);
    auto i = input(); i.outstanding = {old(s::Channel::SellUp)};
    const auto d = missing.decide(i);
    check(!d.ready && d.quotes.empty() && d.cancels.size() == 1 && d.reason == "missing_frozen_model", "missing model halts/cancels");
}

void prices_and_risk_directions() {
    auto m = model(); m.plus.intercept = .001; m.minus.intercept = -.002;
    s::LeadLagEngine e(market(), {}, m); warm(e);
    auto d = e.decide(input());
    check(d.ready && d.quotes.size() == 4 && std::abs(d.alpha + .0005) < 1e-12, "two models feed four channels");
    check(d.delta[0] == d.delta[3] && d.delta[1] == d.delta[2], "UP risk group mapping");
    for (const auto& q : d.quotes) {
        const auto& leg = q.has_bid ? q.bid : q.ask;
        check(leg.price_tick % 10'000 == 0, "tick rounding");
        check(q.has_bid ? leg.price_tick <= 490'000 : leg.price_tick >= 510'000, "quotes do not improve touch");
    }
    m = model(); m.plus.intercept = m.minus.intercept = .02;
    s::LeadLagEngine shock(market(), {}, m); warm(shock);
    auto i = input(); i.outstanding = {old(s::Channel::SellUp), old(s::Channel::BuyDown, 490'000)};
    d = shock.decide(i);
    check(d.regime == s::Regime::Shock && d.cancels.size() == 2, "positive shock cancels both dangerous channels");
    for (const auto& q : d.quotes) check((q.asset_index == 0 && q.has_bid) || (q.asset_index == 1 && q.has_ask), "shock only favorable side");
    m.plus.intercept = m.minus.intercept = -.02;
    s::LeadLagEngine negative(market(), {}, m); warm(negative);
    i.outstanding = {old(s::Channel::BuyUp, 490'000), old(s::Channel::SellDown)};
    d = negative.decide(i);
    check(d.cancels.size() == 2 && d.regime == s::Regime::Shock, "negative shock protects complementary pair");
}

void recovery_and_visibility() {
    auto m = model(); m.plus.mean[10] = m.minus.mean[10] = .5;
    m.plus.coefficients[10] = m.minus.coefficients[10] = .2;
    s::LeadLagEngine e(market(), {}, m); warm(e); update(e, 100'000, .6);
    check(e.decide(input()).regime == s::Regime::Shock, "enter shock");
    update(e, 100'100); check(e.decide(input(100'100)).regime == s::Regime::Wait, "shock starts quiet wait");
    update(e, 101'000, .55); check(e.decide(input(101'000)).regime == s::Regime::Wait, "shift resets quiet timer");
    update(e, 101'100); check(e.decide(input(101'100)).regime == s::Regime::Wait, "quiet restart");
    update(e, 102'599); check(e.decide(input(102'599)).regime == s::Regime::Wait, "1499ms insufficient");
    update(e, 102'600); check(e.decide(input(102'600)).regime == s::Regime::Calm, "1500ms recovers");
    update(e, 102'700, .75); check(e.decide(input(102'700)).regime == s::Regime::Halt, "halt threshold");
    s::LeadLagEngine future(market(), {}, model()); warm(future); update(future, 100'100);
    check(!future.decide(input()).ready, "future receive data invisible");
    s::LeadLagEngine stale(market(), {}, model()); warm(stale);
    check(stale.decide(input(101'501)).quotes.empty(), "stale data halts");
    rejects([&] { stale.on_agg_trade({100'001, 100'000, 70'000, 1, false}); }, "event after receipt rejected");
}

void retention_and_reservations() {
    s::LeadLagEngine e(market(), {}, model()); warm(e);
    auto i = input(); i.outstanding = {old(s::Channel::BuyUp, 490'000)};
    auto d = e.decide(i);
    check(d.cancels.empty() && d.quotes.size() == 3, "safe queue retained");
    i.outstanding[0].price_tick = 500'000;
    d = e.decide(i); check(d.cancels.size() == 1 && d.quotes.size() == 3, "unsafe old order cancels without immediate replacement");
    i.outstanding[0].cancel_pending = true;
    d = e.decide(i); check(d.cancels.empty() && d.quotes.size() == 3, "pending cancel is still reserved");
    s::Config cfg; cfg.candidate = s::Candidate::ShockEntry;
    s::LeadLagEngine reducing(market(), cfg, model()); warm(reducing);
    i = input(); i.known_lots = {28, 25};
    d = reducing.decide(i);
    std::int64_t reductions = 0;
    for (const auto& q : d.quotes) reductions += q.has_bid ? q.bid.quantity_lots : q.ask.quantity_lots;
    check(reductions == 3, "two reducing channels share net reduction budget");
    i.outstanding = {old(s::Channel::SellUp)}; i.outstanding[0].reserved_lots = 3;
    d = reducing.decide(i); check(d.quotes.empty(), "pending reduction consumes shared budget");
    i = input(); i.known_cash_tick = 0;
    d = e.decide(i); for (const auto& q : d.quotes) check(!q.has_bid, "cash reservation prevents unfunded buys");
    i = input(); i.known_lots = {75, 25};
    d = e.decide(i); for (const auto& q : d.quotes) check(!(q.asset_index == 0 && q.has_bid) && !(q.asset_index == 1 && q.has_ask), "combined UP exposure capped");
    // An inventory shift alone cannot justify preserving an unprofitable old order.
    i = input(); i.known_lots = {20, 40}; i.outstanding = {old(s::Channel::BuyUp, 500'000)};
    d = e.decide(i); check(d.cancels.size() == 1, "unshifted buffer required for old order retention");
}

void venue_minimum_and_common_readiness() {
    s::Config cfg; cfg.min_size_lots = 5; cfg.candidate = s::Candidate::ShockEntry;
    s::LeadLagEngine reducing(market(), cfg, model()); warm(reducing);
    auto i = input(); i.known_lots = {28, 25};
    check(reducing.decide(i).quotes.empty(), "three-share reduction cannot pass five-share venue minimum");
    i.known_lots = {30, 25};
    const auto reduction = reducing.decide(i);
    check(reduction.quotes.size() == 1, "five-share reduction shares a single inventory budget");
    auto pm = model(); pm.pm_only = true;
    s::LeadLagEngine native(market(), {}, pm);
    for (auto ms = 80'000; ms <= 100'000; ms += 100)
        native.on_books(book(0, ms), book(1, ms), ms);
    check(native.decide(input()).ready, "native PM-only does not require external feed");
    cfg = {}; cfg.pm_only_require_external_readiness = true;
    s::LeadLagEngine common(market(), cfg, pm);
    for (auto ms = 80'000; ms <= 100'000; ms += 100)
        common.on_books(book(0, ms), book(1, ms), ms);
    check(!common.decide(input()).ready, "common-availability baseline requires a ready external feed");
    s::LeadLagEngine warm_common(market(), cfg, pm); warm(warm_common);
    check(warm_common.decide(input()).ready, "common baseline becomes ready after external warmup");
    cfg.min_size_lots = 6;
    rejects([&] { s::LeadLagEngine invalid(market(), cfg, pm); }, "minimum above nominal size rejected");
}

void expiry_cancellation_race() {
    ll::LeadLagReplay replay(s::LeadLagEngine(market(), {}, model()), {100'000'000, {25, 25}});
    for (auto ms = 980'000; ms <= 999'950; ms += 100) {
        replay.add_agg_trade(btc(ms)); replay.add_books(book(0, ms), book(1, ms), ms, ms);
    }
    replay.add_decision_time(999'950);
    replay.add_decision_time(1'000'000);
    replay.add_public_trade({1'000'050, 1'000'150, 0, true, 510'000, 15});
    replay.add_public_trade({1'000'101, 1'000'201, 0, true, 510'000, 15});
    const auto result = replay.run(false);
    check(replay.fills().size() == 1 && replay.fills()[0].event_ms == 1'000'050,
        "expiry cancellation remains exposed until its latency elapses");
    check(result.actual.lots[0] == 20 && result.final_reserved_lots == 0,
        "post-expiry fill reconciles and later trade cannot fill canceled order");
}

void continuous_cross_zero_risk_classification() {
    s::LeadLagEngine e(market(), {}, model()); warm(e);
    auto i = input(); i.known_lots = {28, 25};
    const auto d = e.decide(i);
    std::size_t mixed = 0;
    trading_engine::risk::QuoteRiskEvaluator risk;
    trading_engine::risk::QuoteRiskPolicy policy;
    policy.min_edge_to_fair_tick = 2'500;
    for (const auto& q : d.quotes) {
        const auto c = static_cast<s::Channel>((q.asset_index == 0 ? 0 : 2) + (q.has_bid ? 0 : 1));
        const auto& leg = q.has_bid ? q.bid : q.ask;
        if (s::LeadLagEngine::risk_sign(c) < 0) {
            ++mixed;
            check(q.risk_mode == mm::QuoteIntentRiskMode::Mixed && !leg.risk_reducing,
                "order exceeding net reduction must carry opening risk");
        }
        const auto checked = risk.evaluate(trading_engine::risk::QuoteRiskInput{
            .quote = &q, .depth = &e.depth(c), .policy = &policy,
            .current_position_lots = i.known_lots[s::LeadLagEngine::token(c)],
            .now_ns = static_cast<std::uint64_t>(i.now_ms) * 1'000'000ULL
        });
        check(checked.approved_quote.has_value(), "cross-zero continuous quote passes normal opening-risk checks");
    }
    check(mixed == 2, "sell-UP and buy-DOWN both classified when crossing zero");
}

ll::Summary replay_case(std::int64_t trade_time, std::int64_t volume, bool halt = false, std::int64_t queue = 10) {
    auto m = model(); m.plus.coefficients[10] = m.minus.coefficients[10] = .2;
    m.plus.mean[10] = m.minus.mean[10] = .5;
    ll::LeadLagReplay replay(s::LeadLagEngine(market(), {}, m), {100'000'000, {25, 25}});
    for (auto ms = 80'000; ms <= 100'000; ms += 100) {
        replay.add_agg_trade(btc(ms)); replay.add_books(book(0, ms, .5, queue), book(1, ms, .5, queue), ms, ms);
    }
    replay.add_decision_time(100'000);
    if (halt) {
        replay.add_agg_trade(btc(100'050));
        replay.add_books(book(0, 100'050, .75), book(1, 100'050, .75), 100'050, 100'050);
        replay.add_decision_time(100'050);
    }
    replay.add_public_trade({trade_time, trade_time + 200, 0, true, 510'000, volume});
    const auto summary = replay.run();
    check(summary.actual.cash_tick == summary.known.cash_tick && summary.actual.lots == summary.known.lots, "fill notices reconcile ledgers");
    return summary;
}

void delayed_execution() {
    const auto before = replay_case(100'020, 15);
    check(before.actual.lots[0] == 25, "not filled before 25ms placement");
    const auto queue_only = replay_case(100'030, 10);
    check(queue_only.actual.lots[0] == 25, "public volume first consumes queue");
    const auto partial = replay_case(100'030, 12);
    check(partial.actual.lots[0] == 23, "fill limited to remaining public volume");
    const auto cancel_race = replay_case(100'150, 15, true);
    check(cancel_race.actual.lots[0] == 20, "trade at cancel-effective millisecond wins conservative ordering");
    check(cancel_race.pnl_min_tick == -2'450'000 && cancel_race.pnl_max_tick == 2'550'000, "settlement bounds include paired seed cost");
    const auto canceled = replay_case(100'151, 15, true);
    check(canceled.actual.lots[0] == 25, "fill after cancellation rejected");
    check(cancel_race.output_hash == replay_case(100'150, 15, true).output_hash, "replay deterministic");
    const auto no_queue = replay_case(100'030, 2, false, 0);
    check(no_queue.actual.lots[0] == 25, "missing displayed size refuses order");
}

void receive_clock_and_accounting() {
    auto m = model(); m.plus.coefficients[10] = m.minus.coefficients[10] = .2;
    m.plus.mean[10] = m.minus.mean[10] = .5;
    s::Config cfg; cfg.max_net_lots = 5;
    ll::LatencyConfig latency; latency.maker_fee_bps = 50; latency.operating_cost_tick = 1'000;
    ll::LeadLagReplay replay(s::LeadLagEngine(market(), cfg, m), {100'000'000, {25, 25}}, latency);
    for (auto ms = 80'000; ms <= 100'000; ms += 100) {
        replay.add_agg_trade(btc(ms)); replay.add_books(book(0, ms), book(1, ms), ms, ms);
    }
    replay.add_decision_time(100'000);
    // Actual public fill precedes its public feed and own fill notification.
    replay.add_public_trade({100'100, 100'300, 0, true, 510'000, 15});
    replay.add_decision_time(100'125);
    replay.add_books(book(0, 100'250, .75), book(1, 100'250, .75), 100'050, 100'250);
    replay.add_decision_time(100'250);
    for (const auto ms : {100'350, 101'100, 105'100}) {
        replay.add_books(book(0, ms), book(1, ms), ms, ms);
    }
    const auto result = replay.run(false);
    bool before_notice = false, after_notice = false, delayed_shock = false;
    for (const auto& d : replay.decisions()) {
        if (d.now_ms == 100'125) {
            before_notice = d.known_lots[0] == 25 && d.alpha == 0;
            for (const auto& q : d.quotes)
                check(!(q.asset_index == 0 && q.has_ask) && !(q.asset_index == 1 && q.has_bid), "unnotified fill still reserves net risk");
        }
        if (d.now_ms == 100'150) after_notice = d.known_lots[0] == 20;
        if (d.now_ms == 100'250) delayed_shock = d.regime == s::Regime::Halt;
    }
    check(before_notice && after_notice && delayed_shock, "source, receipt and fill notification clocks separated");
    check(result.pnl_min_tick == -2'463'750 && result.pnl_max_tick == 2'536'250, "maker fees and operating costs charged");
    check(result.settlement_pnl_tick == result.pnl_max_tick, "DOWN settlement uses actual DOWN inventory");
    check(replay.fills().size() == 1, "one bounded public fill");
    check(result.markout_quantity[0] == 5 && result.markout_quantity[1] == 5 && result.markout_quantity[2] == 5, "quantity weighted horizons populated");
    // The delayed source-100050 shock snapshot arrives after 100ms horizon,
    // but is too old to be its mark. The source-100350 snapshot is the mark.
    check(std::abs(result.weighted_markout[0] - .01) < 1e-12, "markout uses source horizon, not delivery time");
}

void late_acceptance_and_terminal_exit() {
    auto m = model(); m.plus.coefficients[10] = m.minus.coefficients[10] = .2;
    m.plus.mean[10] = m.minus.mean[10] = .5;
    ll::LatencyConfig delay; delay.submit_ms = 200;
    ll::LeadLagReplay late(s::LeadLagEngine(market(), {}, m), {100'000'000, {25, 25}}, delay);
    for (auto ms = 80'000; ms <= 100'000; ms += 100) {
        late.add_agg_trade(btc(ms)); late.add_books(book(0, ms), book(1, ms), ms, ms);
    }
    late.add_decision_time(100'000);
    late.add_books(book(0, 100'050, .75), book(1, 100'050, .75), 100'050, 100'050);
    late.add_decision_time(100'050);
    late.add_public_trade({100'250, 100'350, 0, true, 510'000, 15});
    const auto loss = late.run();
    check(loss.actual.lots[0] == 20, "late stale submit is accepted before cancellation can take effect");
    ll::LeadLagReplay terminal(s::LeadLagEngine(market(), {}, model()), {100'000'000, {25, 25}});
    for (auto ms = 80'000; ms <= 100'000; ms += 100) {
        terminal.add_agg_trade(btc(ms)); terminal.add_books(book(0, ms), book(1, ms), ms, ms);
    }
    terminal.add_decision_time(100'000);
    terminal.add_public_trade({100'030, 100'060, 0, true, 510'000, 15});
    terminal.add_terminal_reduction(100'200);
    terminal.add_decision_time(100'300);
    terminal.add_decision_time(100'500);
    const auto exit = terminal.run();
    check(terminal.fills().size() == 2 && terminal.fills()[1].terminal, "explicit terminal reduction fills once");
    check(terminal.fills()[1].event_ms == 100'475, "terminal reduction includes 275ms execution delay");
    check(exit.actual.lots[0] == 20 && exit.actual.lots[1] == 20 && exit.pnl_min_tick == 0 && exit.pnl_max_tick == 0,
          "terminal sells only net risk and keeps paired inventory");
    for (const auto& d : terminal.decisions()) check(d.now_ms < 100'200, "terminal exit cannot reopen alpha positions");
}
}

int main() {
    try {
        venue_minimum_and_common_readiness();
        expiry_cancellation_race();
        scope_and_artifact(); prices_and_risk_directions(); recovery_and_visibility();
        retention_and_reservations(); delayed_execution(); receive_clock_and_accounting();
        continuous_cross_zero_risk_classification();
        late_acceptance_and_terminal_exit();
        std::cout << "BTC15m lead-lag strategy and delayed replay checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}

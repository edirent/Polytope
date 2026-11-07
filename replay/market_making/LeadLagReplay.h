#pragma once

#include "engine/strategy/market_making/products/btc15m_lead_lag/LeadLagEngine.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"

#include <functional>
#include <map>
#include <queue>
#include <tuple>

namespace trading_engine::replay::lead_lag {
namespace strategy = strategy::market_making::products::btc15m_lead_lag;
namespace mm = trading_engine::strategy::market_making;

struct LatencyConfig {
    std::int64_t submit_ms = 25;
    std::int64_t cancel_ms = 100;
    std::int64_t fill_notice_ms = 50;
    std::int64_t terminal_reduce_ms = 275;
    double queue_factor = 1.0;
    std::int64_t maker_fee_bps = 0;
    std::int64_t taker_fee_bps = 0;
    std::int64_t operating_cost_tick = 0;
};

struct Ledger {
    std::int64_t cash_tick = 0;
    std::array<std::int64_t, 2> lots{};
};

struct PublicTrade {
    std::int64_t event_ms = 0;
    std::int64_t recv_ms = 0;
    int token = 0;
    bool taker_buy = false;
    std::int64_t price_tick = 0;
    std::int64_t quantity_lots = 0;
};

struct Fill {
    std::int64_t event_ms = 0;
    strategy::Channel channel = strategy::Channel::BuyUp;
    std::int64_t price_tick = 0;
    std::int64_t quantity_lots = 0;
    bool terminal = false;
    std::array<std::optional<double>, 3> markout{};
    bool cancel_pending = false;
};

struct Summary {
    Ledger actual, known;
    std::int64_t pnl_min_tick = 0, pnl_max_tick = 0;
    std::optional<std::int64_t> settlement_pnl_tick;
    std::array<double, 3> weighted_markout{};
    std::array<std::int64_t, 3> markout_quantity{};
    std::uint64_t submitted = 0, rejected = 0, cancel_requests = 0;
    std::map<std::string, std::uint64_t> rejection_counts;
    std::int64_t final_reserved_lots = 0;
    std::uint64_t output_hash = 0;
};

// Deterministic research execution boundary. Only approved QuoteIntent values
// enter the simulated order book. No network/live trading side effects.
class LeadLagReplay {
public:
    LeadLagReplay(strategy::LeadLagEngine engine, Ledger initial,
                  LatencyConfig latency = {}, risk::QuoteRiskPolicy policy = {});
    void add_agg_trade(const strategy::AggTrade& trade);
    void add_books(const state::MarketDepthView& up, const state::MarketDepthView& down,
                   std::int64_t event_ms, std::int64_t recv_ms);
    void add_public_trade(const PublicTrade& trade);
    void add_decision_time(std::int64_t now_ms);
    // Terminal reduction is explicit and uses visible depth after the 275ms
    // delay. It closes net risk only, is limited by depth and has no alpha entry.
    void add_terminal_reduction(std::int64_t request_ms);
    Summary run(std::optional<bool> up_result = std::nullopt);
    [[nodiscard]] const std::vector<Fill>& fills() const { return fills_; }
    [[nodiscard]] const std::vector<strategy::Decision>& decisions() const { return decisions_; }
private:
    struct Event {
        std::int64_t ms;
        int priority;
        std::uint64_t sequence;
        std::function<void()> apply;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return std::tie(a.ms, a.priority, a.sequence) > std::tie(b.ms, b.priority, b.sequence);
        }
    };
    struct Order {
        mm::QuoteIntent quote;
        strategy::OutstandingOrder reservation;
        std::int64_t actual_remaining = 0;
        double queue_ahead = 0;
        bool active = false;
        bool cancel_scheduled = false;
    };
    void schedule(std::int64_t ms, int priority, std::function<void()> action);
    void decide();
    void request_cancel(std::uint64_t id);
    void cancel_effective(std::uint64_t id);
    void trade(const PublicTrade& trade);
    void fill(Order& order, std::int64_t qty);
    void update_marks();
    void terminal_reduce();
    strategy::LeadLagEngine engine_;
    LatencyConfig latency_;
    risk::QuoteRiskPolicy policy_;
    risk::QuoteRiskEvaluator risk_;
    Ledger initial_, actual_, known_;
    std::array<state::MarketDepthView, 2> visible_books_{};
    std::map<std::uint64_t, Order> orders_;
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    std::vector<Fill> fills_;
    std::vector<strategy::Decision> decisions_;
    std::int64_t now_ms_ = 0;
    std::int64_t mark_source_ms_ = 0;
    std::uint64_t sequence_ = 0;
    bool started_ = false;
    bool terminal_requested_ = false;
    Summary summary_;
};
}

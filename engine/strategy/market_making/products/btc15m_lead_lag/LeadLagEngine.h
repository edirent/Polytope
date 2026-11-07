#pragma once

#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::strategy::market_making::products::btc15m_lead_lag {

// Channel order is part of the frozen artifact/replay contract.
enum class Channel : std::uint8_t { BuyUp, SellUp, BuyDown, SellDown };
enum class Regime : std::uint8_t { Calm, Shift, Shock, Halt, Wait };
enum class Candidate : std::uint8_t { Continuous, ShockEntry };
inline constexpr std::array<const char*, 13> kFeatureNames{
    "btc_100ms", "btc_250ms", "btc_1s", "btc_5s", "imbalance_250ms",
    "pm_500ms", "pm_1s", "pm_5s", "pair_gap", "token_spread",
    "anchor", "tau_seconds", "token_deviation"
};
using Features = std::array<double, 13>;

struct RidgeModel {
    Features mean{};
    Features scale{};
    Features coefficients{};
    double intercept = 0;
    [[nodiscard]] double predict(const Features& x) const;
};

struct FrozenModel {
    RidgeModel plus;   // Buy UP and sell DOWN.
    RidgeModel minus;  // Sell UP and buy DOWN.
    std::int64_t trained_until_ms = 0;
    std::uint64_t artifact_hash = 0;
    bool synthetic = false;
    bool pm_only = false;
    // Clip belongs to preprocessing and is frozen with the model.
    double external_feature_clip = 0.25;
    [[nodiscard]] static FrozenModel load(const std::string& path);
};

struct Config {
    std::uint64_t strategy_id = 15;
    Candidate candidate = Candidate::Continuous;
    std::int64_t grid_tick = 10'000;
    std::int64_t buffer_tick = 2'500;
    std::int64_t inventory_skew_tick_per_lot = 250;
    std::int64_t queue_tolerance_tick = 10'000;
    std::int64_t size_lots = 5;
    std::int64_t min_size_lots = 1;
    bool pm_only_require_external_readiness = false;
    std::int64_t max_net_lots = 50;
    std::int64_t max_token_lots = 100;
    std::int64_t max_data_age_ms = 1'500;
    std::int64_t quiet_recovery_ms = 1'500;
    std::int64_t quote_ttl_ms = 5'000;
    std::int64_t calm_tick = 5'000;
    std::int64_t shock_tick = 15'000;
    std::int64_t halt_tick = 40'000;
};

struct Market {
    std::string market_id;
    std::array<std::string, 2> token_ids;
    std::array<std::uint32_t, 2> asset_indices{0, 1};
    std::uint32_t market_index = 0;
    std::int64_t start_ms = 0;
    std::int64_t expiry_ms = 0;
    std::string symbol = "BTCUSDT";
};

struct AggTrade {
    std::int64_t event_ms = 0;
    std::int64_t recv_ms = 0;
    double price = 0;
    double quantity = 0;
    bool buyer_is_maker = false;
};

// Supply remaining reservations, including submissions, cancels and fills not
// yet notified. Never populate known inventory from the actual replay ledger.
struct OutstandingOrder {
    Channel channel = Channel::BuyUp;
    std::uint64_t quote_id = 0;
    std::uint64_t group_id = 0;
    std::int64_t price_tick = 0;
    std::int64_t reserved_lots = 0;
    bool acknowledged = false;
    bool cancel_pending = false;
    std::int64_t expires_ms = 0;
};

struct Input {
    std::int64_t now_ms = 0;
    std::array<std::int64_t, 2> known_lots{};
    std::int64_t known_cash_tick = 0;
    std::vector<OutstandingOrder> outstanding;
};

struct Decision {
    std::int64_t now_ms = 0;
    std::array<std::int64_t, 2> known_lots{};
    std::int64_t known_cash_tick = 0;
    bool ready = false;
    std::string reason;
    Regime regime = Regime::Halt;
    double anchor = 0;
    double alpha = 0;
    std::array<double, 4> delta{};
    std::array<Features, 4> features{};
    std::vector<QuoteIntent> quotes;
    std::vector<CancelQuoteIntent> cancels;
};

class LeadLagEngine {
public:
    LeadLagEngine(Market market, Config config = {},
                  std::optional<FrozenModel> model = std::nullopt);
    // Call only after recv_ms has arrived. Out-of-order source timestamps may
    // contribute volume but never overwrite a newer source price.
    void on_agg_trade(const AggTrade& trade);
    void on_books(const state::MarketDepthView& up,
                  const state::MarketDepthView& down, std::int64_t recv_ms);
    [[nodiscard]] Decision decide(const Input& input);
    [[nodiscard]] const Market& market() const { return market_; }
    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] const state::MarketDepthView& depth(Channel channel) const;
    [[nodiscard]] static int risk_sign(Channel channel);
    [[nodiscard]] static int token(Channel channel);
    [[nodiscard]] static bool is_bid(Channel channel);
private:
    struct Point { std::int64_t ms; double value; };
    [[nodiscard]] std::optional<double> lagged(
        const std::deque<Point>& history, std::int64_t ms) const;
    [[nodiscard]] std::optional<Features> features(
        int token_index, std::int64_t now_ms) const;
    void update_regime(double alpha, std::int64_t now_ms);
    Market market_;
    Config config_;
    std::optional<FrozenModel> model_;
    std::array<state::MarketDepthView, 2> books_{};
    std::deque<Point> btc_history_, pm_history_;
    std::deque<AggTrade> trades_;
    std::int64_t latest_source_ms_ = -1;
    std::int64_t latest_recv_ms_ = -1;
    std::int64_t book_recv_ms_ = -1;
    std::int64_t last_decision_ms_ = -1;
    std::optional<std::int64_t> quiet_since_ms_;
    bool recovering_ = false;
    Regime regime_ = Regime::Halt;
};

[[nodiscard]] const char* regime_name(Regime regime);

}  // namespace trading_engine::strategy::market_making::products::btc15m_lead_lag

#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/execution/public/ExecutionTypes.h"
#include "engine/paper/ledger/MakerFillApplication.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/pnl/MakerPnLEngine.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/strategy/market_making/canonical/CanonicalExposureMapper.h"
#include "engine/strategy/market_making/canonical/CanonicalMarketState.h"
#include "engine/strategy/market_making/canonical/CanonicalPriceMapper.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/fair/DigitalOptionFairModel.h"
#include "engine/strategy/market_making/fair/ExternalFairModel.h"
#include "engine/strategy/market_making/fair/ExternalFairRuntime.h"
#include "engine/strategy/market_making/fair/FixedVolProvider.h"
#include "engine/strategy/market_making/fair/InMemorySpotOracle.h"
#include "engine/strategy/market_making/fair/MarketImpliedFairModel.h"
#include "engine/strategy/market_making/fair/TradableFairBuilder.h"
#include "engine/strategy/market_making/inventory/DynamicInventoryTargeter.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"
#include "engine/strategy/market_making/research/CanonicalQuoteResearchLogger.h"
#include "engine/strategy/market_making/research/ExternalFairBasisLogger.h"
#include "engine/strategy/market_making/risk/PortfolioTouchRiskManager.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/WebSocketClient.h"
#include "state/book/DepthPrefix.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace decode = trading_engine::decode;
namespace execution = trading_engine::execution;
namespace feed = trading_engine::feed;
namespace mm = trading_engine::strategy::market_making;
namespace mm_research = trading_engine::strategy::market_making::research;
namespace paper = trading_engine::paper;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr std::int64_t kTicksPerDollar = 1'000'000;
constexpr double kSecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0;

struct Config {
    std::uint64_t seconds = 1'800;
    std::uint64_t ping_interval_ms = 10'000;
    std::uint64_t dashboard_interval_ms = 1'000;
    std::string asset_id;
    std::string market_id{"world-cup"};
    std::string endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };
    std::filesystem::path dashboard_file;
    std::filesystem::path out_json;
    std::uint64_t window_end_unix_seconds = 0;

    std::int64_t starting_cash_tick = 1'000 * kTicksPerDollar;
    std::string complement_asset_id;
    std::int64_t half_spread_tick = 5'000;
    std::int64_t quote_size_lots = 10;
    std::int64_t inventory_skew_tick = 150'000;
    std::int64_t inventory_skew_nonlinear_start_bps = 2'000;
    double inventory_skew_exponent = 2.0;
    std::int64_t target_position_lots = 0;
    std::int64_t min_inventory_lots = 0;
    std::int64_t max_inventory_lots = 100;
    std::int64_t max_fill_qty_per_trade = 10;
    std::int64_t initial_position_lots = 0;
    std::int64_t initial_position_price_tick = 0;
    std::int64_t initial_complement_position_lots = 0;
    std::int64_t initial_complement_position_price_tick = 0;
    bool seed_complete_set = false;
    bool pure_taker_mode = false;
    std::int64_t min_quote_edge_tick = 0;
    std::int64_t adverse_selection_buffer_tick = 4'000;
    std::uint64_t book_quarantine_ms = 75;
    bool fair_basis_smoothing_enabled = true;
    std::int64_t fair_basis_seed_tick = -34'800;
    double fair_basis_ewma_alpha = 0.02;
    std::uint64_t fair_basis_update_interval_ms = 1'000;
    std::int64_t fair_basis_min_tick = -100'000;
    std::int64_t fair_basis_max_tick = 100'000;
    bool lead_lag_sniping_enabled = false;
    double lead_lag_min_move_500ms_bps = 2.0;
    std::int64_t lead_lag_min_stale_edge_tick = 5'000;
    std::uint64_t lead_lag_cooldown_ms = 250;
    std::int64_t lead_lag_taker_size_lots = 5;
    bool macro_divergence_taker_enabled = true;
    double macro_divergence_ewma_alpha = 0.01;  // EWMA fair (slow oracle)
    double basis_ewma_alpha = 0.001;            // EWMA basis (local-mid - fair)
    std::int64_t macro_shock_min_edge_tick = 20'000;
    std::int64_t max_allowed_spread_tick = 20'000;
    std::int64_t max_allowed_basis_tick = 300'000;
    std::uint64_t macro_divergence_cooldown_ms = 60'000;
    std::int64_t macro_divergence_taker_size_lots = 10;
    std::uint64_t max_taker_fills_per_minute = 10;
    std::int64_t taker_max_entry_mid_slippage_tick = 2'000;
    bool locked_book_taker_hunter_enabled = true;
    std::int64_t locked_book_taker_min_edge_tick = 10'000;
    std::uint64_t locked_book_taker_cooldown_ms = 250;
    std::int64_t locked_book_taker_size_lots = 5;
    bool momentum_bid_shutoff_enabled = true;
    double momentum_bid_shutoff_500ms_bps = 1.0;
    double momentum_bid_shutoff_1s_bps = 2.0;
    double assumed_latency_ms = 2.0;
    std::int64_t latency_buffer_tick_per_ms = 250;
    std::int64_t min_top_depth_lots = 1;
    std::int64_t min_total_depth_lots = 1;
    std::int64_t min_book_spread_tick = 5'000;
    std::int64_t max_book_spread_tick = 0;
    std::int64_t max_book_spread_bps = 0;
    std::int64_t max_quote_fair_deviation_tick = 20'000;
    std::int64_t max_quote_fair_deviation_bps = 0;
    std::int64_t min_fair_confidence_bps = 6'000;
    std::int64_t complement_fair_weight_bps = 3'000;
    std::int64_t external_fair_value_tick = 0;
    std::int64_t external_fair_weight_bps = 0;
    bool external_fair_invert = false;
    bool require_external_fair_for_opening_quotes = true;
    double btc_spot = 0.0;
    double btc_threshold = 0.0;
    double btc_vol_annual_bps = 0.0;
    double btc_drift_annual_bps = 0.0;
    bool btc_oracle_enabled = false;
    std::string btc_oracle_endpoint{
        "wss://data-stream.binance.vision/ws/btcusdt@trade"
    };
    std::uint64_t btc_oracle_max_age_ms = 2'000;
    double btc_toxic_move_1s_bps = 100.0;
    bool btc_use_realized_vol = false;
    std::uint64_t btc_realized_vol_window_seconds = 300;
    double btc_min_realized_vol_annual_bps = 1'000.0;
    double btc_max_realized_vol_annual_bps = 20'000.0;
    bool sol_external_fair_enabled = true;
    bool external_fair_basis_log = false;
    bool canonical_quote_research_log = false;
    bool external_fair_shadow_only = false;
    double external_fair_tradable_lambda = 0.20;
    bool dynamic_inventory_targeter_enabled = true;
    bool portfolio_touch_risk_enabled = true;
    double sol_fixed_vol_annualized = 0.90;
    double sol_spot_bid = 0.0;
    double sol_spot_ask = 0.0;
    std::string sol_spot_feed{"manual"};
    std::string sol_spot_feed_endpoint{
        "wss://data-stream.binance.vision/ws/solusdt@bookTicker"
    };
    double sol_spot_max_spread_bps = 20.0;
    std::string sol_external_fair_outcome_side{"yes"};
    bool enable_as_model = false;
    double as_risk_aversion = 0.05;
    double as_order_arrival_k = 0.02;
    double as_toxic_spread_multiplier = 3.0;
    std::int64_t as_min_half_spread_tick = 10'000;
    std::int64_t as_max_half_spread_tick = 100'000;
    std::int64_t as_min_inventory_skew_tick = 0;
    std::int64_t as_max_inventory_skew_tick = 250'000;
    std::int64_t max_quote_size_multiplier_bps = 20'000;
    std::int64_t passive_unwind_position_bps = 7'000;
    std::int64_t forced_unwind_position_bps = 9'000;
    std::int64_t passive_unwind_aggression_tick = 0;
    std::int64_t passive_reduce_excess_lots = 20;
    std::int64_t urgent_reduce_excess_lots = 50;
    std::int64_t passive_reduce_join_tick = 1;
    std::int64_t urgent_unwind_aggression_tick = 0;
    std::uint64_t urgent_reduce_age_ms = 30'000;
    std::uint64_t puke_reduce_age_ms = 120'000;
    std::int64_t urgent_reduce_pressure_bps = 5'000;
    std::int64_t puke_reduce_pressure_bps = 9'000;
    bool reduce_only_quote_to_target = true;
    std::uint64_t tte_skew_start_seconds = 120;
    std::uint64_t tte_puke_start_seconds = 30;
    double tte_max_skew_multiplier = 4.0;
    std::uint64_t queue_min_rest_ms = 250;
    std::uint64_t min_requote_interval_ms = 2;
    std::int64_t min_quote_price_change_tick = 10'000;
    execution::PaperMakerFillMode fill_mode =
        execution::PaperMakerFillMode::BookCross;
};

struct LatencyStats {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

struct Stats {
    std::atomic<std::uint64_t> ws_packets{0};
    std::atomic<std::uint64_t> normalized_events{0};
    std::atomic<std::uint64_t> filtered_events{0};
    std::atomic<std::uint64_t> book_snapshots{0};
    std::atomic<std::uint64_t> book_deltas{0};
    std::atomic<std::uint64_t> snapshots_published{0};
    std::atomic<std::uint64_t> depth_updates{0};

    std::atomic<std::uint64_t> mm_quote_intents{0};
    std::atomic<std::uint64_t> mm_cancel_intents{0};
    std::atomic<std::uint64_t> mm_rejected_no_quote{0};
    std::array<
        std::atomic<std::uint64_t>,
        mm::kNoQuoteReasonCount
    > no_quote_reasons{};
    std::atomic<std::uint64_t> latest_fair_value_quality{
        static_cast<std::uint64_t>(mm::FairValueQuality::Disabled)
    };
    std::atomic<std::int64_t> latest_fair_confidence_bps{0};
    std::atomic<std::int64_t> latest_fair_book_spread_tick{0};
    std::atomic<std::int64_t> latest_fair_value_tick{0};
    std::atomic<std::int64_t> latest_external_fair_raw_tick{0};
    std::atomic<std::int64_t> latest_external_fair_adjusted_tick{0};
    std::atomic<std::int64_t> latest_fair_basis_tick{0};
    std::atomic<std::uint64_t> risk_evaluated{0};
    std::atomic<std::uint64_t> risk_approved{0};
    std::atomic<std::uint64_t> risk_rejected{0};
    std::array<
        std::atomic<std::uint64_t>,
        risk::kQuoteRiskDecisionTypeCount
    > risk_decisions{};

    std::atomic<std::uint64_t> submitted_quotes{0};
    std::atomic<std::uint64_t> submit_errors{0};
    std::atomic<std::uint64_t> replaced_quotes{0};
    std::atomic<std::uint64_t> duplicate_ignored{0};
    std::atomic<std::uint64_t> cancelled_quotes{0};
    std::atomic<std::uint64_t> cancel_errors{0};

    std::atomic<std::uint64_t> maker_reports{0};
    std::atomic<std::uint64_t> maker_fills_applied{0};
    std::atomic<std::uint64_t> maker_fills_rejected{0};
    std::atomic<std::uint64_t> gross_fill_notional_tick{0};
    std::atomic<std::uint64_t> book_quarantine_events{0};
    std::atomic<std::uint64_t> depth_updates_quarantined{0};
    std::atomic<std::uint64_t> momentum_bid_shutoffs{0};
    std::atomic<std::uint64_t> lead_lag_sniping_signals{0};
    std::atomic<std::uint64_t> lead_lag_taker_ioc_fills_applied{0};
    std::atomic<std::uint64_t> macro_divergence_sniping_signals{0};
    std::atomic<std::uint64_t> macro_divergence_taker_ioc_fills_applied{0};
    std::atomic<std::uint64_t> macro_structural_dislocation_blocked{0};
    std::atomic<std::uint64_t> macro_basis_uninitialized_blocked{0};
    std::atomic<std::uint64_t> macro_basis_insanity_blocked{0};
    std::atomic<std::uint64_t> macro_edge_not_crossing_blocked{0};
    std::atomic<std::uint64_t> taker_circuit_breaker_tripped{0};
    std::atomic<std::int64_t> latest_ewma_fair_yes_tick{0};
    std::atomic<std::int64_t> latest_ewma_basis_yes_tick{0};
    std::atomic<std::uint64_t> latest_lead_lag_side{0};
    std::atomic<std::int64_t> latest_lead_lag_edge_tick{0};
    std::atomic<std::int64_t> latest_lead_lag_price_tick{0};
    std::atomic<std::uint64_t> taker_ioc_signals{0};
    std::atomic<std::uint64_t> taker_ioc_fills_applied{0};
    std::atomic<std::uint64_t> taker_ioc_yes_fills_applied{0};
    std::atomic<std::uint64_t> taker_ioc_no_fills_applied{0};
    std::atomic<std::uint64_t> taker_ioc_fills_rejected{0};
    std::atomic<std::uint64_t> taker_ioc_cooldown_blocked{0};
    std::atomic<std::uint64_t> taker_ioc_inventory_blocked{0};
    std::atomic<std::uint64_t> taker_ioc_cash_blocked{0};
    std::atomic<std::uint64_t> taker_ioc_mid_slippage_blocked{0};
    std::atomic<std::uint64_t> taker_ioc_notional_tick{0};
    std::atomic<std::int64_t> taker_ioc_expected_edge_tick{0};
    std::atomic<std::int64_t> latest_taker_ioc_edge_tick{0};
    std::atomic<std::int64_t> latest_taker_ioc_price_tick{0};
    std::atomic<std::int64_t> latest_taker_ioc_qty_lots{0};
    std::atomic<std::int64_t> latest_taker_ioc_mid_tick{0};
    std::atomic<std::int64_t> latest_taker_ioc_mid_slippage_tick{0};
    std::atomic<std::uint64_t> latest_taker_ioc_asset_side{0};
    std::atomic<std::uint64_t> latest_taker_ioc_source{0};
    std::atomic<std::uint64_t> reduce_exit_quotes_submitted{0};
    std::atomic<std::uint64_t> reduce_exit_passive_quotes{0};
    std::atomic<std::uint64_t> reduce_exit_urgent_quotes{0};
    std::atomic<std::uint64_t> reduce_exit_puke_quotes{0};
    std::atomic<std::uint64_t> latest_reduce_exit_asset_side{0};
    std::atomic<std::uint64_t> latest_reduce_exit_stage{0};
    std::atomic<std::uint64_t> latest_reduce_exit_age_ms{0};
    std::atomic<std::int64_t> latest_reduce_exit_excess_lots{0};
    std::atomic<std::int64_t> latest_reduce_exit_pressure_bps{0};
    std::atomic<std::int64_t> latest_reduce_exit_price_tick{0};
    std::atomic<std::int64_t> latest_reduce_exit_qty_lots{0};

    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> state_errors{0};
    std::atomic<std::uint64_t> transport_errors{0};
    std::atomic<std::uint64_t> dashboard_write_errors{0};
    std::atomic<std::uint64_t> ping_sent{0};
    std::atomic<std::uint64_t> pong_received{0};
    std::atomic<std::uint64_t> latest_pipeline_latency_ns{0};
};

struct RecentFill {
    std::uint64_t report_id = 0;
    std::uint64_t quote_id = 0;
    std::uint64_t quote_group_id = 0;
    std::string asset_id;
    std::string side;
    std::int64_t qty_lots = 0;
    std::int64_t fill_price_tick = 0;
    std::int64_t remaining_qty_lots = 0;
    std::string reason;
    std::string liquidity_role{"maker"};
    std::int64_t expected_edge_tick = 0;
    std::uint64_t ts_ns = 0;
    std::int64_t mark_at_fill_tick = 0;
    bool markout_1s_ready = false;
    std::int64_t markout_1s_tick = 0;
    bool markout_5s_ready = false;
    std::int64_t markout_5s_tick = 0;
    bool markout_30s_ready = false;
    std::int64_t markout_30s_tick = 0;
};

enum class ReduceExitStage : std::uint64_t {
    None = 0,
    Passive = 1,
    Urgent = 2,
    Puke = 3
};

struct ReduceQuoteState {
    std::uint64_t quote_group_id = 0;
    bool quote_active = false;
    std::uint64_t excess_since_ns = 0;
    ReduceExitStage latest_stage = ReduceExitStage::None;
    std::uint64_t latest_age_ms = 0;
    std::int64_t latest_excess_lots = 0;
    std::int64_t latest_pressure_bps = 0;
    std::int64_t latest_price_tick = 0;
    std::int64_t latest_qty_lots = 0;
};

struct PnLAttribution {
    std::int64_t seed_position_lots = 0;
    std::int64_t seed_cost_basis_tick = 0;
    std::int64_t seed_realized_pnl_tick = 0;
    std::uint64_t seed_sell_fill_count = 0;
    std::int64_t seed_complement_position_lots = 0;
    std::int64_t seed_complement_cost_basis_tick = 0;
    std::int64_t seed_complement_realized_pnl_tick = 0;

    std::int64_t strategy_position_lots = 0;
    std::int64_t strategy_cost_basis_tick = 0;
    std::int64_t strategy_complement_position_lots = 0;
    std::int64_t strategy_complement_cost_basis_tick = 0;
    std::int64_t strategy_realized_pnl_tick = 0;
    std::int64_t strategy_complement_realized_pnl_tick = 0;
    std::int64_t strategy_spread_capture_tick = 0;
    std::uint64_t strategy_buy_fill_count = 0;
    std::uint64_t strategy_sell_fill_count = 0;
    std::uint64_t strategy_complement_buy_fill_count = 0;
    std::uint64_t strategy_complement_sell_fill_count = 0;
};

struct RuntimeState {
    explicit RuntimeState(const Config& config)
        : ledger(config.starting_cash_tick),
          execution_adapter(execution_config(config)) {
        primary_asset_id = config.asset_id;
        complement_asset_id = config.complement_asset_id;
        pnl = pnl_engine.compute(
            ledger,
            std::span<const state::MarketDepthView>{},
            0
        );
        high_watermark_tick = pnl.equity_mid_tick;
        if (config.fair_basis_smoothing_enabled) {
            fair_basis_tick = std::clamp(
                config.fair_basis_seed_tick,
                config.fair_basis_min_tick,
                config.fair_basis_max_tick
            );
            fair_basis_initialized = true;
        }
    }

    static execution::PaperMakerExecutionConfig execution_config(
        const Config& config
    ) {
        execution::PaperMakerExecutionConfig out;
        out.fill_mode = config.fill_mode;
        out.allow_partial_fills = true;
        out.max_fill_qty_per_trade = config.max_fill_qty_per_trade;
        out.queue_min_rest_ns = config.queue_min_rest_ms * 1'000'000ULL;
        return out;
    }

    void seed_fill(
        const std::string& asset_id,
        std::uint32_t asset_index,
        std::int64_t lots,
        std::int64_t price_tick,
        std::uint64_t salt
    ) {
        if (lots <= 0) {
            return;
        }
        if (price_tick <= 0) {
            throw std::runtime_error(
                "seed position price tick must be > 0 when initial lots are set"
            );
        }
        paper::PaperFill seed;
        seed.fill_id = 0x9e3779b97f4a7c15ULL ^
                       salt ^
                       static_cast<std::uint64_t>(lots) ^
                       static_cast<std::uint64_t>(asset_index) ^
                       static_cast<std::uint64_t>(price_tick);
        seed.report_id = seed.fill_id;
        seed.asset_index = asset_index;
        seed.asset_id = asset_id;
        seed.side = paper::Side::Buy;
        seed.liquidity_role = paper::FillLiquidityRole::Maker;
        seed.qty_lots = lots;
        seed.fill_price_tick = price_tick;
        seed.ts_ns = 0;
        const auto result = ledger.apply_fill(seed);
        if (!result.applied) {
            throw std::runtime_error(
                "failed to seed initial position: " + result.reason
            );
        }
    }

    void seed_initial_position(
        const Config& config,
        std::uint32_t asset_index,
        std::uint32_t complement_asset_index,
        bool has_complement_asset_index
    ) {
        if (initial_position_seeded || config.initial_position_lots <= 0) {
            return;
        }
        if (config.seed_complete_set && !has_complement_asset_index) {
            return;
        }

        seed_fill(
            config.asset_id,
            asset_index,
            config.initial_position_lots,
            config.initial_position_price_tick,
            0x11ULL
        );
        attribution.seed_position_lots = config.initial_position_lots;
        attribution.seed_cost_basis_tick =
            config.initial_position_lots *
            config.initial_position_price_tick;

        const auto complement_lots =
            config.seed_complete_set &&
                    config.initial_complement_position_lots <= 0
                ? config.initial_position_lots
                : config.initial_complement_position_lots;
        if (complement_lots > 0) {
            if (config.complement_asset_id.empty() ||
                !has_complement_asset_index) {
                throw std::runtime_error(
                    "complement seed requires --complement-asset-id and a complement book"
                );
            }
            const auto complement_price =
                config.initial_complement_position_price_tick > 0
                    ? config.initial_complement_position_price_tick
                    : std::clamp<std::int64_t>(
                          kTicksPerDollar -
                              config.initial_position_price_tick,
                          1,
                          kTicksPerDollar - 1
                      );
            seed_fill(
                config.complement_asset_id,
                complement_asset_index,
                complement_lots,
                complement_price,
                0x22ULL
            );
            attribution.seed_complement_position_lots = complement_lots;
            attribution.seed_complement_cost_basis_tick =
                complement_lots * complement_price;
        }
        initial_position_seeded = true;
    }

    void attribute_fill_side(
        const std::string& asset_id,
        bool buy,
        std::int64_t qty_lots,
        std::int64_t fill_price_tick,
        std::int64_t mark_at_fill_tick
    ) {
        if (qty_lots <= 0 || fill_price_tick <= 0) {
            return;
        }
        const auto is_complement =
            !complement_asset_id.empty() && asset_id == complement_asset_id;
        auto& strategy_position =
            is_complement ? attribution.strategy_complement_position_lots
                          : attribution.strategy_position_lots;
        auto& strategy_cost =
            is_complement ? attribution.strategy_complement_cost_basis_tick
                          : attribution.strategy_cost_basis_tick;
        auto& strategy_realized =
            is_complement ? attribution.strategy_complement_realized_pnl_tick
                          : attribution.strategy_realized_pnl_tick;
        auto& strategy_buy_count =
            is_complement ? attribution.strategy_complement_buy_fill_count
                          : attribution.strategy_buy_fill_count;
        auto& strategy_sell_count =
            is_complement ? attribution.strategy_complement_sell_fill_count
                          : attribution.strategy_sell_fill_count;
        auto& seed_position =
            is_complement ? attribution.seed_complement_position_lots
                          : attribution.seed_position_lots;
        auto& seed_cost =
            is_complement ? attribution.seed_complement_cost_basis_tick
                          : attribution.seed_cost_basis_tick;
        auto& seed_realized =
            is_complement ? attribution.seed_complement_realized_pnl_tick
                          : attribution.seed_realized_pnl_tick;
        auto& seed_sell_count = attribution.seed_sell_fill_count;

        if (buy) {
            const auto notional = qty_lots * fill_price_tick;
            strategy_position += qty_lots;
            strategy_cost += notional;
            ++strategy_buy_count;
            if (mark_at_fill_tick > 0) {
                attribution.strategy_spread_capture_tick +=
                    (mark_at_fill_tick - fill_price_tick) * qty_lots;
            }
            return;
        }

        auto remaining = qty_lots;
        if (strategy_position > 0 && remaining > 0) {
            const auto qty = std::min(remaining, strategy_position);
            const auto avg_cost = strategy_cost / strategy_position;
            strategy_realized +=
                (fill_price_tick - avg_cost) * qty;
            strategy_position -= qty;
            strategy_cost -= avg_cost * qty;
            remaining -= qty;
            ++strategy_sell_count;
        }
        if (seed_position > 0 && remaining > 0) {
            const auto qty = std::min(remaining, seed_position);
            const auto avg_cost = seed_cost / seed_position;
            seed_realized += (fill_price_tick - avg_cost) * qty;
            seed_position -= qty;
            seed_cost -= avg_cost * qty;
            remaining -= qty;
            ++seed_sell_count;
        }
        if (mark_at_fill_tick > 0) {
            attribution.strategy_spread_capture_tick +=
                (fill_price_tick - mark_at_fill_tick) * qty_lots;
        }
    }

    void attribute_fill(
        const execution::MakerExecutionReport& report,
        std::int64_t mark_at_fill_tick
    ) {
        attribute_fill_side(
            report.asset_id,
            report.side == execution::QuoteSide::Bid,
            report.filled_qty_lots,
            report.avg_fill_price_tick,
            mark_at_fill_tick
        );
    }

    void attribute_fill(
        const paper::PaperFill& fill,
        std::int64_t mark_at_fill_tick
    ) {
        attribute_fill_side(
            fill.asset_id,
            fill.side == paper::Side::Buy,
            fill.qty_lots,
            fill.fill_price_tick,
            mark_at_fill_tick
        );
    }

    std::mutex mutex;
    paper::PaperLedger ledger;
    execution::PaperMakerExecutionAdapter execution_adapter;
    paper::PaperEventAdapter event_adapter;
    paper::MakerPnLEngine pnl_engine;
    paper::MakerPnLSnapshot pnl;
    state::MarketDepthView last_depth;
    state::MarketDepthView last_complement_depth;
    bool has_depth = false;
    bool has_complement_depth = false;
    bool initial_position_seeded = false;
    std::int64_t high_watermark_tick = 0;
    std::int64_t max_drawdown_tick = 0;
    std::uint64_t dashboard_seq_no = 0;
    std::uint64_t dashboard_samples = 0;
    bool fair_basis_initialized = false;
    std::int64_t fair_basis_tick = 0;
    std::uint64_t fair_basis_updates = 0;
    std::uint64_t last_fair_basis_update_ns = 0;
    std::uint64_t last_lead_lag_signal_ns = 0;
    std::uint64_t last_macro_divergence_signal_ns = 0;
    std::uint64_t last_locked_book_taker_ns = 0;
    bool ewma_fair_initialized = false;
    std::int64_t ewma_fair_yes_tick = 0;
    bool basis_initialized = false;
    double ewma_basis_yes_tick = 0.0;
    std::int64_t macro_prev_yes_edge_tick = 0;
    std::int64_t macro_prev_no_edge_tick = 0;
    std::deque<std::uint64_t> taker_fill_timestamps_ns;
    std::uint64_t next_taker_ioc_report_id = 1;
    ReduceQuoteState yes_reduce_quote;
    ReduceQuoteState no_reduce_quote;
    std::uint64_t book_quarantine_until_ns = 0;
    std::vector<RecentFill> recent_fills;
    PnLAttribution attribution;
    std::string primary_asset_id;
    std::string complement_asset_id;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t now_ns() {
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

std::int64_t current_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
    );
}

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()
    );
}

bool contains_fragment(
    const std::string& value,
    const std::string_view fragment
) {
    return value.find(fragment) != std::string::npos;
}

mm::OutcomeSide parse_outcome_side(const std::string& value) {
    if (value == "no" || value == "NO" || value == "No") {
        return mm::OutcomeSide::No;
    }
    return mm::OutcomeSide::Yes;
}

mm::OutcomeSide opposite_outcome_side(mm::OutcomeSide side) {
    return side == mm::OutcomeSide::Yes ? mm::OutcomeSide::No
                                        : mm::OutcomeSide::Yes;
}

bool populate_sol_external_fair_spec(
    const Config& config,
    const std::string& token_id,
    mm::OutcomeSide outcome_side,
    mm::ExternalFairMarketSpec* spec
) {
    if (!config.sol_external_fair_enabled || token_id.empty() ||
        config.window_end_unix_seconds == 0 || spec == nullptr) {
        return false;
    }

    mm::ExternalFairEventType event_type = mm::ExternalFairEventType::Unknown;
    double barrier = 0.0;
    if (contains_fragment(config.market_id, "reach-90") ||
        contains_fragment(config.market_id, "above-90")) {
        event_type = mm::ExternalFairEventType::UpTouch;
        barrier = 90.0;
    } else if (contains_fragment(config.market_id, "dip-to-60") ||
               contains_fragment(config.market_id, "below-60") ||
               contains_fragment(config.market_id, "below60")) {
        event_type = mm::ExternalFairEventType::DownTouch;
        barrier = 60.0;
    } else if (contains_fragment(config.market_id, "dip-to-50") ||
               contains_fragment(config.market_id, "below-50") ||
               contains_fragment(config.market_id, "below50")) {
        event_type = mm::ExternalFairEventType::DownTouch;
        barrier = 50.0;
    } else {
        return false;
    }

    spec->market_id = config.market_id;
    spec->token_id = token_id;
    spec->symbol = mm::ExternalFairSymbol::SOL;
    spec->event_type = event_type;
    spec->outcome_side = outcome_side;
    spec->barrier_price = barrier;
    spec->expiry_unix_ms =
        static_cast<std::int64_t>(config.window_end_unix_seconds) * 1000LL;
    spec->price_scale_tick = mm::kPriceOneTick;
    return true;
}

std::filesystem::path external_fair_basis_log_path(const Config& config) {
    std::filesystem::path run_dir;
    if (!config.dashboard_file.empty() &&
        !config.dashboard_file.parent_path().empty()) {
        run_dir = config.dashboard_file.parent_path();
    } else if (!config.out_json.empty() &&
               !config.out_json.parent_path().empty()) {
        run_dir = config.out_json.parent_path();
    } else {
        run_dir = std::filesystem::current_path();
    }
    return run_dir / "external_fair_basis_snapshots.csv";
}

std::filesystem::path canonical_quote_research_log_path(
    const Config& config
) {
    std::filesystem::path run_dir;
    if (!config.dashboard_file.empty() &&
        !config.dashboard_file.parent_path().empty()) {
        run_dir = config.dashboard_file.parent_path();
    } else if (!config.out_json.empty() &&
               !config.out_json.parent_path().empty()) {
        run_dir = config.out_json.parent_path();
    } else {
        run_dir = std::filesystem::current_path();
    }
    return run_dir / "canonical_quote_research.csv";
}

std::uint64_t now_unix_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count()
    );
}

std::uint64_t time_to_expiry_ns(const Config& config) {
    if (config.window_end_unix_seconds == 0) {
        return 0;
    }
    const auto now = now_unix_seconds();
    if (config.window_end_unix_seconds <= now) {
        return 0;
    }
    return (config.window_end_unix_seconds - now) * kNsPerSecond;
}

std::int64_t latency_buffer_tick(const Config& config) {
    if (config.assumed_latency_ms <= 0.0 ||
        config.latency_buffer_tick_per_ms <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::llround(
            config.assumed_latency_ms *
            static_cast<double>(config.latency_buffer_tick_per_ms)
        )
    );
}

struct BtcOracleSnapshot {
    bool enabled = false;
    bool has_spot = false;
    bool stale = false;
    bool toxic_bid = false;
    bool toxic_ask = false;
    double spot = 0.0;
    double move_1s_bps = 0.0;
    double move_500ms_bps = 0.0;
    double realized_vol_annual_bps = 0.0;
    std::uint64_t realized_vol_sample_count = 0;
    std::uint64_t latest_age_ms = 0;
    std::uint64_t updates = 0;
    std::uint64_t parse_errors = 0;
    std::uint64_t transport_errors = 0;
};

class BtcOracleState {
public:
    void observe_trade(double spot) {
        if (spot <= 0.0 || !std::isfinite(spot)) {
            observe_parse_error();
            return;
        }
        const auto now = now_ns();
        std::lock_guard<std::mutex> lock(mutex_);
        latest_spot_ = spot;
        latest_ts_ns_ = now;
        ++updates_;
        samples_.push_back({now, spot});
        while (!samples_.empty() &&
               now > samples_.front().first + 3'600 * kNsPerSecond) {
            samples_.pop_front();
        }
    }

    void observe_parse_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++parse_errors_;
    }

    void observe_transport_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++transport_errors_;
    }

    [[nodiscard]] BtcOracleSnapshot snapshot(
        const Config& config
    ) const {
        BtcOracleSnapshot out;
        out.enabled = config.btc_oracle_enabled;
        std::lock_guard<std::mutex> lock(mutex_);
        out.updates = updates_;
        out.parse_errors = parse_errors_;
        out.transport_errors = transport_errors_;
        if (latest_spot_ <= 0.0 || latest_ts_ns_ == 0) {
            return out;
        }

        const auto now = now_ns();
        out.has_spot = true;
        out.spot = latest_spot_;
        out.latest_age_ms = now >= latest_ts_ns_
            ? (now - latest_ts_ns_) / 1'000'000ULL
            : 0;
        out.stale =
            out.latest_age_ms > config.btc_oracle_max_age_ms;

        double reference = 0.0;
        const auto cutoff = now > kNsPerSecond ? now - kNsPerSecond : 0;
        for (const auto& [ts_ns, price] : samples_) {
            if (ts_ns <= cutoff) {
                reference = price;
            } else {
                break;
            }
        }
        if (reference <= 0.0 && !samples_.empty()) {
            reference = samples_.front().second;
        }
        if (reference > 0.0) {
            out.move_1s_bps =
                (out.spot - reference) / reference * 10'000.0;
            if (std::fabs(out.move_1s_bps) >=
                config.btc_toxic_move_1s_bps) {
                out.toxic_bid = out.move_1s_bps < 0.0;
                out.toxic_ask = out.move_1s_bps > 0.0;
            }
        }

        reference = 0.0;
        const auto cutoff_500ms =
            now > kNsPerSecond / 2 ? now - kNsPerSecond / 2 : 0;
        for (const auto& [ts_ns, price] : samples_) {
            if (ts_ns <= cutoff_500ms) {
                reference = price;
            } else {
                break;
            }
        }
        if (reference <= 0.0 && !samples_.empty()) {
            reference = samples_.front().second;
        }
        if (reference > 0.0) {
            out.move_500ms_bps =
                (out.spot - reference) / reference * 10'000.0;
        }

        const auto vol_window_ns =
            config.btc_realized_vol_window_seconds * kNsPerSecond;
        const auto vol_cutoff = now > vol_window_ns ? now - vol_window_ns : 0;
        double sum_sq_log_return = 0.0;
        double elapsed_seconds = 0.0;
        bool have_prev = false;
        std::uint64_t prev_ts = 0;
        double prev_price = 0.0;
        for (const auto& [ts_ns, price] : samples_) {
            if (ts_ns < vol_cutoff || price <= 0.0) {
                continue;
            }
            if (have_prev && ts_ns > prev_ts && prev_price > 0.0) {
                const auto log_return = std::log(price / prev_price);
                if (std::isfinite(log_return)) {
                    sum_sq_log_return += log_return * log_return;
                    elapsed_seconds +=
                        static_cast<double>(ts_ns - prev_ts) /
                        static_cast<double>(kNsPerSecond);
                    ++out.realized_vol_sample_count;
                }
            }
            have_prev = true;
            prev_ts = ts_ns;
            prev_price = price;
        }
        if (out.realized_vol_sample_count > 1 && elapsed_seconds > 0.0) {
            const auto annual_variance =
                sum_sq_log_return * kSecondsPerYear / elapsed_seconds;
            if (annual_variance > 0.0 && std::isfinite(annual_variance)) {
                out.realized_vol_annual_bps = std::clamp(
                    std::sqrt(annual_variance) * 10'000.0,
                    config.btc_min_realized_vol_annual_bps,
                    config.btc_max_realized_vol_annual_bps
                );
            }
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::pair<std::uint64_t, double>> samples_;
    double latest_spot_ = 0.0;
    std::uint64_t latest_ts_ns_ = 0;
    std::uint64_t updates_ = 0;
    std::uint64_t parse_errors_ = 0;
    std::uint64_t transport_errors_ = 0;
};

struct ExternalFairRuntime {
    std::int64_t tick = 0;
    std::int64_t raw_tick = 0;
    std::int64_t basis_tick = 0;
    bool basis_applied = false;
    bool from_digital_option = false;
    double spot = 0.0;
    mm::DigitalOptionFairResult digital;
};

struct DynamicQuoteRuntime {
    std::int64_t half_spread_tick = 0;
    std::int64_t max_inventory_skew_tick = 0;
    std::int64_t reservation_risk_tick = 0;
    bool as_enabled = false;
    bool as_ok = false;
    bool toxic_bid = false;
    bool toxic_ask = false;
    double spread_multiplier = 1.0;
};

ExternalFairRuntime external_fair_runtime(
    const Config& config,
    std::uint64_t tte_ns,
    const BtcOracleSnapshot& oracle
) {
    ExternalFairRuntime out;
    if (config.external_fair_value_tick > 0) {
        auto tick = std::clamp<std::int64_t>(
            config.external_fair_value_tick,
            1,
            mm::kPriceOneTick - 1
        );
        if (config.external_fair_invert) {
            tick = mm::kPriceOneTick - tick;
        }
        out.tick = tick;
        out.raw_tick = tick;
        return out;
    }

    const auto spot = config.btc_oracle_enabled
        ? (!oracle.stale && oracle.has_spot ? oracle.spot : 0.0)
        : config.btc_spot;
    out.spot = spot;

    const auto vol_annual_bps =
        config.btc_use_realized_vol &&
            oracle.realized_vol_annual_bps > 0.0
        ? oracle.realized_vol_annual_bps
        : config.btc_vol_annual_bps;

    if (spot <= 0.0 || config.btc_threshold <= 0.0 ||
        vol_annual_bps <= 0.0) {
        return out;
    }

    out.digital = mm::DigitalOptionFairModel{}.compute(
        mm::DigitalOptionFairInput{
            .spot = spot,
            .strike = config.btc_threshold,
            .vol_annual_bps = vol_annual_bps,
            .drift_annual_bps = config.btc_drift_annual_bps,
            .time_to_expiry_ns = tte_ns,
            .invert = config.external_fair_invert
        }
    );
    if (!out.digital.ok) {
        return out;
    }
    out.from_digital_option = true;
    out.tick = out.digital.fair_value_tick;
    out.raw_tick = out.tick;
    return out;
}

DynamicQuoteRuntime dynamic_quote_runtime(
    const Config& config,
    const ExternalFairRuntime& fair,
    const BtcOracleSnapshot& oracle
) {
    DynamicQuoteRuntime out;
    out.toxic_bid = oracle.enabled && !oracle.stale && oracle.toxic_bid;
    out.toxic_ask = oracle.enabled && !oracle.stale && oracle.toxic_ask;
    out.as_enabled = config.enable_as_model;
    out.spread_multiplier =
        (out.toxic_bid || out.toxic_ask)
            ? std::max(1.0, config.as_toxic_spread_multiplier)
            : 1.0;
    if (!config.enable_as_model || !fair.digital.ok) {
        return out;
    }

    const auto as = mm::AvellanedaStoikovModel{}.compute(
        mm::AvellanedaStoikovInput{
            .fair = fair.digital,
            .risk_aversion = config.as_risk_aversion,
            .order_arrival_k = config.as_order_arrival_k,
            .spread_multiplier = out.spread_multiplier,
            .min_half_spread_tick = config.as_min_half_spread_tick,
            .max_half_spread_tick = config.as_max_half_spread_tick,
            .min_inventory_skew_tick = config.as_min_inventory_skew_tick,
            .max_inventory_skew_tick = config.as_max_inventory_skew_tick
        }
    );
    out.as_ok = as.ok;
    out.half_spread_tick = as.half_spread_tick;
    out.max_inventory_skew_tick = as.max_inventory_skew_tick;
    out.reservation_risk_tick = as.reservation_risk_tick;
    return out;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(
                    static_cast<unsigned char>(c) < 0x20 ? ' ' : c
                );
                break;
        }
    }
    return out;
}

std::int64_t midpoint_tick(const state::MarketDepthView& depth) {
    if (depth.bid_count == 0 || depth.ask_count == 0 ||
        depth.bids[0].price_tick <= 0 || depth.asks[0].price_tick <= 0) {
        return 0;
    }
    return (depth.bids[0].price_tick + depth.asks[0].price_tick) / 2;
}

bool locked_or_crossed_book(const state::MarketDepthView& depth) noexcept {
    return depth.crossed ||
           (depth.bid_count > 0 && depth.ask_count > 0 &&
            depth.bids[0].price_tick > 0 && depth.asks[0].price_tick > 0 &&
            depth.bids[0].price_tick >= depth.asks[0].price_tick);
}

bool momentum_bid_shutoff(
    const Config& config,
    const BtcOracleSnapshot& oracle
) noexcept {
    if (!config.momentum_bid_shutoff_enabled || !oracle.enabled ||
        oracle.stale || !oracle.has_spot) {
        return false;
    }
    const auto threshold_500ms =
        std::max(0.0, config.momentum_bid_shutoff_500ms_bps);
    const auto threshold_1s =
        std::max(0.0, config.momentum_bid_shutoff_1s_bps);
    return (threshold_500ms > 0.0 &&
            oracle.move_500ms_bps <= -threshold_500ms) ||
           (threshold_1s > 0.0 &&
            oracle.move_1s_bps <= -threshold_1s);
}

std::int64_t effective_locked_book_taker_min_edge_tick(
    const Config& config
) noexcept {
    return std::max<std::int64_t>(
        config.locked_book_taker_min_edge_tick,
        config.min_quote_edge_tick +
            config.adverse_selection_buffer_tick +
            latency_buffer_tick(config)
    );
}

std::int64_t complement_fair_tick(std::int64_t fair_tick) noexcept {
    if (fair_tick <= 0 || fair_tick >= mm::kPriceOneTick) {
        return 0;
    }
    return mm::kPriceOneTick - fair_tick;
}

struct LockedBookTakerOpportunity {
    bool active = false;
    bool blocked_by_mid_slippage = false;
    bool complement_asset = false;
    std::int64_t edge_tick = 0;
    std::int64_t price_tick = 0;
    std::int64_t qty_lots = 0;
    std::int64_t mid_tick = 0;
    std::int64_t mid_slippage_tick = 0;
};

struct LeadLagSnipingSignal {
    bool active = false;
    std::uint64_t side = 0;
    bool complement_asset = false;
    std::int64_t edge_tick = 0;
    std::int64_t price_tick = 0;
};

bool apply_taker_entry_mid_filter(
    const Config& config,
    const state::MarketDepthView& depth,
    LockedBookTakerOpportunity* opportunity
) noexcept {
    if (opportunity == nullptr || opportunity->price_tick <= 0) {
        return false;
    }
    const auto mid = midpoint_tick(depth);
    opportunity->mid_tick = mid;
    opportunity->mid_slippage_tick = opportunity->price_tick - mid;
    if (config.taker_max_entry_mid_slippage_tick < 0) {
        return true;
    }
    if (mid <= 0 ||
        opportunity->mid_slippage_tick >
            config.taker_max_entry_mid_slippage_tick) {
        opportunity->active = false;
        opportunity->qty_lots = 0;
        opportunity->blocked_by_mid_slippage = true;
        return false;
    }
    return true;
}

void record_taker_mid_slippage_block(
    const LockedBookTakerOpportunity& opportunity,
    std::uint64_t source,
    Stats* stats
) noexcept {
    if (stats == nullptr || !opportunity.blocked_by_mid_slippage) {
        return;
    }
    stats->taker_ioc_mid_slippage_blocked.fetch_add(1);
    stats->latest_taker_ioc_edge_tick.store(opportunity.edge_tick);
    stats->latest_taker_ioc_price_tick.store(opportunity.price_tick);
    stats->latest_taker_ioc_qty_lots.store(0);
    stats->latest_taker_ioc_mid_tick.store(opportunity.mid_tick);
    stats->latest_taker_ioc_mid_slippage_tick.store(
        opportunity.mid_slippage_tick
    );
    stats->latest_taker_ioc_asset_side.store(
        opportunity.complement_asset ? 2 : 1
    );
    stats->latest_taker_ioc_source.store(source);
}

LockedBookTakerOpportunity evaluate_locked_book_taker_buy(
    const Config& config,
    const state::MarketDepthView& depth,
    const ExternalFairRuntime& fair,
    const BtcOracleSnapshot& oracle,
    bool bid_momentum_shutoff,
    bool toxic_bid,
    std::int64_t current_position_lots
) noexcept {
    LockedBookTakerOpportunity out;
    if (!config.locked_book_taker_hunter_enabled ||
        !locked_or_crossed_book(depth) ||
        oracle.stale ||
        !oracle.has_spot ||
        bid_momentum_shutoff ||
        toxic_bid ||
        fair.tick <= 0 ||
        depth.ask_count == 0 ||
        depth.asks[0].price_tick <= 0) {
        return out;
    }

    const auto edge = fair.tick - depth.asks[0].price_tick;
    if (edge < effective_locked_book_taker_min_edge_tick(config)) {
        return out;
    }
    out.edge_tick = edge;
    out.price_tick = depth.asks[0].price_tick;
    if (!apply_taker_entry_mid_filter(config, depth, &out)) {
        return out;
    }

    const auto top_ask_lots =
        state::depth_prefix_level_size_lots(depth.asks[0]);
    const auto inventory_capacity =
        std::max<std::int64_t>(
            0,
            config.max_inventory_lots - current_position_lots
        );
    auto qty = std::min({
        config.locked_book_taker_size_lots,
        config.max_fill_qty_per_trade,
        top_ask_lots,
        inventory_capacity
    });
    if (qty <= 0) {
        out.active = true;
        return out;
    }

    out.active = true;
    out.qty_lots = qty;
    return out;
}

LockedBookTakerOpportunity evaluate_lead_lag_taker_buy(
    const Config& config,
    const state::MarketDepthView& depth,
    const LeadLagSnipingSignal& signal,
    std::int64_t current_position_lots
) noexcept {
    LockedBookTakerOpportunity out;
    out.complement_asset = signal.complement_asset;
    if (!config.macro_divergence_taker_enabled ||
        !signal.active ||
        (signal.side != 1 && signal.side != 2) ||
        signal.edge_tick < config.macro_shock_min_edge_tick ||
        depth.ask_count == 0 ||
        depth.asks[0].price_tick <= 0) {
        return out;
    }
    out.edge_tick = signal.edge_tick;
    out.price_tick = depth.asks[0].price_tick;
    if (!apply_taker_entry_mid_filter(config, depth, &out)) {
        return out;
    }

    const auto top_ask_lots =
        state::depth_prefix_level_size_lots(depth.asks[0]);
    const auto inventory_capacity =
        std::max<std::int64_t>(
            0,
            config.max_inventory_lots - current_position_lots
        );
    auto qty = std::min({
        config.macro_divergence_taker_size_lots,
        config.max_fill_qty_per_trade,
        top_ask_lots,
        inventory_capacity
    });

    out.active = true;
    out.qty_lots = std::max<std::int64_t>(0, qty);
    return out;
}

LockedBookTakerOpportunity evaluate_locked_book_taker_buy_for_asset(
    const Config& config,
    const state::MarketDepthView& depth,
    std::int64_t fair_tick,
    const BtcOracleSnapshot& oracle,
    bool bid_momentum_shutoff,
    bool toxic_bid,
    std::int64_t current_position_lots,
    bool complement_asset
) noexcept {
    ExternalFairRuntime fair;
    fair.tick = fair_tick;
    auto out = evaluate_locked_book_taker_buy(
        config,
        depth,
        fair,
        oracle,
        bid_momentum_shutoff,
        toxic_bid,
        current_position_lots
    );
    out.complement_asset = complement_asset;
    return out;
}

LockedBookTakerOpportunity better_taker_opportunity(
    LockedBookTakerOpportunity lhs,
    LockedBookTakerOpportunity rhs
) noexcept {
    const auto lhs_usable = lhs.active || lhs.blocked_by_mid_slippage;
    const auto rhs_usable = rhs.active || rhs.blocked_by_mid_slippage;
    if (!lhs_usable) {
        return rhs;
    }
    if (!rhs_usable) {
        return lhs;
    }
    return rhs.edge_tick > lhs.edge_tick ? rhs : lhs;
}

const char* lead_lag_side_name(std::uint64_t side) noexcept {
    switch (side) {
        case 1:
            return "buy_yes";
        case 2:
            return "buy_no";
        default:
            return "none";
    }
}

const char* taker_ioc_source_name(std::uint64_t source) noexcept {
    switch (source) {
        case 1:
            return "locked_book";
        case 2:
            return "lead_lag";
        case 3:
            return "macro_divergence";
        default:
            return "none";
    }
}

const char* taker_asset_side_name(std::uint64_t side) noexcept {
    switch (side) {
        case 1:
            return "yes";
        case 2:
            return "no";
        default:
            return "none";
    }
}

const char* reduce_exit_stage_name(std::uint64_t stage) noexcept {
    switch (static_cast<ReduceExitStage>(stage)) {
        case ReduceExitStage::Passive:
            return "passive";
        case ReduceExitStage::Urgent:
            return "urgent";
        case ReduceExitStage::Puke:
            return "puke";
        case ReduceExitStage::None:
            return "none";
    }
    return "none";
}

ExternalFairRuntime apply_existing_basis(
    const Config& config,
    const RuntimeState& runtime,
    ExternalFairRuntime fair
) {
    fair.raw_tick = fair.raw_tick > 0 ? fair.raw_tick : fair.tick;
    if (!config.fair_basis_smoothing_enabled || fair.raw_tick <= 0) {
        fair.basis_tick = 0;
        fair.basis_applied = false;
        return fair;
    }
    fair.basis_tick = runtime.fair_basis_tick;
    fair.tick = std::clamp<std::int64_t>(
        fair.raw_tick + fair.basis_tick,
        1,
        mm::kPriceOneTick - 1
    );
    fair.basis_applied = true;
    return fair;
}

ExternalFairRuntime update_basis_and_apply(
    const Config& config,
    RuntimeState* runtime,
    ExternalFairRuntime fair,
    const state::MarketDepthView& depth
) {
    if (!runtime) {
        return fair;
    }
    fair.raw_tick = fair.raw_tick > 0 ? fair.raw_tick : fair.tick;
    if (!config.fair_basis_smoothing_enabled || fair.raw_tick <= 0) {
        runtime->fair_basis_tick = 0;
        runtime->fair_basis_initialized = false;
        return fair;
    }
    if (!runtime->fair_basis_initialized) {
        runtime->fair_basis_tick = std::clamp(
            config.fair_basis_seed_tick,
            config.fair_basis_min_tick,
            config.fair_basis_max_tick
        );
        runtime->fair_basis_initialized = true;
    }

    const auto mid = midpoint_tick(depth);
    const auto update_interval_ns =
        config.fair_basis_update_interval_ms * 1'000'000ULL;
    const auto now = now_ns();
    const auto can_update =
        runtime->last_fair_basis_update_ns == 0 ||
        update_interval_ns == 0 ||
        now >= runtime->last_fair_basis_update_ns + update_interval_ns;
    if (mid > 0 && can_update) {
        const auto observed_basis = std::clamp<std::int64_t>(
            mid - fair.raw_tick,
            config.fair_basis_min_tick,
            config.fair_basis_max_tick
        );
        const auto alpha = std::clamp(config.fair_basis_ewma_alpha, 0.0, 1.0);
        const auto smoothed =
            static_cast<double>(runtime->fair_basis_tick) * (1.0 - alpha) +
            static_cast<double>(observed_basis) * alpha;
        runtime->fair_basis_tick = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::llround(smoothed)),
            config.fair_basis_min_tick,
            config.fair_basis_max_tick
        );
        runtime->last_fair_basis_update_ns = now;
        ++runtime->fair_basis_updates;
    }
    return apply_existing_basis(config, *runtime, fair);
}

void update_ewma_fair(
    const Config& config,
    RuntimeState* runtime,
    std::int64_t current_yes_fair_tick
) noexcept {
    if (!runtime || !config.macro_divergence_taker_enabled ||
        current_yes_fair_tick <= 0) {
        return;
    }
    if (!runtime->ewma_fair_initialized) {
        runtime->ewma_fair_yes_tick = current_yes_fair_tick;
        runtime->ewma_fair_initialized = true;
        return;
    }
    const auto alpha =
        std::clamp(config.macro_divergence_ewma_alpha, 0.0, 1.0);
    const auto smoothed =
        alpha * static_cast<double>(current_yes_fair_tick) +
        (1.0 - alpha) *
            static_cast<double>(runtime->ewma_fair_yes_tick);
    runtime->ewma_fair_yes_tick = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(smoothed)),
        1,
        mm::kPriceOneTick - 1
    );
}

void update_ewma_basis_yes(
    const Config& config,
    RuntimeState* runtime,
    const state::MarketDepthView& depth,
    std::int64_t external_fair_yes_tick
) noexcept {
    if (!runtime || !config.macro_divergence_taker_enabled ||
        external_fair_yes_tick <= 0) {
        return;
    }
    const auto best_bid =
        depth.bid_count > 0 ? depth.bids[0].price_tick : 0;
    const auto best_ask =
        depth.ask_count > 0 ? depth.asks[0].price_tick : 0;
    if (best_bid <= 0 || best_ask <= 0 || best_ask <= best_bid) {
        return;
    }
    const auto spread = best_ask - best_bid;
    if (config.max_allowed_spread_tick > 0 &&
        spread > config.max_allowed_spread_tick) {
        return;
    }
    const auto mid = (best_bid + best_ask) / 2;
    const auto instant_basis =
        static_cast<double>(mid) - static_cast<double>(external_fair_yes_tick);
    if (!runtime->basis_initialized) {
        runtime->ewma_basis_yes_tick = instant_basis;
        runtime->basis_initialized = true;
        return;
    }
    const auto alpha = std::clamp(config.basis_ewma_alpha, 0.0, 1.0);
    runtime->ewma_basis_yes_tick =
        alpha * instant_basis + (1.0 - alpha) * runtime->ewma_basis_yes_tick;
}

[[nodiscard]] bool macro_edge_crossed(
    std::int64_t edge_tick,
    std::int64_t min_edge_tick,
    std::int64_t* previous_edge_tick
) noexcept {
    if (previous_edge_tick == nullptr) {
        return false;
    }
    if (edge_tick < min_edge_tick) {
        *previous_edge_tick = edge_tick;
        return false;
    }
    const auto crossed = *previous_edge_tick < min_edge_tick;
    *previous_edge_tick = edge_tick;
    return crossed;
}

LeadLagSnipingSignal evaluate_lead_lag_sniping(
    const Config& config,
    const BtcOracleSnapshot& oracle,
    std::int64_t ewma_fair_yes_tick,
    const state::MarketDepthView& depth,
    const state::MarketDepthView* complement_depth,
    RuntimeState* runtime,
    Stats* stats
) {
    LeadLagSnipingSignal out;
    if (!config.macro_divergence_taker_enabled || runtime == nullptr ||
        oracle.stale || !oracle.has_spot || ewma_fair_yes_tick <= 0) {
        return out;
    }

    if (!runtime->basis_initialized) {
        if (stats != nullptr) {
            stats->macro_basis_uninitialized_blocked.fetch_add(1);
        }
        return out;
    }
    if (config.max_allowed_basis_tick > 0 &&
        std::llabs(runtime->ewma_basis_yes_tick) >
            static_cast<double>(config.max_allowed_basis_tick)) {
        if (stats != nullptr) {
            stats->macro_basis_insanity_blocked.fetch_add(1);
        }
        return out;
    }
    const auto basis_tick = static_cast<std::int64_t>(
        std::llround(runtime->ewma_basis_yes_tick)
    );
    const auto adjusted_fair_yes =
        std::clamp<std::int64_t>(
            ewma_fair_yes_tick + basis_tick,
            1,
            mm::kPriceOneTick - 1
        );
    const auto adjusted_fair_no =
        std::clamp<std::int64_t>(
            complement_fair_tick(ewma_fair_yes_tick) - basis_tick,
            1,
            mm::kPriceOneTick - 1
        );

    if (depth.ask_count > 0 && depth.asks[0].price_tick > 0) {
        const auto edge = adjusted_fair_yes - depth.asks[0].price_tick;
        if (macro_edge_crossed(
                edge,
                config.macro_shock_min_edge_tick,
                &runtime->macro_prev_yes_edge_tick)) {
            out.active = true;
            out.side = 1;
            out.edge_tick = edge;
            out.price_tick = depth.asks[0].price_tick;
        } else if (edge >= config.macro_shock_min_edge_tick &&
                   stats != nullptr) {
            stats->macro_edge_not_crossing_blocked.fetch_add(1);
        }
    }

    if (complement_depth != nullptr &&
        complement_depth->ask_count > 0 &&
        complement_depth->asks[0].price_tick > 0) {
        const auto edge =
            adjusted_fair_no - complement_depth->asks[0].price_tick;
        if (macro_edge_crossed(
                edge,
                config.macro_shock_min_edge_tick,
                &runtime->macro_prev_no_edge_tick) &&
            (!out.active || edge > out.edge_tick)) {
            out.active = true;
            out.side = 2;
            out.complement_asset = true;
            out.edge_tick = edge;
            out.price_tick = complement_depth->asks[0].price_tick;
        } else if (edge >= config.macro_shock_min_edge_tick &&
                   stats != nullptr) {
            stats->macro_edge_not_crossing_blocked.fetch_add(1);
        }
    }
    return out;
}

void prune_taker_fill_window(
    RuntimeState* runtime,
    std::uint64_t now
) noexcept {
    if (runtime == nullptr) {
        return;
    }
    constexpr auto window_ns = 60'000'000'000ULL;
    while (!runtime->taker_fill_timestamps_ns.empty() &&
           runtime->taker_fill_timestamps_ns.front() + window_ns <= now) {
        runtime->taker_fill_timestamps_ns.pop_front();
    }
}

void enforce_taker_fill_circuit_breaker(
    const Config& config,
    RuntimeState* runtime,
    Stats* stats,
    std::uint64_t now
) {
    if (config.max_taker_fills_per_minute == 0 || runtime == nullptr) {
        return;
    }
    prune_taker_fill_window(runtime, now);
    if (runtime->taker_fill_timestamps_ns.size() >=
        config.max_taker_fills_per_minute) {
        if (stats != nullptr) {
            stats->taker_circuit_breaker_tripped.fetch_add(1);
        }
        fail(
            "FATAL: taker fill circuit breaker tripped: " +
            std::to_string(runtime->taker_fill_timestamps_ns.size()) +
            " fills in the last 60s (limit=" +
            std::to_string(config.max_taker_fills_per_minute) + ")"
        );
    }
}

std::int64_t markout_tick(
    const RecentFill& fill,
    std::int64_t mark_tick
) {
    if (mark_tick <= 0 || fill.fill_price_tick <= 0) {
        return 0;
    }
    return fill.side == "sell" ? fill.fill_price_tick - mark_tick
                                : mark_tick - fill.fill_price_tick;
}

std::int64_t mark_tick_for_asset(
    const Config& config,
    const RuntimeState& runtime,
    const std::string& asset_id
) {
    if (asset_id == config.asset_id && runtime.has_depth) {
        return midpoint_tick(runtime.last_depth);
    }
    if (asset_id == config.complement_asset_id &&
        runtime.has_complement_depth) {
        return midpoint_tick(runtime.last_complement_depth);
    }
    return 0;
}

void update_recent_fill_markouts(
    const Config& config,
    RuntimeState* runtime,
    std::uint64_t now
) {
    for (auto& fill : runtime->recent_fills) {
        const auto mark_tick = mark_tick_for_asset(config, *runtime, fill.asset_id);
        if (mark_tick <= 0) {
            continue;
        }
        if (fill.mark_at_fill_tick == 0) {
            fill.mark_at_fill_tick = mark_tick;
        }
        if (!fill.markout_1s_ready && now >= fill.ts_ns + kNsPerSecond) {
            fill.markout_1s_tick = markout_tick(fill, mark_tick);
            fill.markout_1s_ready = true;
        }
        if (!fill.markout_5s_ready && now >= fill.ts_ns + 5 * kNsPerSecond) {
            fill.markout_5s_tick = markout_tick(fill, mark_tick);
            fill.markout_5s_ready = true;
        }
        if (!fill.markout_30s_ready && now >= fill.ts_ns + 30 * kNsPerSecond) {
            fill.markout_30s_tick = markout_tick(fill, mark_tick);
            fill.markout_30s_ready = true;
        }
    }
}

template <typename NameFn, typename CounterArray>
std::string histogram_json(
    const CounterArray& counters,
    std::size_t count,
    NameFn name
) {
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out << ',';
        }
        out << "\"" << name(i) << "\":" << counters[i].load();
    }
    out << "}";
    return out.str();
}

bool is_pong(const std::string& payload) {
    return payload == "PONG" || payload == "pong" ||
           payload == "\"PONG\"" || payload == "\"pong\"";
}

bool parse_binance_trade_price(
    const std::string& payload,
    double* out_price
) {
    if (!out_price) {
        return false;
    }
    boost::json::error_code error;
    const auto parsed = boost::json::parse(payload, error);
    if (error || !parsed.is_object()) {
        return false;
    }

    const boost::json::object* object = &parsed.as_object();
    if (const auto it = object->find("data");
        it != object->end() && it->value().is_object()) {
        object = &it->value().as_object();
    }

    const auto price_it = object->find("p");
    if (price_it == object->end()) {
        return false;
    }
    try {
        if (price_it->value().is_string()) {
            const auto& value = price_it->value().as_string();
            *out_price =
                std::stod(std::string(value.data(), value.size()));
            return *out_price > 0.0 && std::isfinite(*out_price);
        }
        if (price_it->value().is_double()) {
            *out_price = price_it->value().as_double();
            return *out_price > 0.0 && std::isfinite(*out_price);
        }
        if (price_it->value().is_int64()) {
            *out_price =
                static_cast<double>(price_it->value().as_int64());
            return *out_price > 0.0 && std::isfinite(*out_price);
        }
        if (price_it->value().is_uint64()) {
            *out_price =
                static_cast<double>(price_it->value().as_uint64());
            return *out_price > 0.0 && std::isfinite(*out_price);
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool parse_json_double(
    const boost::json::value& value,
    double* out_value
) {
    if (!out_value) {
        return false;
    }
    try {
        if (value.is_string()) {
            const auto& text = value.as_string();
            *out_value =
                std::stod(std::string(text.data(), text.size()));
            return *out_value > 0.0 && std::isfinite(*out_value);
        }
        if (value.is_double()) {
            *out_value = value.as_double();
            return *out_value > 0.0 && std::isfinite(*out_value);
        }
        if (value.is_int64()) {
            *out_value = static_cast<double>(value.as_int64());
            return *out_value > 0.0 && std::isfinite(*out_value);
        }
        if (value.is_uint64()) {
            *out_value = static_cast<double>(value.as_uint64());
            return *out_value > 0.0 && std::isfinite(*out_value);
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool parse_binance_book_ticker(
    const std::string& payload,
    double* out_bid,
    double* out_ask,
    std::int64_t* out_exchange_ts_ms
) {
    if (!out_bid || !out_ask) {
        return false;
    }
    boost::json::error_code error;
    const auto parsed = boost::json::parse(payload, error);
    if (error || !parsed.is_object()) {
        return false;
    }

    const boost::json::object* object = &parsed.as_object();
    if (const auto it = object->find("data");
        it != object->end() && it->value().is_object()) {
        object = &it->value().as_object();
    }

    const auto bid_it = object->find("b");
    const auto ask_it = object->find("a");
    if (bid_it == object->end() || ask_it == object->end()) {
        return false;
    }
    if (!parse_json_double(bid_it->value(), out_bid) ||
        !parse_json_double(ask_it->value(), out_ask)) {
        return false;
    }

    if (out_exchange_ts_ms != nullptr) {
        *out_exchange_ts_ms = 0;
        for (const char* key : {"E", "T"}) {
            const auto ts_it = object->find(key);
            if (ts_it == object->end()) {
                continue;
            }
            try {
                if (ts_it->value().is_int64()) {
                    *out_exchange_ts_ms = ts_it->value().as_int64();
                    break;
                }
                if (ts_it->value().is_uint64()) {
                    *out_exchange_ts_ms = static_cast<std::int64_t>(
                        ts_it->value().as_uint64()
                    );
                    break;
                }
                if (ts_it->value().is_string()) {
                    const auto& text = ts_it->value().as_string();
                    *out_exchange_ts_ms = std::stoll(
                        std::string(text.data(), text.size())
                    );
                    break;
                }
            } catch (...) {
                continue;
            }
        }
    }

    return *out_bid > 0.0 && *out_ask > 0.0 && *out_ask >= *out_bid;
}

bool sol_book_ticker_spread_ok(
    double bid,
    double ask,
    double max_spread_bps
) {
    if (bid <= 0.0 || ask <= 0.0 || ask < bid || max_spread_bps <= 0.0) {
        return false;
    }
    const double mid = 0.5 * (bid + ask);
    if (mid <= 0.0) {
        return false;
    }
    const double spread_bps = (ask - bid) / mid * 10'000.0;
    return spread_bps <= max_spread_bps;
}

bool targets_other_asset(
    const decode::NormalizedEvent& event,
    const Config& config
) {
    if (event.event_type == decode::NormalizedEventType::Heartbeat) {
        return false;
    }
    const std::string& target = !event.asset_id.empty()
        ? event.asset_id
        : event.entity_id;
    if (target.empty() || target == config.asset_id) {
        return false;
    }
    return config.complement_asset_id.empty() ||
           target != config.complement_asset_id;
}

std::string market_subscription(
    const std::string& asset_id,
    const std::string& complement_asset_id
) {
    std::string payload =
        std::string{R"({"assets_ids":[")"} + json_escape(asset_id) + R"(")";
    if (!complement_asset_id.empty()) {
        payload += std::string{R"(,")"} + json_escape(complement_asset_id) +
                   R"(")";
    }
    payload += R"(],"type":"market","custom_feature_enabled":true})";
    return payload;
}

const char* fill_mode_name(execution::PaperMakerFillMode mode) noexcept {
    switch (mode) {
        case execution::PaperMakerFillMode::NoFill:
            return "nofill";
        case execution::PaperMakerFillMode::Conservative:
            return "conservative";
        case execution::PaperMakerFillMode::BookCross:
            return "book-cross";
        case execution::PaperMakerFillMode::MidCross:
            return "mid-cross";
        case execution::PaperMakerFillMode::QueueAware:
            return "queue-aware";
    }
    return "unknown";
}

execution::PaperMakerFillMode parse_fill_mode(const std::string& value) {
    if (value == "nofill" || value == "no-fill") {
        return execution::PaperMakerFillMode::NoFill;
    }
    if (value == "conservative") {
        return execution::PaperMakerFillMode::Conservative;
    }
    if (value == "book-cross" || value == "bookcross") {
        return execution::PaperMakerFillMode::BookCross;
    }
    if (value == "mid-cross" || value == "midcross") {
        return execution::PaperMakerFillMode::MidCross;
    }
    if (value == "queue-aware" || value == "queueaware") {
        return execution::PaperMakerFillMode::QueueAware;
    }
    fail("unknown fill mode: " + value);
}

const char* mark_quality_name(paper::MarkQuality quality) noexcept {
    switch (quality) {
        case paper::MarkQuality::Good:
            return "Good";
        case paper::MarkQuality::MissingBid:
            return "MissingBid";
        case paper::MarkQuality::MissingAsk:
            return "MissingAsk";
        case paper::MarkQuality::MissingBook:
            return "MissingBook";
        case paper::MarkQuality::Degraded:
            return "Degraded";
        case paper::MarkQuality::NoPosition:
            return "NoPosition";
    }
    return "Unknown";
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* asset = std::getenv("POLYMARKET_ASSET_ID")) {
        config.asset_id = asset;
    }

    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto value = [&](const char* option) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + option);
            }
            return argv[++index];
        };

        if (arg == "--seconds" || arg == "--duration-seconds") {
            config.seconds = std::stoull(value(arg.c_str()));
        } else if (arg == "--asset-id") {
            config.asset_id = value("--asset-id");
        } else if (arg == "--complement-asset-id") {
            config.complement_asset_id = value("--complement-asset-id");
        } else if (arg == "--market-id") {
            config.market_id = value("--market-id");
        } else if (arg == "--endpoint") {
            config.endpoint = value("--endpoint");
        } else if (arg == "--dashboard-file") {
            config.dashboard_file = value("--dashboard-file");
        } else if (arg == "--out-json") {
            config.out_json = value("--out-json");
        } else if (arg == "--window-end-unix-seconds") {
            config.window_end_unix_seconds =
                std::stoull(value("--window-end-unix-seconds"));
        } else if (arg == "--starting-cash") {
            config.starting_cash_tick =
                static_cast<std::int64_t>(
                    std::stod(value("--starting-cash")) *
                    static_cast<double>(kTicksPerDollar)
                );
        } else if (arg == "--starting-cash-tick") {
            config.starting_cash_tick = std::stoll(value("--starting-cash-tick"));
        } else if (arg == "--half-spread-tick") {
            config.half_spread_tick = std::stoll(value("--half-spread-tick"));
        } else if (arg == "--quote-size-lots") {
            config.quote_size_lots = std::stoll(value("--quote-size-lots"));
        } else if (arg == "--inventory-skew-tick") {
            config.inventory_skew_tick =
                std::stoll(value("--inventory-skew-tick"));
        } else if (arg == "--inventory-skew-nonlinear-start-bps") {
            config.inventory_skew_nonlinear_start_bps =
                std::stoll(value("--inventory-skew-nonlinear-start-bps"));
        } else if (arg == "--inventory-skew-exponent") {
            config.inventory_skew_exponent =
                std::stod(value("--inventory-skew-exponent"));
        } else if (arg == "--target-position-lots") {
            config.target_position_lots =
                std::stoll(value("--target-position-lots"));
        } else if (arg == "--min-inventory-lots") {
            config.min_inventory_lots =
                std::stoll(value("--min-inventory-lots"));
        } else if (arg == "--max-inventory-lots") {
            config.max_inventory_lots =
                std::stoll(value("--max-inventory-lots"));
        } else if (arg == "--max-fill-qty-per-trade") {
            config.max_fill_qty_per_trade =
                std::stoll(value("--max-fill-qty-per-trade"));
        } else if (arg == "--initial-position-lots") {
            config.initial_position_lots =
                std::stoll(value("--initial-position-lots"));
        } else if (arg == "--initial-position-price-tick") {
            config.initial_position_price_tick =
                std::stoll(value("--initial-position-price-tick"));
        } else if (arg == "--initial-position-price") {
            config.initial_position_price_tick =
                static_cast<std::int64_t>(
                    std::stod(value("--initial-position-price")) *
                    static_cast<double>(kTicksPerDollar)
                );
        } else if (arg == "--initial-complement-position-lots") {
            config.initial_complement_position_lots =
                std::stoll(value("--initial-complement-position-lots"));
        } else if (arg == "--initial-complement-position-price-tick") {
            config.initial_complement_position_price_tick =
                std::stoll(value("--initial-complement-position-price-tick"));
        } else if (arg == "--initial-complement-position-price") {
            config.initial_complement_position_price_tick =
                static_cast<std::int64_t>(
                    std::stod(value("--initial-complement-position-price")) *
                    static_cast<double>(kTicksPerDollar)
                );
        } else if (arg == "--seed-complete-set") {
            config.seed_complete_set = true;
        } else if (arg == "--pure-taker-mode") {
            config.pure_taker_mode = true;
            config.enable_as_model = false;
        } else if (arg == "--min-quote-edge-tick") {
            config.min_quote_edge_tick =
                std::stoll(value("--min-quote-edge-tick"));
        } else if (arg == "--adverse-selection-buffer-tick") {
            config.adverse_selection_buffer_tick =
                std::stoll(value("--adverse-selection-buffer-tick"));
        } else if (arg == "--book-quarantine-ms") {
            config.book_quarantine_ms =
                std::stoull(value("--book-quarantine-ms"));
        } else if (arg == "--disable-fair-basis-smoothing") {
            config.fair_basis_smoothing_enabled = false;
        } else if (arg == "--fair-basis-seed-tick") {
            config.fair_basis_seed_tick =
                std::stoll(value("--fair-basis-seed-tick"));
        } else if (arg == "--fair-basis-ewma-alpha") {
            config.fair_basis_ewma_alpha =
                std::stod(value("--fair-basis-ewma-alpha"));
        } else if (arg == "--fair-basis-update-interval-ms") {
            config.fair_basis_update_interval_ms =
                std::stoull(value("--fair-basis-update-interval-ms"));
        } else if (arg == "--fair-basis-min-tick") {
            config.fair_basis_min_tick =
                std::stoll(value("--fair-basis-min-tick"));
        } else if (arg == "--fair-basis-max-tick") {
            config.fair_basis_max_tick =
                std::stoll(value("--fair-basis-max-tick"));
        } else if (arg == "--disable-lead-lag-sniping") {
            config.lead_lag_sniping_enabled = false;
        } else if (arg == "--enable-lead-lag-sniping") {
            config.lead_lag_sniping_enabled = true;
        } else if (arg == "--disable-macro-divergence-taker") {
            config.macro_divergence_taker_enabled = false;
        } else if (arg == "--enable-macro-divergence-taker") {
            config.macro_divergence_taker_enabled = true;
        } else if (arg == "--macro-divergence-ewma-alpha") {
            config.macro_divergence_ewma_alpha =
                std::stod(value("--macro-divergence-ewma-alpha"));
        } else if (arg == "--basis-ewma-alpha") {
            config.basis_ewma_alpha = std::stod(value("--basis-ewma-alpha"));
        } else if (arg == "--macro-shock-min-edge-tick") {
            config.macro_shock_min_edge_tick =
                std::stoll(value("--macro-shock-min-edge-tick"));
        } else if (arg == "--max-allowed-spread-tick") {
            config.max_allowed_spread_tick =
                std::stoll(value("--max-allowed-spread-tick"));
        } else if (arg == "--max-allowed-basis-tick") {
            config.max_allowed_basis_tick =
                std::stoll(value("--max-allowed-basis-tick"));
        } else if (arg == "--macro-divergence-cooldown-ms") {
            config.macro_divergence_cooldown_ms =
                std::stoull(value("--macro-divergence-cooldown-ms"));
        } else if (arg == "--macro-divergence-taker-size-lots") {
            config.macro_divergence_taker_size_lots =
                std::stoll(value("--macro-divergence-taker-size-lots"));
        } else if (arg == "--max-taker-fills-per-minute") {
            config.max_taker_fills_per_minute =
                std::stoull(value("--max-taker-fills-per-minute"));
        } else if (arg == "--lead-lag-min-move-500ms-bps") {
            config.lead_lag_min_move_500ms_bps =
                std::stod(value("--lead-lag-min-move-500ms-bps"));
        } else if (arg == "--lead-lag-min-stale-edge-tick") {
            config.lead_lag_min_stale_edge_tick =
                std::stoll(value("--lead-lag-min-stale-edge-tick"));
        } else if (arg == "--lead-lag-cooldown-ms") {
            config.lead_lag_cooldown_ms =
                std::stoull(value("--lead-lag-cooldown-ms"));
        } else if (arg == "--lead-lag-taker-size-lots") {
            config.lead_lag_taker_size_lots =
                std::stoll(value("--lead-lag-taker-size-lots"));
        } else if (arg == "--taker-max-entry-mid-slippage-tick") {
            config.taker_max_entry_mid_slippage_tick =
                std::stoll(value("--taker-max-entry-mid-slippage-tick"));
        } else if (arg == "--lead-lag-max-entry-mid-slippage-tick") {
            config.taker_max_entry_mid_slippage_tick =
                std::stoll(value("--lead-lag-max-entry-mid-slippage-tick"));
        } else if (arg == "--disable-locked-book-taker-hunter") {
            config.locked_book_taker_hunter_enabled = false;
        } else if (arg == "--enable-locked-book-taker-hunter") {
            config.locked_book_taker_hunter_enabled = true;
        } else if (arg == "--locked-book-taker-min-edge-tick") {
            config.locked_book_taker_min_edge_tick =
                std::stoll(value("--locked-book-taker-min-edge-tick"));
        } else if (arg == "--locked-book-taker-cooldown-ms") {
            config.locked_book_taker_cooldown_ms =
                std::stoull(value("--locked-book-taker-cooldown-ms"));
        } else if (arg == "--locked-book-taker-size-lots") {
            config.locked_book_taker_size_lots =
                std::stoll(value("--locked-book-taker-size-lots"));
        } else if (arg == "--disable-momentum-bid-shutoff") {
            config.momentum_bid_shutoff_enabled = false;
        } else if (arg == "--momentum-bid-shutoff-500ms-bps") {
            config.momentum_bid_shutoff_500ms_bps =
                std::stod(value("--momentum-bid-shutoff-500ms-bps"));
        } else if (arg == "--momentum-bid-shutoff-1s-bps") {
            config.momentum_bid_shutoff_1s_bps =
                std::stod(value("--momentum-bid-shutoff-1s-bps"));
        } else if (arg == "--assumed-latency-ms") {
            config.assumed_latency_ms =
                std::stod(value("--assumed-latency-ms"));
        } else if (arg == "--latency-buffer-tick-per-ms") {
            config.latency_buffer_tick_per_ms =
                std::stoll(value("--latency-buffer-tick-per-ms"));
        } else if (arg == "--min-top-depth-lots") {
            config.min_top_depth_lots =
                std::stoll(value("--min-top-depth-lots"));
        } else if (arg == "--min-total-depth-lots") {
            config.min_total_depth_lots =
                std::stoll(value("--min-total-depth-lots"));
        } else if (arg == "--min-book-spread-tick") {
            config.min_book_spread_tick =
                std::stoll(value("--min-book-spread-tick"));
        } else if (arg == "--max-book-spread-tick") {
            config.max_book_spread_tick =
                std::stoll(value("--max-book-spread-tick"));
        } else if (arg == "--max-book-spread-bps") {
            config.max_book_spread_bps =
                std::stoll(value("--max-book-spread-bps"));
        } else if (arg == "--max-quote-fair-deviation-tick") {
            config.max_quote_fair_deviation_tick =
                std::stoll(value("--max-quote-fair-deviation-tick"));
        } else if (arg == "--max-quote-fair-deviation-bps") {
            config.max_quote_fair_deviation_bps =
                std::stoll(value("--max-quote-fair-deviation-bps"));
        } else if (arg == "--min-fair-confidence-bps") {
            config.min_fair_confidence_bps =
                std::stoll(value("--min-fair-confidence-bps"));
        } else if (arg == "--complement-fair-weight-bps") {
            config.complement_fair_weight_bps =
                std::stoll(value("--complement-fair-weight-bps"));
        } else if (arg == "--external-fair-tick") {
            config.external_fair_value_tick =
                std::stoll(value("--external-fair-tick"));
        } else if (arg == "--external-fair") {
            config.external_fair_value_tick =
                static_cast<std::int64_t>(
                    std::stod(value("--external-fair")) *
                    static_cast<double>(kTicksPerDollar)
                );
        } else if (arg == "--external-fair-weight-bps") {
            config.external_fair_weight_bps =
                std::stoll(value("--external-fair-weight-bps"));
        } else if (arg == "--external-fair-invert") {
            config.external_fair_invert = true;
        } else if (arg == "--allow-book-fair-opening") {
            config.require_external_fair_for_opening_quotes = false;
        } else if (arg == "--require-external-fair") {
            config.require_external_fair_for_opening_quotes = true;
        } else if (arg == "--btc-spot") {
            config.btc_spot = std::stod(value("--btc-spot"));
        } else if (arg == "--btc-threshold") {
            config.btc_threshold = std::stod(value("--btc-threshold"));
        } else if (arg == "--btc-vol-annual-bps") {
            config.btc_vol_annual_bps =
                std::stod(value("--btc-vol-annual-bps"));
        } else if (arg == "--btc-drift-annual-bps") {
            config.btc_drift_annual_bps =
                std::stod(value("--btc-drift-annual-bps"));
        } else if (arg == "--enable-binance-btc-oracle") {
            config.btc_oracle_enabled = true;
        } else if (arg == "--btc-spot-ws") {
            config.btc_oracle_enabled = true;
            config.btc_oracle_endpoint = value("--btc-spot-ws");
        } else if (arg == "--btc-oracle-max-age-ms") {
            config.btc_oracle_max_age_ms =
                std::stoull(value("--btc-oracle-max-age-ms"));
        } else if (arg == "--btc-toxic-move-1s-bps") {
            config.btc_toxic_move_1s_bps =
                std::stod(value("--btc-toxic-move-1s-bps"));
        } else if (arg == "--use-btc-realized-vol") {
            config.btc_use_realized_vol = true;
        } else if (arg == "--btc-realized-vol-window-seconds") {
            config.btc_realized_vol_window_seconds =
                std::stoull(value("--btc-realized-vol-window-seconds"));
        } else if (arg == "--btc-min-realized-vol-annual-bps") {
            config.btc_min_realized_vol_annual_bps =
                std::stod(value("--btc-min-realized-vol-annual-bps"));
        } else if (arg == "--btc-max-realized-vol-annual-bps") {
            config.btc_max_realized_vol_annual_bps =
                std::stod(value("--btc-max-realized-vol-annual-bps"));
        } else if (arg == "--disable-sol-external-fair") {
            config.sol_external_fair_enabled = false;
        } else if (arg == "--external-fair-basis-log") {
            config.external_fair_basis_log = true;
            config.canonical_quote_research_log = true;
        } else if (arg == "--canonical-quote-research-log") {
            config.canonical_quote_research_log = true;
        } else if (arg == "--external-fair-shadow-only") {
            config.external_fair_shadow_only = true;
        } else if (arg == "--external-fair-tradable-lambda") {
            config.external_fair_tradable_lambda =
                std::stod(value("--external-fair-tradable-lambda"));
        } else if (arg == "--enable-dynamic-inventory-targeter") {
            config.dynamic_inventory_targeter_enabled = true;
        } else if (arg == "--disable-dynamic-inventory-targeter") {
            config.dynamic_inventory_targeter_enabled = false;
        } else if (arg == "--enable-portfolio-touch-risk") {
            config.portfolio_touch_risk_enabled = true;
        } else if (arg == "--disable-portfolio-touch-risk") {
            config.portfolio_touch_risk_enabled = false;
        } else if (arg == "--sol-fixed-vol-annualized") {
            config.sol_fixed_vol_annualized =
                std::stod(value("--sol-fixed-vol-annualized"));
        } else if (arg == "--sol-spot") {
            config.sol_spot_bid = std::stod(value("--sol-spot"));
            config.sol_spot_ask = config.sol_spot_bid;
        } else if (arg == "--sol-spot-bid") {
            config.sol_spot_bid = std::stod(value("--sol-spot-bid"));
        } else if (arg == "--sol-spot-ask") {
            config.sol_spot_ask = std::stod(value("--sol-spot-ask"));
        } else if (arg == "--sol-spot-feed") {
            config.sol_spot_feed = value("--sol-spot-feed");
        } else if (arg == "--sol-external-fair-outcome-side") {
            config.sol_external_fair_outcome_side =
                value("--sol-external-fair-outcome-side");
        } else if (arg == "--enable-as-model") {
            config.enable_as_model = true;
        } else if (arg == "--as-risk-aversion") {
            config.as_risk_aversion = std::stod(value("--as-risk-aversion"));
        } else if (arg == "--as-order-arrival-k") {
            config.as_order_arrival_k =
                std::stod(value("--as-order-arrival-k"));
        } else if (arg == "--as-toxic-spread-multiplier") {
            config.as_toxic_spread_multiplier =
                std::stod(value("--as-toxic-spread-multiplier"));
        } else if (arg == "--as-min-half-spread-tick") {
            config.as_min_half_spread_tick =
                std::stoll(value("--as-min-half-spread-tick"));
        } else if (arg == "--as-max-half-spread-tick") {
            config.as_max_half_spread_tick =
                std::stoll(value("--as-max-half-spread-tick"));
        } else if (arg == "--as-min-inventory-skew-tick") {
            config.as_min_inventory_skew_tick =
                std::stoll(value("--as-min-inventory-skew-tick"));
        } else if (arg == "--as-max-inventory-skew-tick") {
            config.as_max_inventory_skew_tick =
                std::stoll(value("--as-max-inventory-skew-tick"));
        } else if (arg == "--max-quote-size-multiplier-bps") {
            config.max_quote_size_multiplier_bps =
                std::stoll(value("--max-quote-size-multiplier-bps"));
        } else if (arg == "--passive-unwind-position-bps") {
            config.passive_unwind_position_bps =
                std::stoll(value("--passive-unwind-position-bps"));
        } else if (arg == "--forced-unwind-position-bps") {
            config.forced_unwind_position_bps =
                std::stoll(value("--forced-unwind-position-bps"));
        } else if (arg == "--passive-unwind-aggression-tick") {
            config.passive_unwind_aggression_tick =
                std::stoll(value("--passive-unwind-aggression-tick"));
        } else if (arg == "--passive-reduce-excess-lots") {
            config.passive_reduce_excess_lots =
                std::stoll(value("--passive-reduce-excess-lots"));
        } else if (arg == "--urgent-reduce-excess-lots") {
            config.urgent_reduce_excess_lots =
                std::stoll(value("--urgent-reduce-excess-lots"));
        } else if (arg == "--passive-reduce-join-tick") {
            config.passive_reduce_join_tick =
                std::stoll(value("--passive-reduce-join-tick"));
        } else if (arg == "--urgent-unwind-aggression-tick") {
            config.urgent_unwind_aggression_tick =
                std::stoll(value("--urgent-unwind-aggression-tick"));
        } else if (arg == "--urgent-reduce-age-ms") {
            config.urgent_reduce_age_ms =
                std::stoull(value("--urgent-reduce-age-ms"));
        } else if (arg == "--puke-reduce-age-ms") {
            config.puke_reduce_age_ms =
                std::stoull(value("--puke-reduce-age-ms"));
        } else if (arg == "--urgent-reduce-pressure-bps") {
            config.urgent_reduce_pressure_bps =
                std::stoll(value("--urgent-reduce-pressure-bps"));
        } else if (arg == "--puke-reduce-pressure-bps") {
            config.puke_reduce_pressure_bps =
                std::stoll(value("--puke-reduce-pressure-bps"));
        } else if (arg == "--reduce-only-to-target") {
            config.reduce_only_quote_to_target = true;
        } else if (arg == "--reduce-to-min-inventory") {
            config.reduce_only_quote_to_target = false;
        } else if (arg == "--tte-skew-start-seconds") {
            config.tte_skew_start_seconds =
                std::stoull(value("--tte-skew-start-seconds"));
        } else if (arg == "--tte-puke-start-seconds") {
            config.tte_puke_start_seconds =
                std::stoull(value("--tte-puke-start-seconds"));
        } else if (arg == "--tte-max-skew-multiplier") {
            config.tte_max_skew_multiplier =
                std::stod(value("--tte-max-skew-multiplier"));
        } else if (arg == "--queue-min-rest-ms") {
            config.queue_min_rest_ms = std::stoull(value("--queue-min-rest-ms"));
        } else if (arg == "--min-requote-interval-ms") {
            config.min_requote_interval_ms =
                std::stoull(value("--min-requote-interval-ms"));
        } else if (arg == "--min-quote-price-change-tick") {
            config.min_quote_price_change_tick =
                std::stoll(value("--min-quote-price-change-tick"));
        } else if (arg == "--fill-mode") {
            config.fill_mode = parse_fill_mode(value("--fill-mode"));
        } else if (arg == "--dashboard-interval-ms") {
            config.dashboard_interval_ms =
                std::stoull(value("--dashboard-interval-ms"));
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: run_market_maker_dashboard_live "
                << "[--seconds 1800|--duration-seconds 1800] "
                << "[--asset-id ASSET] "
                << "[--dashboard-file PATH] [--out-json PATH] "
                << "[--starting-cash 1000] [--fill-mode book-cross] "
                << "[--passive-reduce-excess-lots 20] "
                << "[--urgent-reduce-excess-lots 50] "
                << "[--urgent-reduce-age-ms 10000] "
                << "[--puke-reduce-age-ms 30000] "
                << "[--inventory-skew-exponent 2.0] "
                << "[--taker-max-entry-mid-slippage-tick 1000]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (config.seconds == 0) {
        fail("--seconds must be greater than zero");
    }
    if (config.asset_id.empty()) {
        fail("--asset-id is required unless POLYMARKET_ASSET_ID is set");
    }
    if (config.pure_taker_mode) {
        config.enable_as_model = false;
    }
    if (!config.complement_asset_id.empty() &&
        config.complement_asset_id == config.asset_id) {
        fail("--complement-asset-id must differ from --asset-id");
    }
    if ((config.seed_complete_set ||
         config.initial_complement_position_lots > 0) &&
        config.complement_asset_id.empty()) {
        fail("complete-set or complement seed requires --complement-asset-id");
    }
    if (config.seed_complete_set && config.initial_position_lots <= 0) {
        fail("--seed-complete-set requires --initial-position-lots > 0");
    }
    if (config.starting_cash_tick <= 0) {
        fail("--starting-cash must be greater than zero");
    }
    if (config.quote_size_lots <= 0) {
        fail("--quote-size-lots must be greater than zero");
    }
    if (config.max_inventory_lots <= 0) {
        fail("--max-inventory-lots must be greater than zero");
    }
    if (config.min_inventory_lots > config.target_position_lots ||
        config.target_position_lots > config.max_inventory_lots) {
        fail("inventory bounds must satisfy min <= target <= max");
    }
    if (config.inventory_skew_nonlinear_start_bps < 0 ||
        config.inventory_skew_nonlinear_start_bps > 10'000) {
        fail("--inventory-skew-nonlinear-start-bps must be in [0, 10000]");
    }
    if (config.inventory_skew_exponent < 1.0 ||
        config.inventory_skew_exponent > 20.0) {
        fail("--inventory-skew-exponent must be in [1, 20]");
    }
    if (config.initial_position_lots < 0) {
        fail("--initial-position-lots must be >= 0");
    }
    if (config.adverse_selection_buffer_tick < 0) {
        fail("--adverse-selection-buffer-tick must be >= 0");
    }
    if (config.fair_basis_ewma_alpha < 0.0 ||
        config.fair_basis_ewma_alpha > 1.0) {
        fail("--fair-basis-ewma-alpha must be in [0, 1]");
    }
    if (config.fair_basis_min_tick > config.fair_basis_max_tick) {
        fail("--fair-basis-min-tick must be <= --fair-basis-max-tick");
    }
    if (config.fair_basis_update_interval_ms > 60'000) {
        fail("--fair-basis-update-interval-ms must be <= 60000");
    }
    if (config.fair_basis_seed_tick < config.fair_basis_min_tick ||
        config.fair_basis_seed_tick > config.fair_basis_max_tick) {
        fail("--fair-basis-seed-tick must be within basis bounds");
    }
    if (config.lead_lag_min_move_500ms_bps < 0.0 ||
        config.lead_lag_min_move_500ms_bps > 10'000.0) {
        fail("--lead-lag-min-move-500ms-bps must be in [0, 10000]");
    }
    if (config.lead_lag_min_stale_edge_tick < 0) {
        fail("--lead-lag-min-stale-edge-tick must be >= 0");
    }
    if (config.lead_lag_cooldown_ms > 60'000) {
        fail("--lead-lag-cooldown-ms must be <= 60000");
    }
    if (config.lead_lag_taker_size_lots <= 0) {
        fail("--lead-lag-taker-size-lots must be > 0");
    }
    if (config.macro_divergence_ewma_alpha < 0.0 ||
        config.macro_divergence_ewma_alpha > 1.0) {
        fail("--macro-divergence-ewma-alpha must be in [0, 1]");
    }
    if (config.basis_ewma_alpha < 0.0 || config.basis_ewma_alpha > 1.0) {
        fail("--basis-ewma-alpha must be in [0, 1]");
    }
    if (config.macro_shock_min_edge_tick < 0) {
        fail("--macro-shock-min-edge-tick must be >= 0");
    }
    if (config.max_allowed_spread_tick < 0) {
        fail("--max-allowed-spread-tick must be >= 0");
    }
    if (config.max_allowed_basis_tick < 0) {
        fail("--max-allowed-basis-tick must be >= 0");
    }
    if (config.macro_divergence_cooldown_ms > 60'000) {
        fail("--macro-divergence-cooldown-ms must be <= 60000");
    }
    if (config.macro_divergence_taker_enabled &&
        config.macro_divergence_taker_size_lots <= 0) {
        fail("--macro-divergence-taker-size-lots must be > 0");
    }
    if (config.max_taker_fills_per_minute > 600) {
        fail("--max-taker-fills-per-minute must be <= 600");
    }
    if (config.taker_max_entry_mid_slippage_tick < -1) {
        fail("--taker-max-entry-mid-slippage-tick must be >= -1");
    }
    if (config.locked_book_taker_min_edge_tick < 0) {
        fail("--locked-book-taker-min-edge-tick must be >= 0");
    }
    if (config.locked_book_taker_cooldown_ms > 60'000) {
        fail("--locked-book-taker-cooldown-ms must be <= 60000");
    }
    if (config.locked_book_taker_size_lots <= 0) {
        fail("--locked-book-taker-size-lots must be > 0");
    }
    if (config.tte_puke_start_seconds > config.tte_skew_start_seconds) {
        fail("--tte-puke-start-seconds must be <= --tte-skew-start-seconds");
    }
    if (config.tte_max_skew_multiplier < 1.0) {
        fail("--tte-max-skew-multiplier must be >= 1.0");
    }
    if (config.complement_fair_weight_bps < 0 ||
        config.complement_fair_weight_bps > 10'000) {
        fail("--complement-fair-weight-bps must be in [0, 10000]");
    }
    if (config.external_fair_weight_bps < 0 ||
        config.external_fair_weight_bps > 10'000) {
        fail("--external-fair-weight-bps must be in [0, 10000]");
    }
    if (config.external_fair_tradable_lambda < 0.0 ||
        config.external_fair_tradable_lambda > 1.0) {
        fail("--external-fair-tradable-lambda must be in [0, 1]");
    }
    if (config.external_fair_value_tick < 0 ||
        config.external_fair_value_tick >= mm::kPriceOneTick) {
        fail("--external-fair-tick must be in [0, 999999]");
    }
    if (config.sol_fixed_vol_annualized <= 0.0 ||
        config.sol_fixed_vol_annualized > 10.0) {
        fail("--sol-fixed-vol-annualized must be in (0, 10]");
    }
    if (config.sol_spot_feed != "manual" &&
        config.sol_spot_feed != "binance_book_ticker") {
        fail("--sol-spot-feed must be manual or binance_book_ticker");
    }
    if ((config.sol_spot_bid > 0.0 || config.sol_spot_ask > 0.0) &&
        (config.sol_spot_bid <= 0.0 ||
         config.sol_spot_ask <= 0.0 ||
         config.sol_spot_ask < config.sol_spot_bid)) {
        fail("SOL spot bid/ask must be positive with ask >= bid");
    }
    if (config.sol_spot_feed == "binance_book_ticker" &&
        config.sol_spot_feed_endpoint.empty()) {
        fail("SOL binance book ticker endpoint must not be empty");
    }
    if (config.sol_spot_max_spread_bps <= 0.0) {
        fail("SOL spot max spread bps must be positive");
    }
    if (config.sol_external_fair_outcome_side != "yes" &&
        config.sol_external_fair_outcome_side != "Yes" &&
        config.sol_external_fair_outcome_side != "YES" &&
        config.sol_external_fair_outcome_side != "no" &&
        config.sol_external_fair_outcome_side != "No" &&
        config.sol_external_fair_outcome_side != "NO") {
        fail("--sol-external-fair-outcome-side must be yes or no");
    }
    if ((config.btc_spot > 0.0 || config.btc_threshold > 0.0 ||
         config.btc_vol_annual_bps > 0.0 ||
         config.btc_oracle_enabled) &&
        config.window_end_unix_seconds == 0) {
        fail("BTC external fair requires --window-end-unix-seconds");
    }
    if ((config.btc_spot > 0.0 || config.btc_oracle_enabled) &&
        (config.btc_threshold <= 0.0 ||
         config.btc_vol_annual_bps <= 0.0)) {
        fail("BTC external fair requires --btc-threshold and --btc-vol-annual-bps");
    }
    if (config.btc_oracle_enabled && config.btc_oracle_endpoint.empty()) {
        fail("--btc-spot-ws endpoint must not be empty");
    }
    if (config.btc_oracle_max_age_ms == 0 ||
        config.btc_oracle_max_age_ms > 60'000) {
        fail("--btc-oracle-max-age-ms must be in [1, 60000]");
    }
    if (config.btc_toxic_move_1s_bps < 0.0 ||
        config.btc_toxic_move_1s_bps > 10'000.0) {
        fail("--btc-toxic-move-1s-bps must be in [0, 10000]");
    }
    if (config.btc_realized_vol_window_seconds == 0 ||
        config.btc_realized_vol_window_seconds > 3'600) {
        fail("--btc-realized-vol-window-seconds must be in [1, 3600]");
    }
    if (config.btc_min_realized_vol_annual_bps < 0.0 ||
        config.btc_max_realized_vol_annual_bps <= 0.0 ||
        config.btc_min_realized_vol_annual_bps >
            config.btc_max_realized_vol_annual_bps) {
        fail("realized vol bounds are invalid");
    }
    if (config.enable_as_model) {
        if (config.as_risk_aversion <= 0.0 ||
            config.as_order_arrival_k < 0.0) {
            fail("AS model requires positive risk aversion and nonnegative k");
        }
        if (config.as_toxic_spread_multiplier < 1.0) {
            fail("--as-toxic-spread-multiplier must be >= 1");
        }
        if (config.as_min_half_spread_tick < 0 ||
            config.as_max_half_spread_tick < 0 ||
            (config.as_max_half_spread_tick > 0 &&
             config.as_min_half_spread_tick >
                 config.as_max_half_spread_tick)) {
            fail("AS half-spread bounds are invalid");
        }
        if (config.as_min_inventory_skew_tick < 0 ||
            config.as_max_inventory_skew_tick < 0 ||
            (config.as_max_inventory_skew_tick > 0 &&
             config.as_min_inventory_skew_tick >
                 config.as_max_inventory_skew_tick)) {
            fail("AS inventory-skew bounds are invalid");
        }
    }
    if (config.queue_min_rest_ms > 60'000) {
        fail("--queue-min-rest-ms must be <= 60000");
    }
    if (config.min_requote_interval_ms > 60'000) {
        fail("--min-requote-interval-ms must be <= 60000");
    }
    if (config.min_quote_price_change_tick < 0) {
        fail("--min-quote-price-change-tick must be >= 0");
    }
    if (config.min_book_spread_tick < 0) {
        fail("--min-book-spread-tick must be >= 0");
    }
    if (config.passive_reduce_excess_lots < 0) {
        fail("--passive-reduce-excess-lots must be >= 0");
    }
    if (config.urgent_reduce_excess_lots < 0) {
        fail("--urgent-reduce-excess-lots must be >= 0");
    }
    if (config.passive_reduce_join_tick < 0) {
        fail("--passive-reduce-join-tick must be >= 0");
    }
    if (config.urgent_unwind_aggression_tick < 0) {
        fail("--urgent-unwind-aggression-tick must be >= 0");
    }
    if (config.puke_reduce_age_ms < config.urgent_reduce_age_ms) {
        fail("--puke-reduce-age-ms must be >= --urgent-reduce-age-ms");
    }
    if (config.urgent_reduce_pressure_bps < 0 ||
        config.urgent_reduce_pressure_bps > 10'000) {
        fail("--urgent-reduce-pressure-bps must be in [0, 10000]");
    }
    if (config.puke_reduce_pressure_bps < 0 ||
        config.puke_reduce_pressure_bps > 10'000 ||
        config.puke_reduce_pressure_bps < config.urgent_reduce_pressure_bps) {
        fail("--puke-reduce-pressure-bps must be in [urgent, 10000]");
    }
    if (config.assumed_latency_ms < 0.0 ||
        config.assumed_latency_ms > 10'000.0) {
        fail("--assumed-latency-ms must be in [0, 10000]");
    }
    if (config.latency_buffer_tick_per_ms < 0) {
        fail("--latency-buffer-tick-per-ms must be >= 0");
    }
    return config;
}

mm::MarketMakingConfig market_making_config(const Config& input) {
    mm::MarketMakingConfig config;
    config.strategy_id = 101;
    config.oracle_artifact_hash = 11;
    config.policy_hash = 22;
    config.min_half_spread_tick = input.half_spread_tick;
    config.max_inventory_skew_tick = input.inventory_skew_tick;
    config.inventory_skew_nonlinear_start_bps =
        input.inventory_skew_nonlinear_start_bps;
    config.inventory_skew_exponent = input.inventory_skew_exponent;
    config.target_position_lots = input.target_position_lots;
    config.min_inventory_lots = input.min_inventory_lots;
    config.tte_skew_start_ns =
        input.tte_skew_start_seconds * kNsPerSecond;
    config.tte_puke_start_ns =
        input.tte_puke_start_seconds * kNsPerSecond;
    config.tte_max_skew_multiplier = input.tte_max_skew_multiplier;
    config.base_quote_size_lots = input.quote_size_lots;
    config.max_inventory_lots = input.max_inventory_lots;
    config.min_quote_edge_tick = input.min_quote_edge_tick;
    config.latency_buffer_tick = latency_buffer_tick(input);
    config.adverse_selection_buffer_tick =
        input.adverse_selection_buffer_tick;
    config.min_top_depth_lots = input.min_top_depth_lots;
    config.min_total_depth_lots = input.min_total_depth_lots;
    config.max_book_spread_tick = input.max_book_spread_tick;
    config.max_book_spread_bps = input.max_book_spread_bps;
    config.min_fair_confidence_bps = input.min_fair_confidence_bps;
    config.complement_fair_weight_bps =
        input.complement_asset_id.empty()
            ? 0
            : input.complement_fair_weight_bps;
    config.external_fair_value_tick = input.external_fair_value_tick;
    config.external_fair_weight_bps = input.external_fair_weight_bps;
    config.require_external_fair_for_opening_quotes =
        input.require_external_fair_for_opening_quotes;
    config.max_quote_size_multiplier_bps =
        input.max_quote_size_multiplier_bps;
    config.passive_unwind_position_bps = input.passive_unwind_position_bps;
    config.forced_unwind_position_bps = input.forced_unwind_position_bps;
    config.passive_unwind_aggression_tick =
        input.passive_unwind_aggression_tick;
    config.passive_reduce_excess_lots = input.passive_reduce_excess_lots;
    config.urgent_reduce_excess_lots = input.urgent_reduce_excess_lots;
    config.passive_reduce_join_tick = input.passive_reduce_join_tick;
    config.urgent_unwind_aggression_tick =
        input.urgent_unwind_aggression_tick;
    config.reduce_only_quote_to_target = input.reduce_only_quote_to_target;
    config.quote_ttl_ns = 5'000'000'000ULL;
    config.requote_threshold_tick = 1'000;
    config.min_quote_price_change_tick = input.min_quote_price_change_tick;
    config.min_requote_interval_ns =
        input.min_requote_interval_ms * 1'000'000ULL;
    if (input.pure_taker_mode) {
        config.max_inventory_skew_tick = 0;
        config.inventory_skew_nonlinear_start_bps = 10'000;
        config.inventory_skew_exponent = 1.0;
        config.passive_unwind_position_bps = 0;
        config.forced_unwind_position_bps = 0;
        config.passive_unwind_aggression_tick = 0;
        config.passive_reduce_excess_lots = 0;
        config.urgent_reduce_excess_lots = 0;
        config.urgent_unwind_aggression_tick = 0;
        config.passive_reduce_join_tick =
            std::max<std::int64_t>(1, input.passive_reduce_join_tick);
    }
    return config;
}

risk::QuoteRiskPolicy quote_risk_policy(const Config& input) {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_qty_lots = input.quote_size_lots;
    policy.max_quote_notional_tick =
        std::max<std::int64_t>(
            1,
            input.quote_size_lots * mm::kPriceOneTick * 2
        );
    policy.max_asset_inventory_lots = input.max_inventory_lots;
    policy.min_edge_to_fair_tick = -50'000;
    policy.min_book_spread_tick = input.min_book_spread_tick;
    policy.max_book_spread_tick = input.max_book_spread_tick;
    policy.max_book_spread_bps = input.max_book_spread_bps;
    policy.max_quote_fair_deviation_tick =
        input.max_quote_fair_deviation_tick;
    policy.max_quote_fair_deviation_bps = input.max_quote_fair_deviation_bps;
    policy.max_canonical_yes_exposure_lots = input.max_inventory_lots;
    policy.max_portfolio_touch_exposure_lots =
        input.portfolio_touch_risk_enabled ? input.max_inventory_lots * 2 : 0;
    policy.max_spot_age_ms = 1'500;
    policy.max_vol_age_ms = 60'000;
    policy.min_external_confidence_bps = input.sol_external_fair_enabled
        ? 1
        : 0;
    policy.max_book_age_ns = 1'000'000'000ULL;
    policy.min_replace_interval_ns =
        input.min_requote_interval_ms * 1'000'000ULL;
    policy.max_active_quotes_per_asset = 2;
    return policy;
}

LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();
    auto percentile = [&](double p) -> std::uint64_t {
        const auto index = static_cast<std::size_t>(
            p * static_cast<double>(values.size() - 1) + 0.5
        );
        return values[std::min(index, values.size() - 1)];
    };
    stats.p50 = percentile(0.50);
    stats.p90 = percentile(0.90);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    long double total = 0.0;
    for (const auto value : values) {
        total += static_cast<long double>(value);
    }
    stats.mean = static_cast<double>(total / values.size());
    return stats;
}

void write_atomic(const std::filesystem::path& path, const std::string& body) {
    if (path.empty()) {
        return;
    }
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("failed to open temporary output file: " + tmp);
        }
        out << body;
        if (!out) {
            fail("failed to write output file: " + tmp);
        }
    }
    std::filesystem::rename(tmp, path);
}

std::int64_t best_bid_tick(const RuntimeState& runtime) {
    return runtime.has_depth && runtime.last_depth.bid_count > 0
        ? runtime.last_depth.bids[0].price_tick
        : 0;
}

std::int64_t best_ask_tick(const RuntimeState& runtime) {
    return runtime.has_depth && runtime.last_depth.ask_count > 0
        ? runtime.last_depth.asks[0].price_tick
        : 0;
}

std::int64_t spread_tick(const RuntimeState& runtime) {
    const auto bid = best_bid_tick(runtime);
    const auto ask = best_ask_tick(runtime);
    return bid > 0 && ask > 0 ? ask - bid : 0;
}

double safe_ratio(std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

void observe_no_quote_reason(Stats* stats, mm::NoQuoteReason reason) {
    const auto index = static_cast<std::size_t>(reason);
    if (index < stats->no_quote_reasons.size()) {
        stats->no_quote_reasons[index].fetch_add(1);
    }
}

void observe_risk_decision(
    Stats* stats,
    risk::QuoteRiskDecisionType decision
) {
    const auto index = static_cast<std::size_t>(decision);
    if (index < stats->risk_decisions.size()) {
        stats->risk_decisions[index].fetch_add(1);
    }
}

void update_drawdown(RuntimeState* runtime) {
    const auto equity = runtime->pnl.equity_mid_tick;
    runtime->high_watermark_tick =
        std::max(runtime->high_watermark_tick, equity);
    const auto drawdown = equity - runtime->high_watermark_tick;
    runtime->max_drawdown_tick =
        std::min(runtime->max_drawdown_tick, drawdown);
}

std::int64_t attributed_unrealized_tick(
    std::int64_t position_lots,
    std::int64_t cost_basis_tick,
    std::int64_t mark_tick
) {
    if (position_lots <= 0 || mark_tick <= 0) {
        return 0;
    }
    return position_lots * mark_tick - cost_basis_tick;
}

void append_recent_fill(
    RuntimeState* runtime,
    const execution::MakerExecutionReport& report,
    std::int64_t mark_at_fill_tick,
    std::int64_t expected_edge_tick = 0
) {
    RecentFill fill;
    fill.report_id = report.report_id;
    fill.quote_id = report.quote_id;
    fill.quote_group_id = report.quote_group_id;
    fill.asset_id = report.asset_id;
    fill.side = report.side == execution::QuoteSide::Ask ? "sell" : "buy";
    fill.qty_lots = report.filled_qty_lots;
    fill.fill_price_tick = report.avg_fill_price_tick;
    fill.remaining_qty_lots = report.remaining_qty_lots;
    fill.reason = report.reason;
    fill.liquidity_role =
        report.liquidity_role == execution::FillLiquidityRole::Taker
            ? "taker"
            : "maker";
    fill.expected_edge_tick = expected_edge_tick;
    fill.ts_ns = report.recv_ts_ns != 0 ? report.recv_ts_ns
                                        : report.exchange_ts_ns;
    fill.mark_at_fill_tick = mark_at_fill_tick;
    runtime->recent_fills.push_back(std::move(fill));
    if (runtime->recent_fills.size() > 50) {
        runtime->recent_fills.erase(
            runtime->recent_fills.begin(),
            runtime->recent_fills.begin() +
                static_cast<std::ptrdiff_t>(runtime->recent_fills.size() - 50)
        );
    }
}

void update_pnl_snapshot_unlocked(
    const Config& config,
    RuntimeState* runtime,
    const state::MarketDepthView& depth,
    std::uint64_t now
) {
    std::array<state::MarketDepthView, 2> pnl_depths{depth, {}};
    std::span<const state::MarketDepthView> pnl_span{pnl_depths.data(), 1};
    if (runtime->has_complement_depth) {
        pnl_depths[1] = runtime->last_complement_depth;
        pnl_span = std::span<const state::MarketDepthView>{
            pnl_depths.data(),
            2
        };
    }
    runtime->pnl = runtime->pnl_engine.compute(runtime->ledger, pnl_span, now);
    update_drawdown(runtime);
}

bool apply_taker_ioc_buy(
    const Config& config,
    const state::MarketDepthView& depth,
    const LockedBookTakerOpportunity& opportunity,
    std::uint64_t source,
    const char* reason,
    Stats* stats,
    RuntimeState* runtime,
    std::uint64_t now
) {
    if (!opportunity.active) {
        return false;
    }

    stats->taker_ioc_signals.fetch_add(1);
    stats->latest_taker_ioc_edge_tick.store(opportunity.edge_tick);
    stats->latest_taker_ioc_price_tick.store(opportunity.price_tick);
    stats->latest_taker_ioc_qty_lots.store(opportunity.qty_lots);
    stats->latest_taker_ioc_mid_tick.store(opportunity.mid_tick);
    stats->latest_taker_ioc_mid_slippage_tick.store(
        opportunity.mid_slippage_tick
    );
    stats->latest_taker_ioc_asset_side.store(
        opportunity.complement_asset ? 2 : 1
    );
    stats->latest_taker_ioc_source.store(source);

    if (opportunity.qty_lots <= 0) {
        stats->taker_ioc_inventory_blocked.fetch_add(1);
        return false;
    }

    enforce_taker_fill_circuit_breaker(config, runtime, stats, now);

    const auto cooldown_ms =
        source == 3 ? config.macro_divergence_cooldown_ms
        : source == 2 ? config.lead_lag_cooldown_ms
                      : config.locked_book_taker_cooldown_ms;
    if (runtime->last_locked_book_taker_ns != 0 &&
        now < runtime->last_locked_book_taker_ns +
                  cooldown_ms * 1'000'000ULL) {
        stats->taker_ioc_cooldown_blocked.fetch_add(1);
        return false;
    }

    const auto notional =
        opportunity.qty_lots * opportunity.price_tick;
    if (notional > runtime->ledger.cash_ledger().cash_tick) {
        stats->taker_ioc_cash_blocked.fetch_add(1);
        return false;
    }

    paper::PaperFill fill;
    fill.report_id =
        0x710c000000000000ULL ^ runtime->next_taker_ioc_report_id++;
    fill.plan_id = 0;
    fill.order_id = 0;
    fill.quote_id = 0;
    fill.approved_quote_id = 0;
    fill.quote_group_id = 0;
    fill.asset_index = depth.asset_index;
    fill.asset_id = opportunity.complement_asset
        ? config.complement_asset_id
        : config.asset_id;
    fill.side = paper::Side::Buy;
    fill.liquidity_role = execution::FillLiquidityRole::Taker;
    fill.qty_lots = opportunity.qty_lots;
    fill.fill_price_tick = opportunity.price_tick;
    fill.fee_tick = 0;
    fill.ts_ns = now;
    fill.idempotency_hash = paper::compute_paper_fill_hash(fill);
    fill.fill_id = fill.idempotency_hash;

    const auto applied = runtime->ledger.apply_fill(fill);
    if (!applied.applied) {
        stats->taker_ioc_fills_rejected.fetch_add(1);
        return false;
    }

    runtime->last_locked_book_taker_ns = now;
    runtime->taker_fill_timestamps_ns.push_back(now);
    stats->taker_ioc_fills_applied.fetch_add(1);
    if (opportunity.complement_asset) {
        stats->taker_ioc_no_fills_applied.fetch_add(1);
    } else {
        stats->taker_ioc_yes_fills_applied.fetch_add(1);
    }
    if (source == 2) {
        stats->lead_lag_taker_ioc_fills_applied.fetch_add(1);
    }
    if (source == 3) {
        stats->macro_divergence_taker_ioc_fills_applied.fetch_add(1);
    }
    stats->gross_fill_notional_tick.fetch_add(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, notional))
    );
    stats->taker_ioc_notional_tick.fetch_add(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, notional))
    );
    stats->taker_ioc_expected_edge_tick.fetch_add(
        opportunity.edge_tick * opportunity.qty_lots
    );

    const auto mark_at_fill = midpoint_tick(depth);
    runtime->attribute_fill(fill, mark_at_fill);

    execution::MakerExecutionReport report;
    report.report_id = fill.report_id;
    report.asset_index = fill.asset_index;
    report.asset_id = fill.asset_id;
    report.side = execution::QuoteSide::Bid;
    report.status = execution::MakerQuoteStatus::Filled;
    report.liquidity_role = execution::FillLiquidityRole::Taker;
    report.filled_qty_lots = fill.qty_lots;
    report.avg_fill_price_tick = fill.fill_price_tick;
    report.remaining_qty_lots = 0;
    report.exchange_ts_ns = now;
    report.recv_ts_ns = now;
    report.reason = reason ? reason : "taker_ioc_buy";
    append_recent_fill(
        runtime,
        report,
        mark_at_fill,
        opportunity.edge_tick
    );
    return true;
}

std::int64_t reduce_target_lots(const Config& config) noexcept {
    return config.reduce_only_quote_to_target
        ? config.target_position_lots
        : config.min_inventory_lots;
}

std::uint64_t reduce_quote_group_id(
    const Config& config,
    std::uint32_t asset_index,
    bool complement_asset
) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = mm::fnv1a_mix(hash, 0x7265647563655f71ULL);
    hash = mm::fnv1a_mix(hash, config.enable_as_model ? 1ULL : 0ULL);
    hash = mm::fnv1a_mix(hash, asset_index);
    hash = mm::fnv1a_mix(hash, complement_asset ? 1ULL : 0ULL);
    return hash;
}

std::int64_t reduce_pressure_bps(
    const Config& config,
    std::int64_t excess_lots
) noexcept {
    const auto target = reduce_target_lots(config);
    const auto capacity =
        std::max<std::int64_t>(1, config.max_inventory_lots - target);
    return std::clamp<std::int64_t>(
        excess_lots * 10'000 / capacity,
        0,
        10'000
    );
}

ReduceExitStage reduce_exit_stage(
    const Config& config,
    std::int64_t excess_lots,
    std::uint64_t age_ms,
    std::int64_t pressure_bps
) noexcept {
    if (excess_lots <= 0) {
        return ReduceExitStage::None;
    }
    if (config.passive_reduce_excess_lots > 0 &&
        excess_lots < config.passive_reduce_excess_lots) {
        return ReduceExitStage::None;
    }
    if ((config.puke_reduce_age_ms > 0 &&
         age_ms >= config.puke_reduce_age_ms) ||
        (config.puke_reduce_pressure_bps > 0 &&
         pressure_bps >= config.puke_reduce_pressure_bps)) {
        return ReduceExitStage::Puke;
    }
    if ((config.urgent_reduce_excess_lots > 0 &&
         excess_lots >= config.urgent_reduce_excess_lots) ||
        (config.urgent_reduce_age_ms > 0 &&
         age_ms >= config.urgent_reduce_age_ms) ||
        (config.urgent_reduce_pressure_bps > 0 &&
         pressure_bps >= config.urgent_reduce_pressure_bps)) {
        return ReduceExitStage::Urgent;
    }
    return ReduceExitStage::Passive;
}

std::int64_t reduce_exit_price_tick(
    const Config& config,
    const state::MarketDepthView& depth,
    ReduceExitStage stage,
    std::uint64_t age_ms
) noexcept {
    if (depth.ask_count == 0 || depth.asks[0].price_tick <= 0) {
        return 0;
    }
    const auto top_ask = depth.asks[0].price_tick;
    const auto top_bid =
        depth.bid_count > 0 && depth.bids[0].price_tick > 0
            ? depth.bids[0].price_tick
            : 0;
    const auto join_tick =
        std::max<std::int64_t>(1, config.passive_reduce_join_tick);
    const auto passive_price =
        std::max<std::int64_t>(1, top_ask - join_tick);
    if (stage == ReduceExitStage::Passive || top_bid <= 0) {
        return passive_price;
    }

    if (stage == ReduceExitStage::Puke) {
        return std::clamp<std::int64_t>(
            top_bid - config.urgent_unwind_aggression_tick,
            1,
            mm::kPriceOneTick - 1
        );
    }

    const auto mid = midpoint_tick(depth);
    const auto age_extra =
        static_cast<std::int64_t>(std::min<std::uint64_t>(
            age_ms / 1'000,
            60
        )) * join_tick;
    const auto urgent_candidate = mid > 0
        ? std::min(passive_price, mid - age_extra)
        : passive_price - age_extra;
    const auto post_only_floor =
        std::min<std::int64_t>(top_ask - 1, top_bid + join_tick);
    return std::clamp<std::int64_t>(
        std::max<std::int64_t>(post_only_floor, urgent_candidate),
        1,
        mm::kPriceOneTick - 1
    );
}

risk::ApprovedQuote make_reduce_ask_quote(
    const Config& config,
    const state::MarketDepthView& depth,
    const std::string& asset_id,
    std::int64_t position_lots,
    std::int64_t fair_tick,
    bool complement_asset,
    ReduceQuoteState* reduce_state,
    std::uint64_t now
) {
    risk::ApprovedQuote approved;
    if (asset_id.empty() || reduce_state == nullptr ||
        depth.ask_count == 0 || depth.asks[0].price_tick <= 0) {
        return approved;
    }

    const auto target = reduce_target_lots(config);
    if (position_lots <= target) {
        return approved;
    }
    const auto reducible = position_lots - target;
    if (reduce_state->excess_since_ns == 0) {
        reduce_state->excess_since_ns = now;
    }
    const auto age_ms =
        now >= reduce_state->excess_since_ns
            ? (now - reduce_state->excess_since_ns) / 1'000'000ULL
            : 0;
    const auto pressure_bps = reduce_pressure_bps(config, reducible);
    const auto stage =
        reduce_exit_stage(config, reducible, age_ms, pressure_bps);
    const auto ask_price =
        reduce_exit_price_tick(config, depth, stage, age_ms);
    const auto base_qty =
        stage == ReduceExitStage::Passive
            ? config.quote_size_lots
            : config.max_fill_qty_per_trade;
    const auto qty = std::min({
        std::max<std::int64_t>(1, base_qty),
        std::max<std::int64_t>(1, config.max_fill_qty_per_trade),
        reducible
    });
    if (qty <= 0 || ask_price <= 0) {
        return approved;
    }

    mm::QuoteLeg ask;
    ask.market_id = config.market_id;
    ask.asset_id = asset_id;
    ask.market_index = 1;
    ask.asset_index = depth.asset_index;
    ask.side = mm::QuoteSide::Ask;
    ask.price_tick = ask_price;
    ask.quantity_lots = qty;
    ask.fair_value_tick = fair_tick;
    if (ask.fair_value_tick <= 0) {
        ask.fair_value_tick = midpoint_tick(depth);
    }
    ask.edge_to_fair_tick =
        ask.fair_value_tick > 0 ? ask.price_tick - ask.fair_value_tick : 0;
    ask.risk_reducing = true;
    ask.allow_fair_deviation_exemption = true;
    ask.allow_spread_exemption = true;
    ask.book_version = depth.book_version;
    ask.snapshot_version_hash = depth.snapshot_version_hash;

    approved.quote_intent_id = mm::compute_quote_leg_hash(ask);
    approved.quote_group_id =
        reduce_quote_group_id(config, depth.asset_index, complement_asset);
    approved.has_ask = true;
    approved.ask = ask;
    approved.approved_ts_ns = now;
    approved.expires_at_ns = now + 250'000'000ULL;
    approved.snapshot_version_hash = depth.snapshot_version_hash;
    approved.idempotency_hash = mm::compute_quote_leg_hash(ask);
    approved.approved_quote_id = risk::compute_approved_quote_hash(approved);

    reduce_state->latest_stage = stage;
    reduce_state->latest_age_ms = age_ms;
    reduce_state->latest_excess_lots = reducible;
    reduce_state->latest_pressure_bps = pressure_bps;
    reduce_state->latest_price_tick = ask_price;
    reduce_state->latest_qty_lots = qty;
    return approved;
}

void observe_reduce_exit_quote(
    const risk::ApprovedQuote& quote,
    const ReduceQuoteState& state,
    bool complement_asset,
    Stats* stats
) {
    stats->reduce_exit_quotes_submitted.fetch_add(1);
    switch (state.latest_stage) {
        case ReduceExitStage::Passive:
            stats->reduce_exit_passive_quotes.fetch_add(1);
            break;
        case ReduceExitStage::Urgent:
            stats->reduce_exit_urgent_quotes.fetch_add(1);
            break;
        case ReduceExitStage::Puke:
            stats->reduce_exit_puke_quotes.fetch_add(1);
            break;
        case ReduceExitStage::None:
            break;
    }
    stats->latest_reduce_exit_asset_side.store(complement_asset ? 2 : 1);
    stats->latest_reduce_exit_stage.store(
        static_cast<std::uint64_t>(state.latest_stage)
    );
    stats->latest_reduce_exit_age_ms.store(state.latest_age_ms);
    stats->latest_reduce_exit_excess_lots.store(state.latest_excess_lots);
    stats->latest_reduce_exit_pressure_bps.store(state.latest_pressure_bps);
    stats->latest_reduce_exit_price_tick.store(quote.ask.price_tick);
    stats->latest_reduce_exit_qty_lots.store(quote.ask.quantity_lots);
}

bool reduce_quote_group_matches(
    const RuntimeState& runtime,
    std::uint64_t quote_group_id
) noexcept {
    return quote_group_id != 0 &&
           (quote_group_id == runtime.yes_reduce_quote.quote_group_id ||
            quote_group_id == runtime.no_reduce_quote.quote_group_id);
}

void mark_reduce_quote_inactive(
    RuntimeState* runtime,
    std::uint64_t quote_group_id
) noexcept {
    if (quote_group_id == runtime->yes_reduce_quote.quote_group_id) {
        runtime->yes_reduce_quote.quote_active = false;
    }
    if (quote_group_id == runtime->no_reduce_quote.quote_group_id) {
        runtime->no_reduce_quote.quote_active = false;
    }
}

void consume_maker_reports(
    const Config& config,
    mm::MarketMakingEngine* engine,
    Stats* stats,
    RuntimeState* runtime,
    const std::vector<execution::MakerExecutionReport>& reports
) {
    for (const auto& report : reports) {
        stats->maker_reports.fetch_add(1);
        const auto observed = runtime->event_adapter.observe(report);
        if (!observed.has_paper_fill) {
            continue;
        }
        const auto applied = runtime->ledger.apply_fill(observed.paper_fill);
        if (applied.applied) {
            (void)engine->remove_active_quote(report.asset_index);
            if (reduce_quote_group_matches(*runtime, report.quote_group_id) &&
                report.status == execution::MakerQuoteStatus::Filled) {
                mark_reduce_quote_inactive(runtime, report.quote_group_id);
            }
            stats->maker_fills_applied.fetch_add(1);
            const auto mark_at_fill =
                mark_tick_for_asset(config, *runtime, report.asset_id);
            runtime->attribute_fill(report, mark_at_fill);
            const auto notional =
                static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, report.filled_qty_lots) *
                    std::max<std::int64_t>(0, report.avg_fill_price_tick)
                );
            stats->gross_fill_notional_tick.fetch_add(notional);
            append_recent_fill(
                runtime,
                report,
                mark_at_fill
            );
        } else {
            stats->maker_fills_rejected.fetch_add(1);
        }
    }
}

void update_reduce_quote(
    const Config& config,
    const state::MarketDepthView* depth,
    const std::string& asset_id,
    std::int64_t fair_tick,
    bool complement_asset,
    ReduceQuoteState* reduce_state,
    Stats* stats,
    RuntimeState* runtime,
    std::uint64_t now
) {
    if (depth == nullptr || asset_id.empty() || reduce_state == nullptr) {
        return;
    }
    reduce_state->quote_group_id =
        reduce_quote_group_id(config, depth->asset_index, complement_asset);
    const auto position = runtime->ledger.position_ledger().lots(asset_id);
    if (position <= reduce_target_lots(config)) {
        reduce_state->excess_since_ns = 0;
        reduce_state->latest_stage = ReduceExitStage::None;
        reduce_state->latest_age_ms = 0;
        reduce_state->latest_excess_lots = 0;
        reduce_state->latest_pressure_bps = 0;
        reduce_state->latest_price_tick = 0;
        reduce_state->latest_qty_lots = 0;
        if (reduce_state->quote_active) {
            const auto cancelled =
                runtime->execution_adapter.cancel_quote_group(
                    reduce_state->quote_group_id,
                    now
                );
            if (cancelled.ok) {
                stats->cancelled_quotes.fetch_add(1);
                reduce_state->quote_active = false;
            }
        }
        return;
    }

    auto approved =
        make_reduce_ask_quote(
            config,
            *depth,
            asset_id,
            position,
            fair_tick,
            complement_asset,
            reduce_state,
            now
        );
    if (!approved.has_ask) {
        if (reduce_state->quote_active) {
            const auto cancelled =
                runtime->execution_adapter.cancel_quote_group(
                    reduce_state->quote_group_id,
                    now
                );
            if (cancelled.ok) {
                stats->cancelled_quotes.fetch_add(1);
                reduce_state->quote_active = false;
            }
        }
        return;
    }
    const auto submitted =
        runtime->execution_adapter.submit_approved_quote(approved, now);
    if (submitted.ok) {
        stats->submitted_quotes.fetch_add(1);
        reduce_state->quote_active = true;
        observe_reduce_exit_quote(
            approved,
            *reduce_state,
            complement_asset,
            stats
        );
        if (submitted.replaced) {
            stats->replaced_quotes.fetch_add(1);
        }
        if (submitted.duplicate_ignored) {
            stats->duplicate_ignored.fetch_add(1);
        }
    } else {
        stats->submit_errors.fetch_add(1);
    }
}

std::string dashboard_json_unlocked(
    const Config& config,
    const Stats& stats,
    const RuntimeState& runtime,
    std::uint64_t runtime_seconds,
    const BtcOracleSnapshot& oracle_snapshot
) {
    const auto ledger_snapshot = runtime.ledger.snapshot();
    const auto position =
        runtime.ledger.position_ledger().find(config.asset_id);
    const auto complement_position = config.complement_asset_id.empty()
        ? nullptr
        : runtime.ledger.position_ledger().find(config.complement_asset_id);
    const auto open_position_lots = position ? position->qty_lots : 0;
    const auto complement_position_lots =
        complement_position ? complement_position->qty_lots : 0;
    const auto condition_complete_sets_lots =
        std::min(open_position_lots, complement_position_lots);
    const auto condition_net_exposure_lots =
        open_position_lots - complement_position_lots;
    const auto avg_cost_tick = position ? position->avg_cost_tick : 0;
    const auto cost_basis_tick = position ? position->cost_basis_tick : 0;
    const auto gross_pnl =
        runtime.pnl.maker_realized_pnl_tick +
        runtime.pnl.maker_unrealized_pnl_mid_tick;
    const auto net_pnl = gross_pnl - runtime.pnl.fees_paid_tick;
    const auto latest_return =
        static_cast<double>(runtime.pnl.equity_mid_tick -
                            config.starting_cash_tick) /
        static_cast<double>(config.starting_cash_tick);
    const auto approval_rate = safe_ratio(
        stats.risk_approved.load(),
        stats.risk_evaluated.load()
    );
    const auto fill_rate = safe_ratio(
        stats.maker_fills_applied.load(),
        stats.submitted_quotes.load()
    );
    const auto conversion_rate = safe_ratio(
        stats.submitted_quotes.load(),
        stats.mm_quote_intents.load()
    );
    const auto turnover =
        static_cast<double>(stats.gross_fill_notional_tick.load()) /
        static_cast<double>(config.starting_cash_tick);
    const auto tte_ns = time_to_expiry_ns(config);
    const auto raw_external_fair =
        external_fair_runtime(config, tte_ns, oracle_snapshot);
    const auto external_fair =
        apply_existing_basis(config, runtime, raw_external_fair);
    const auto external_tick = external_fair.tick;
    const auto dynamic_quote =
        dynamic_quote_runtime(config, external_fair, oracle_snapshot);
    const auto bid_momentum_shutoff =
        momentum_bid_shutoff(config, oracle_snapshot);
    const auto attribution_mark_tick =
        mark_tick_for_asset(config, runtime, config.asset_id);
    const auto attribution_complement_mark_tick =
        config.complement_asset_id.empty()
            ? 0
            : mark_tick_for_asset(config, runtime, config.complement_asset_id);
    const auto seed_unrealized_mid_tick = attributed_unrealized_tick(
        runtime.attribution.seed_position_lots,
        runtime.attribution.seed_cost_basis_tick,
        attribution_mark_tick
    );
    const auto seed_complement_unrealized_mid_tick =
        attributed_unrealized_tick(
            runtime.attribution.seed_complement_position_lots,
            runtime.attribution.seed_complement_cost_basis_tick,
            attribution_complement_mark_tick
        );
    const auto seed_total_pnl_mid_tick =
        runtime.attribution.seed_realized_pnl_tick +
        runtime.attribution.seed_complement_realized_pnl_tick +
        seed_unrealized_mid_tick +
        seed_complement_unrealized_mid_tick;
    const auto strategy_unrealized_mid_tick = attributed_unrealized_tick(
        runtime.attribution.strategy_position_lots,
        runtime.attribution.strategy_cost_basis_tick,
        attribution_mark_tick
    );
    const auto strategy_complement_unrealized_mid_tick =
        attributed_unrealized_tick(
            runtime.attribution.strategy_complement_position_lots,
            runtime.attribution.strategy_complement_cost_basis_tick,
            attribution_complement_mark_tick
        );
    const auto strategy_total_pnl_mid_tick =
        runtime.attribution.strategy_realized_pnl_tick +
        runtime.attribution.strategy_complement_realized_pnl_tick +
        strategy_unrealized_mid_tick +
        strategy_complement_unrealized_mid_tick;
    const auto attributed_total_pnl_mid_tick =
        seed_total_pnl_mid_tick + strategy_total_pnl_mid_tick;
    const auto aggregate_total_pnl_mid_tick =
        runtime.pnl.maker_realized_pnl_tick +
        runtime.pnl.maker_unrealized_pnl_mid_tick;
    const auto unattributed_pnl_mid_tick =
        aggregate_total_pnl_mid_tick - attributed_total_pnl_mid_tick;
    const auto no_quote_reasons = histogram_json(
        stats.no_quote_reasons,
        stats.no_quote_reasons.size(),
        [](std::size_t index) {
            return mm::no_quote_reason_name(
                static_cast<mm::NoQuoteReason>(index)
            );
        }
    );
    const auto latest_fair_quality = static_cast<mm::FairValueQuality>(
        stats.latest_fair_value_quality.load()
    );
    const auto risk_decisions = histogram_json(
        stats.risk_decisions,
        stats.risk_decisions.size(),
        [](std::size_t index) {
            return risk::quote_risk_decision_type_name(
                static_cast<risk::QuoteRiskDecisionType>(index)
            );
        }
    );

    std::ostringstream out;
    out << "{"
        << "\"seq_no\":" << runtime.dashboard_seq_no
        << ",\"ts_ns\":" << now_ns()
        << ",\"account\":{\"starting_cash_tick\":"
        << config.starting_cash_tick
        << ",\"cash_balance_tick\":" << runtime.pnl.cash_tick
        << ",\"reserved_cash_tick\":0"
        << ",\"realized_pnl_tick\":"
        << runtime.pnl.maker_realized_pnl_tick
        << ",\"unrealized_pnl_tick\":"
        << runtime.pnl.maker_unrealized_pnl_mid_tick << "}"
        << ",\"performance\":{\"intents_observed\":"
        << stats.mm_quote_intents.load()
        << ",\"approvals_observed\":" << stats.risk_approved.load()
        << ",\"plans_observed\":" << stats.submitted_quotes.load()
        << ",\"execution_reports_observed\":"
        << stats.maker_reports.load()
        << ",\"filled_plans\":" << stats.maker_fills_applied.load()
        << ",\"failed_plans\":0"
        << ",\"gross_pnl_tick\":" << gross_pnl
        << ",\"net_pnl_tick\":" << net_pnl
        << ",\"terminal_payout_tick\":0"
        << ",\"terminal_cost_tick\":0"
        << ",\"terminal_pnl_tick\":0"
        << ",\"terminal_complete_plans\":0"
        << ",\"max_drawdown_tick\":" << runtime.max_drawdown_tick
        << ",\"max_drawdown_ratio\":"
        << (static_cast<double>(runtime.max_drawdown_tick) /
            static_cast<double>(config.starting_cash_tick))
        << ",\"returns_count\":" << runtime.dashboard_samples
        << ",\"latest_return\":" << latest_return
        << ",\"latest_return_status\":\"Ok\""
        << ",\"volatility\":0"
        << ",\"volatility_status\":\"InsufficientData\""
        << ",\"sharpe\":0"
        << ",\"sharpe_status\":\"InsufficientData\""
        << ",\"fill_rate\":" << fill_rate
        << ",\"fill_rate_status\":\"Ok\""
        << ",\"risk_approval_rate\":" << approval_rate
        << ",\"risk_approval_rate_status\":\"Ok\""
        << ",\"intent_conversion_rate\":" << conversion_rate
        << ",\"intent_conversion_rate_status\":\"Ok\""
        << ",\"turnover\":" << turnover
        << ",\"turnover_status\":\"Ok\""
        << ",\"version\":1"
        << ",\"updated_ts_ns\":" << now_ns() << "}"
        << ",\"regime\":{\"data\":\"Live\""
        << ",\"liquidity\":\""
        << (runtime.has_depth && runtime.last_depth.crossed ? "Crossed" : "Healthy")
        << "\",\"chain\":\"ReadOnly\""
        << ",\"signal\":\"MarketMaking\""
        << ",\"risk\":\""
        << (stats.risk_rejected.load() > 0 ? "Constrained" : "Healthy")
        << "\",\"execution\":\"Paper\""
        << ",\"version\":1"
        << ",\"ts_ns\":" << now_ns() << "}"
        << ",\"latency\":{\"feed_to_state_ns\":"
        << stats.latest_pipeline_latency_ns.load()
        << ",\"state_to_signal_ns\":0"
        << ",\"signal_to_risk_ns\":0"
        << ",\"risk_to_execution_ns\":0"
        << ",\"end_to_end_ns\":"
        << stats.latest_pipeline_latency_ns.load() << "}"
        << ",\"signal\":{\"intents_published\":"
        << stats.mm_quote_intents.load()
        << ",\"paper_opportunities\":"
        << stats.depth_updates.load()
        << ",\"rejected\":" << stats.mm_rejected_no_quote.load()
        << ",\"output_hash\":0}"
        << ",\"risk\":{\"decisions\":" << stats.risk_evaluated.load()
        << ",\"approved\":" << stats.risk_approved.load()
        << ",\"rejected\":" << stats.risk_rejected.load()
        << ",\"output_hash\":0}"
        << ",\"execution\":{\"plans_created\":"
        << stats.submitted_quotes.load()
        << ",\"plans_filled\":" << stats.maker_fills_applied.load()
        << ",\"plans_failed\":" << stats.submit_errors.load()
        << ",\"output_hash\":0}"
        << ",\"filled_orders\":[]"
        << ",\"terminal_pnl\":[]"
        << ",\"market_maker\":{"
        << "\"mode\":\"read_only_live\""
        << ",\"fill_mode\":\"" << fill_mode_name(config.fill_mode) << "\""
        << ",\"pure_taker_mode\":"
        << (config.pure_taker_mode ? "true" : "false")
        << ",\"queue_min_rest_ms\":" << config.queue_min_rest_ms
        << ",\"asset_id\":\"" << json_escape(config.asset_id) << "\""
        << ",\"complement_asset_id\":\""
        << json_escape(config.complement_asset_id) << "\""
        << ",\"market_id\":\"" << json_escape(config.market_id) << "\""
        << ",\"runtime_seconds\":" << runtime_seconds
        << ",\"window_end_unix_seconds\":"
        << config.window_end_unix_seconds
        << ",\"time_to_expiry_ns\":" << tte_ns
        << ",\"tte_skew_start_ns\":"
        << config.tte_skew_start_seconds * kNsPerSecond
        << ",\"tte_puke_start_ns\":"
        << config.tte_puke_start_seconds * kNsPerSecond
        << ",\"tte_max_skew_multiplier\":"
        << config.tte_max_skew_multiplier
        << ",\"starting_cash_tick\":" << config.starting_cash_tick
        << ",\"initial_position_lots\":" << config.initial_position_lots
        << ",\"initial_position_price_tick\":"
        << config.initial_position_price_tick
        << ",\"seed_complete_set\":"
        << (config.seed_complete_set ? "true" : "false")
        << ",\"initial_complement_position_lots\":"
        << config.initial_complement_position_lots
        << ",\"initial_complement_position_price_tick\":"
        << config.initial_complement_position_price_tick
        << ",\"target_position_lots\":" << config.target_position_lots
        << ",\"min_inventory_lots\":" << config.min_inventory_lots
        << ",\"max_inventory_lots\":" << config.max_inventory_lots
        << ",\"min_book_spread_tick\":"
        << config.min_book_spread_tick
        << ",\"max_quote_fair_deviation_tick\":"
        << config.max_quote_fair_deviation_tick
        << ",\"max_quote_fair_deviation_bps\":"
        << config.max_quote_fair_deviation_bps
        << ",\"complement_fair_weight_bps\":"
        << (!config.complement_asset_id.empty()
                ? config.complement_fair_weight_bps
                : 0)
        << ",\"external_fair_weight_bps\":"
        << config.external_fair_weight_bps
        << ",\"external_fair_value_tick\":" << external_tick
        << ",\"external_fair_raw_tick\":" << external_fair.raw_tick
        << ",\"external_fair_basis_tick\":" << external_fair.basis_tick
        << ",\"external_fair_basis_applied\":"
        << (external_fair.basis_applied ? "true" : "false")
        << ",\"fair_basis_smoothing_enabled\":"
        << (config.fair_basis_smoothing_enabled ? "true" : "false")
        << ",\"fair_basis_seed_tick\":" << config.fair_basis_seed_tick
        << ",\"fair_basis_ewma_alpha\":" << config.fair_basis_ewma_alpha
        << ",\"fair_basis_update_interval_ms\":"
        << config.fair_basis_update_interval_ms
        << ",\"fair_basis_updates\":" << runtime.fair_basis_updates
        << ",\"latest_fair_value_quality\":\""
        << mm::fair_value_quality_name(latest_fair_quality) << "\""
        << ",\"latest_fair_confidence_bps\":"
        << stats.latest_fair_confidence_bps.load()
        << ",\"latest_fair_book_spread_tick\":"
        << stats.latest_fair_book_spread_tick.load()
        << ",\"latest_fair_value_tick\":"
        << stats.latest_fair_value_tick.load()
        << ",\"external_fair_invert\":"
        << (config.external_fair_invert ? "true" : "false")
        << ",\"require_external_fair_for_opening_quotes\":"
        << (config.require_external_fair_for_opening_quotes
                ? "true"
                : "false")
        << ",\"assumed_latency_ms\":" << config.assumed_latency_ms
        << ",\"min_quote_edge_tick\":" << config.min_quote_edge_tick
        << ",\"adverse_selection_buffer_tick\":"
        << config.adverse_selection_buffer_tick
        << ",\"latency_buffer_tick\":"
        << latency_buffer_tick(config)
        << ",\"latency_buffer_tick_per_ms\":"
        << config.latency_buffer_tick_per_ms
        << ",\"min_requote_interval_ms\":"
        << config.min_requote_interval_ms
        << ",\"min_quote_price_change_tick\":"
        << config.min_quote_price_change_tick
        << ",\"btc_spot\":" << config.btc_spot
        << ",\"btc_threshold\":" << config.btc_threshold
        << ",\"btc_vol_annual_bps\":" << config.btc_vol_annual_bps
        << ",\"btc_drift_annual_bps\":" << config.btc_drift_annual_bps
        << ",\"btc_use_realized_vol\":"
        << (config.btc_use_realized_vol ? "true" : "false")
        << ",\"btc_realized_vol_annual_bps\":"
        << oracle_snapshot.realized_vol_annual_bps
        << ",\"btc_realized_vol_sample_count\":"
        << oracle_snapshot.realized_vol_sample_count
        << ",\"btc_realized_vol_window_seconds\":"
        << config.btc_realized_vol_window_seconds
        << ",\"btc_oracle_enabled\":"
        << (config.btc_oracle_enabled ? "true" : "false")
        << ",\"btc_oracle_endpoint\":\""
        << json_escape(config.btc_oracle_endpoint) << "\""
        << ",\"btc_oracle_spot\":" << oracle_snapshot.spot
        << ",\"btc_oracle_has_spot\":"
        << (oracle_snapshot.has_spot ? "true" : "false")
        << ",\"btc_oracle_stale\":"
        << (oracle_snapshot.stale ? "true" : "false")
        << ",\"btc_oracle_age_ms\":" << oracle_snapshot.latest_age_ms
        << ",\"btc_move_1s_bps\":" << oracle_snapshot.move_1s_bps
        << ",\"btc_move_500ms_bps\":"
        << oracle_snapshot.move_500ms_bps
        << ",\"btc_toxic_move_1s_bps\":"
        << config.btc_toxic_move_1s_bps
        << ",\"btc_toxic_bid\":"
        << (dynamic_quote.toxic_bid ? "true" : "false")
        << ",\"btc_toxic_ask\":"
        << (dynamic_quote.toxic_ask ? "true" : "false")
        << ",\"momentum_bid_shutoff_enabled\":"
        << (config.momentum_bid_shutoff_enabled ? "true" : "false")
        << ",\"momentum_bid_shutoff_500ms_bps\":"
        << config.momentum_bid_shutoff_500ms_bps
        << ",\"momentum_bid_shutoff_1s_bps\":"
        << config.momentum_bid_shutoff_1s_bps
        << ",\"momentum_bid_shutoff_active\":"
        << (bid_momentum_shutoff ? "true" : "false")
        << ",\"momentum_bid_shutoffs\":"
        << stats.momentum_bid_shutoffs.load()
        << ",\"btc_oracle_updates\":" << oracle_snapshot.updates
        << ",\"btc_oracle_parse_errors\":"
        << oracle_snapshot.parse_errors
        << ",\"btc_oracle_transport_errors\":"
        << oracle_snapshot.transport_errors
        << ",\"lead_lag_sniping_enabled\":"
        << (config.lead_lag_sniping_enabled ? "true" : "false")
        << ",\"macro_divergence_taker_enabled\":"
        << (config.macro_divergence_taker_enabled ? "true" : "false")
        << ",\"macro_divergence_ewma_alpha\":"
        << config.macro_divergence_ewma_alpha
        << ",\"basis_ewma_alpha\":"
        << config.basis_ewma_alpha
        << ",\"macro_shock_min_edge_tick\":"
        << config.macro_shock_min_edge_tick
        << ",\"max_allowed_spread_tick\":"
        << config.max_allowed_spread_tick
        << ",\"max_allowed_basis_tick\":"
        << config.max_allowed_basis_tick
        << ",\"macro_divergence_cooldown_ms\":"
        << config.macro_divergence_cooldown_ms
        << ",\"max_taker_fills_per_minute\":"
        << config.max_taker_fills_per_minute
        << ",\"macro_divergence_taker_size_lots\":"
        << config.macro_divergence_taker_size_lots
        << ",\"latest_ewma_fair_yes_tick\":"
        << stats.latest_ewma_fair_yes_tick.load()
        << ",\"latest_ewma_basis_yes_tick\":"
        << stats.latest_ewma_basis_yes_tick.load()
        << ",\"macro_structural_dislocation_blocked\":"
        << stats.macro_structural_dislocation_blocked.load()
        << ",\"macro_basis_uninitialized_blocked\":"
        << stats.macro_basis_uninitialized_blocked.load()
        << ",\"macro_basis_insanity_blocked\":"
        << stats.macro_basis_insanity_blocked.load()
        << ",\"macro_edge_not_crossing_blocked\":"
        << stats.macro_edge_not_crossing_blocked.load()
        << ",\"taker_circuit_breaker_tripped\":"
        << stats.taker_circuit_breaker_tripped.load()
        << ",\"lead_lag_min_move_500ms_bps\":"
        << config.lead_lag_min_move_500ms_bps
        << ",\"lead_lag_min_stale_edge_tick\":"
        << config.lead_lag_min_stale_edge_tick
        << ",\"lead_lag_taker_size_lots\":"
        << config.lead_lag_taker_size_lots
        << ",\"taker_max_entry_mid_slippage_tick\":"
        << config.taker_max_entry_mid_slippage_tick
        << ",\"macro_divergence_sniping_signals\":"
        << stats.macro_divergence_sniping_signals.load()
        << ",\"macro_divergence_taker_ioc_fills_applied\":"
        << stats.macro_divergence_taker_ioc_fills_applied.load()
        << ",\"lead_lag_sniping_signals\":"
        << stats.lead_lag_sniping_signals.load()
        << ",\"lead_lag_taker_ioc_fills_applied\":"
        << stats.lead_lag_taker_ioc_fills_applied.load()
        << ",\"latest_lead_lag_side\":\""
        << lead_lag_side_name(stats.latest_lead_lag_side.load()) << "\""
        << ",\"latest_lead_lag_edge_tick\":"
        << stats.latest_lead_lag_edge_tick.load()
        << ",\"latest_lead_lag_price_tick\":"
        << stats.latest_lead_lag_price_tick.load()
        << ",\"locked_book_taker_hunter_enabled\":"
        << (config.locked_book_taker_hunter_enabled ? "true" : "false")
        << ",\"locked_book_taker_min_edge_tick\":"
        << config.locked_book_taker_min_edge_tick
        << ",\"locked_book_taker_effective_min_edge_tick\":"
        << effective_locked_book_taker_min_edge_tick(config)
        << ",\"locked_book_taker_cooldown_ms\":"
        << config.locked_book_taker_cooldown_ms
        << ",\"locked_book_taker_size_lots\":"
        << config.locked_book_taker_size_lots
        << ",\"taker_ioc_signals\":"
        << stats.taker_ioc_signals.load()
        << ",\"taker_ioc_fills_applied\":"
        << stats.taker_ioc_fills_applied.load()
        << ",\"taker_ioc_yes_fills_applied\":"
        << stats.taker_ioc_yes_fills_applied.load()
        << ",\"taker_ioc_no_fills_applied\":"
        << stats.taker_ioc_no_fills_applied.load()
        << ",\"taker_ioc_fills_rejected\":"
        << stats.taker_ioc_fills_rejected.load()
        << ",\"taker_ioc_cooldown_blocked\":"
        << stats.taker_ioc_cooldown_blocked.load()
        << ",\"taker_ioc_inventory_blocked\":"
        << stats.taker_ioc_inventory_blocked.load()
        << ",\"taker_ioc_cash_blocked\":"
        << stats.taker_ioc_cash_blocked.load()
        << ",\"taker_ioc_mid_slippage_blocked\":"
        << stats.taker_ioc_mid_slippage_blocked.load()
        << ",\"taker_ioc_notional_tick\":"
        << stats.taker_ioc_notional_tick.load()
        << ",\"taker_ioc_expected_edge_tick\":"
        << stats.taker_ioc_expected_edge_tick.load()
        << ",\"latest_taker_ioc_edge_tick\":"
        << stats.latest_taker_ioc_edge_tick.load()
        << ",\"latest_taker_ioc_price_tick\":"
        << stats.latest_taker_ioc_price_tick.load()
        << ",\"latest_taker_ioc_qty_lots\":"
        << stats.latest_taker_ioc_qty_lots.load()
        << ",\"latest_taker_ioc_mid_tick\":"
        << stats.latest_taker_ioc_mid_tick.load()
        << ",\"latest_taker_ioc_mid_slippage_tick\":"
        << stats.latest_taker_ioc_mid_slippage_tick.load()
        << ",\"latest_taker_ioc_asset_side\":\""
        << taker_asset_side_name(stats.latest_taker_ioc_asset_side.load())
        << "\""
        << ",\"latest_taker_ioc_source\":\""
        << taker_ioc_source_name(stats.latest_taker_ioc_source.load())
        << "\""
        << ",\"reduce_exit_quotes_submitted\":"
        << stats.reduce_exit_quotes_submitted.load()
        << ",\"reduce_exit_passive_quotes\":"
        << stats.reduce_exit_passive_quotes.load()
        << ",\"reduce_exit_urgent_quotes\":"
        << stats.reduce_exit_urgent_quotes.load()
        << ",\"reduce_exit_puke_quotes\":"
        << stats.reduce_exit_puke_quotes.load()
        << ",\"latest_reduce_exit_asset_side\":\""
        << taker_asset_side_name(
               stats.latest_reduce_exit_asset_side.load()
           ) << "\""
        << ",\"latest_reduce_exit_stage\":\""
        << reduce_exit_stage_name(stats.latest_reduce_exit_stage.load())
        << "\""
        << ",\"latest_reduce_exit_age_ms\":"
        << stats.latest_reduce_exit_age_ms.load()
        << ",\"latest_reduce_exit_excess_lots\":"
        << stats.latest_reduce_exit_excess_lots.load()
        << ",\"latest_reduce_exit_pressure_bps\":"
        << stats.latest_reduce_exit_pressure_bps.load()
        << ",\"latest_reduce_exit_price_tick\":"
        << stats.latest_reduce_exit_price_tick.load()
        << ",\"latest_reduce_exit_qty_lots\":"
        << stats.latest_reduce_exit_qty_lots.load()
        << ",\"urgent_reduce_age_ms\":"
        << config.urgent_reduce_age_ms
        << ",\"puke_reduce_age_ms\":"
        << config.puke_reduce_age_ms
        << ",\"urgent_reduce_pressure_bps\":"
        << config.urgent_reduce_pressure_bps
        << ",\"puke_reduce_pressure_bps\":"
        << config.puke_reduce_pressure_bps
        << ",\"as_model_enabled\":"
        << (config.enable_as_model ? "true" : "false")
        << ",\"as_model_ok\":"
        << (dynamic_quote.as_ok ? "true" : "false")
        << ",\"as_risk_aversion\":" << config.as_risk_aversion
        << ",\"as_order_arrival_k\":" << config.as_order_arrival_k
        << ",\"as_spread_multiplier\":"
        << dynamic_quote.spread_multiplier
        << ",\"as_half_spread_tick\":"
        << dynamic_quote.half_spread_tick
        << ",\"as_inventory_skew_tick\":"
        << dynamic_quote.max_inventory_skew_tick
        << ",\"as_reservation_risk_tick\":"
        << dynamic_quote.reservation_risk_tick
        << ",\"cash_tick\":" << runtime.pnl.cash_tick
        << ",\"realized_pnl_tick\":"
        << runtime.pnl.maker_realized_pnl_tick
        << ",\"unrealized_pnl_mid_tick\":"
        << runtime.pnl.maker_unrealized_pnl_mid_tick
        << ",\"unrealized_pnl_liquidation_tick\":"
        << runtime.pnl.maker_unrealized_pnl_liquidation_tick
        << ",\"equity_mid_tick\":" << runtime.pnl.equity_mid_tick
        << ",\"equity_liquidation_tick\":"
        << runtime.pnl.equity_liquidation_tick
        << ",\"fees_paid_tick\":" << runtime.pnl.fees_paid_tick
        << ",\"seed_position_lots\":"
        << runtime.attribution.seed_position_lots
        << ",\"seed_cost_basis_tick\":"
        << runtime.attribution.seed_cost_basis_tick
        << ",\"seed_realized_pnl_tick\":"
        << runtime.attribution.seed_realized_pnl_tick
        << ",\"seed_unrealized_pnl_mid_tick\":"
        << seed_unrealized_mid_tick
        << ",\"seed_complement_position_lots\":"
        << runtime.attribution.seed_complement_position_lots
        << ",\"seed_complement_cost_basis_tick\":"
        << runtime.attribution.seed_complement_cost_basis_tick
        << ",\"seed_complement_unrealized_pnl_mid_tick\":"
        << seed_complement_unrealized_mid_tick
        << ",\"seed_total_pnl_mid_tick\":"
        << seed_total_pnl_mid_tick
        << ",\"strategy_position_lots\":"
        << runtime.attribution.strategy_position_lots
        << ",\"strategy_cost_basis_tick\":"
        << runtime.attribution.strategy_cost_basis_tick
        << ",\"strategy_complement_position_lots\":"
        << runtime.attribution.strategy_complement_position_lots
        << ",\"strategy_complement_cost_basis_tick\":"
        << runtime.attribution.strategy_complement_cost_basis_tick
        << ",\"strategy_realized_pnl_tick\":"
        << runtime.attribution.strategy_realized_pnl_tick +
               runtime.attribution.strategy_complement_realized_pnl_tick
        << ",\"strategy_unrealized_pnl_mid_tick\":"
        << strategy_unrealized_mid_tick +
               strategy_complement_unrealized_mid_tick
        << ",\"strategy_complement_unrealized_pnl_mid_tick\":"
        << strategy_complement_unrealized_mid_tick
        << ",\"strategy_total_pnl_mid_tick\":"
        << strategy_total_pnl_mid_tick
        << ",\"strategy_spread_capture_tick\":"
        << runtime.attribution.strategy_spread_capture_tick
        << ",\"unattributed_pnl_mid_tick\":"
        << unattributed_pnl_mid_tick
        << ",\"maker_fill_count\":" << runtime.pnl.maker_fill_count
        << ",\"open_position_lots\":" << open_position_lots
        << ",\"complement_position_lots\":"
        << complement_position_lots
        << ",\"condition_complete_sets_lots\":"
        << condition_complete_sets_lots
        << ",\"condition_net_exposure_lots\":"
        << condition_net_exposure_lots
        << ",\"avg_cost_tick\":" << avg_cost_tick
        << ",\"cost_basis_tick\":" << cost_basis_tick
        << ",\"mark_quality\":\""
        << mark_quality_name(runtime.pnl.mark_quality) << "\""
        << ",\"best_bid_tick\":" << best_bid_tick(runtime)
        << ",\"best_ask_tick\":" << best_ask_tick(runtime)
        << ",\"spread_tick\":" << spread_tick(runtime)
        << ",\"book_version\":"
        << (runtime.has_depth ? runtime.last_depth.book_version : 0)
        << ",\"active_quotes\":"
        << runtime.execution_adapter.quote_book().active_quote_count()
        << ",\"submitted_quotes\":" << stats.submitted_quotes.load()
        << ",\"replaced_quotes\":" << stats.replaced_quotes.load()
        << ",\"cancelled_quotes\":" << stats.cancelled_quotes.load()
        << ",\"cancel_errors\":" << stats.cancel_errors.load()
        << ",\"duplicate_ignored\":" << stats.duplicate_ignored.load()
        << ",\"maker_reports\":" << stats.maker_reports.load()
        << ",\"maker_fills_applied\":"
        << stats.maker_fills_applied.load()
        << ",\"maker_fills_rejected\":"
        << stats.maker_fills_rejected.load()
        << ",\"gross_fill_notional_tick\":"
        << stats.gross_fill_notional_tick.load()
        << ",\"ws_packets\":" << stats.ws_packets.load()
        << ",\"normalized_events\":" << stats.normalized_events.load()
        << ",\"filtered_events\":" << stats.filtered_events.load()
        << ",\"book_snapshots\":" << stats.book_snapshots.load()
        << ",\"book_deltas\":" << stats.book_deltas.load()
        << ",\"depth_updates\":" << stats.depth_updates.load()
        << ",\"book_quarantine_ms\":" << config.book_quarantine_ms
        << ",\"book_quarantine_events\":"
        << stats.book_quarantine_events.load()
        << ",\"depth_updates_quarantined\":"
        << stats.depth_updates_quarantined.load()
        << ",\"book_quarantine_active\":"
        << (runtime.book_quarantine_until_ns > now_ns()
                ? "true"
                : "false")
        << ",\"quote_intents\":" << stats.mm_quote_intents.load()
        << ",\"cancel_intents\":" << stats.mm_cancel_intents.load()
        << ",\"no_quote_reasons\":" << no_quote_reasons
        << ",\"risk_evaluated\":" << stats.risk_evaluated.load()
        << ",\"risk_approved\":" << stats.risk_approved.load()
        << ",\"risk_rejected\":" << stats.risk_rejected.load()
        << ",\"risk_decisions\":" << risk_decisions
        << ",\"decode_errors\":" << stats.decode_errors.load()
        << ",\"state_errors\":" << stats.state_errors.load()
        << ",\"transport_errors\":" << stats.transport_errors.load()
        << ",\"dashboard_write_errors\":"
        << stats.dashboard_write_errors.load()
        << ",\"latest_pipeline_latency_ns\":"
        << stats.latest_pipeline_latency_ns.load()
        << ",\"dashboard_samples\":" << runtime.dashboard_samples
        << ",\"paper_ledger_applied_fills\":"
        << ledger_snapshot.applied_fill_count
        << ",\"paper_ledger_position_count\":"
        << ledger_snapshot.position_count
        << ",\"recent_fills\":[";

    bool first = true;
    for (const auto& fill : runtime.recent_fills) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "{\"report_id\":" << fill.report_id
            << ",\"quote_id\":" << fill.quote_id
            << ",\"quote_group_id\":" << fill.quote_group_id
            << ",\"asset_id\":\"" << json_escape(fill.asset_id)
            << "\",\"side\":\"" << json_escape(fill.side)
            << "\",\"qty_lots\":" << fill.qty_lots
            << ",\"fill_price_tick\":" << fill.fill_price_tick
            << ",\"remaining_qty_lots\":" << fill.remaining_qty_lots
            << ",\"reason\":\"" << json_escape(fill.reason)
            << "\",\"liquidity_role\":\"" << json_escape(fill.liquidity_role)
            << "\",\"expected_edge_tick\":" << fill.expected_edge_tick
            << ",\"mark_at_fill_tick\":" << fill.mark_at_fill_tick
            << ",\"markout_1s_ready\":"
            << (fill.markout_1s_ready ? "true" : "false")
            << ",\"markout_1s_tick\":" << fill.markout_1s_tick
            << ",\"markout_5s_ready\":"
            << (fill.markout_5s_ready ? "true" : "false")
            << ",\"markout_5s_tick\":" << fill.markout_5s_tick
            << ",\"markout_30s_ready\":"
            << (fill.markout_30s_ready ? "true" : "false")
            << ",\"markout_30s_tick\":" << fill.markout_30s_tick
            << ",\"ts_ns\":" << fill.ts_ns << "}";
    }

    out << "]}}";
    return out.str();
}

void write_dashboard_snapshot(
    const Config& config,
    Stats* stats,
    RuntimeState* runtime,
    std::uint64_t runtime_seconds,
    const BtcOracleSnapshot& oracle_snapshot
) {
    if (config.dashboard_file.empty()) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        ++runtime->dashboard_seq_no;
        ++runtime->dashboard_samples;
        if (runtime->has_depth) {
            update_pnl_snapshot_unlocked(
                config,
                runtime,
                runtime->last_depth,
                now_ns()
            );
        }
        const auto body =
            dashboard_json_unlocked(
                config,
                *stats,
                *runtime,
                runtime_seconds,
                oracle_snapshot
            );
        write_atomic(config.dashboard_file, body);
    } catch (const std::exception& error) {
        stats->dashboard_write_errors.fetch_add(1);
        std::cerr << "dashboard_write_error: " << error.what() << '\n';
    }
}

void write_latency_json(
    std::ostream& out,
    const char* name,
    const LatencyStats& stats,
    bool trailing_comma
) {
    out << "  \"" << name << "\": {\n"
        << "    \"count\": " << stats.count << ",\n"
        << "    \"min\": " << stats.min << ",\n"
        << "    \"p50\": " << stats.p50 << ",\n"
        << "    \"p90\": " << stats.p90 << ",\n"
        << "    \"p95\": " << stats.p95 << ",\n"
        << "    \"p99\": " << stats.p99 << ",\n"
        << "    \"max\": " << stats.max << ",\n"
        << "    \"mean\": " << std::fixed << std::setprecision(2)
        << stats.mean << "\n"
        << "  }" << (trailing_comma ? "," : "") << "\n";
}

void write_summary_json(
    const Config& config,
    const Stats& stats,
    RuntimeState* runtime,
    std::uint64_t runtime_seconds,
    const LatencyStats& pipeline_stats,
    const BtcOracleSnapshot& oracle_snapshot
) {
    if (config.out_json.empty()) {
        return;
    }
    std::ostringstream out;
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        const auto tte_ns = time_to_expiry_ns(config);
        const auto raw_external_fair =
            external_fair_runtime(config, tte_ns, oracle_snapshot);
        const auto external_fair =
            apply_existing_basis(config, *runtime, raw_external_fair);
        const auto external_tick = external_fair.tick;
        const auto dynamic_quote =
            dynamic_quote_runtime(config, external_fair, oracle_snapshot);
        const auto bid_momentum_shutoff =
            momentum_bid_shutoff(config, oracle_snapshot);
        const auto no_quote_reasons = histogram_json(
            stats.no_quote_reasons,
            stats.no_quote_reasons.size(),
            [](std::size_t index) {
                return mm::no_quote_reason_name(
                    static_cast<mm::NoQuoteReason>(index)
                );
            }
        );
        const auto latest_fair_quality =
            static_cast<mm::FairValueQuality>(
                stats.latest_fair_value_quality.load()
            );
        const auto risk_decisions = histogram_json(
            stats.risk_decisions,
            stats.risk_decisions.size(),
            [](std::size_t index) {
                return risk::quote_risk_decision_type_name(
                    static_cast<risk::QuoteRiskDecisionType>(index)
                );
            }
        );
        const auto complement_position_lots =
            config.complement_asset_id.empty()
                ? 0
                : runtime->ledger.position_ledger().lots(
                      config.complement_asset_id
                  );
        const auto open_position_lots =
            runtime->ledger.position_ledger().lots(config.asset_id);
        const auto attribution_mark_tick =
            mark_tick_for_asset(config, *runtime, config.asset_id);
        const auto attribution_complement_mark_tick =
            config.complement_asset_id.empty()
                ? 0
                : mark_tick_for_asset(
                      config,
                      *runtime,
                      config.complement_asset_id
                  );
        const auto seed_unrealized_mid_tick = attributed_unrealized_tick(
            runtime->attribution.seed_position_lots,
            runtime->attribution.seed_cost_basis_tick,
            attribution_mark_tick
        );
        const auto seed_complement_unrealized_mid_tick =
            attributed_unrealized_tick(
                runtime->attribution.seed_complement_position_lots,
                runtime->attribution.seed_complement_cost_basis_tick,
                attribution_complement_mark_tick
            );
        const auto seed_total_pnl_mid_tick =
            runtime->attribution.seed_realized_pnl_tick +
            runtime->attribution.seed_complement_realized_pnl_tick +
            seed_unrealized_mid_tick +
            seed_complement_unrealized_mid_tick;
        const auto strategy_unrealized_mid_tick = attributed_unrealized_tick(
            runtime->attribution.strategy_position_lots,
            runtime->attribution.strategy_cost_basis_tick,
            attribution_mark_tick
        );
        const auto strategy_complement_unrealized_mid_tick =
            attributed_unrealized_tick(
                runtime->attribution.strategy_complement_position_lots,
                runtime->attribution.strategy_complement_cost_basis_tick,
                attribution_complement_mark_tick
            );
        const auto strategy_total_pnl_mid_tick =
            runtime->attribution.strategy_realized_pnl_tick +
            runtime->attribution.strategy_complement_realized_pnl_tick +
            strategy_unrealized_mid_tick +
            strategy_complement_unrealized_mid_tick;
        const auto attributed_total_pnl_mid_tick =
            seed_total_pnl_mid_tick + strategy_total_pnl_mid_tick;
        const auto aggregate_total_pnl_mid_tick =
            runtime->pnl.maker_realized_pnl_tick +
            runtime->pnl.maker_unrealized_pnl_mid_tick;
        const auto unattributed_pnl_mid_tick =
            aggregate_total_pnl_mid_tick - attributed_total_pnl_mid_tick;
        out << "{\n"
            << "  \"mode\": \"read_only_live_market_maker\",\n"
            << "  \"fill_mode\": \"" << fill_mode_name(config.fill_mode)
            << "\",\n"
            << "  \"pure_taker_mode\": "
            << (config.pure_taker_mode ? "true" : "false") << ",\n"
            << "  \"queue_min_rest_ms\": "
            << config.queue_min_rest_ms << ",\n"
            << "  \"runtime_seconds\": " << runtime_seconds << ",\n"
            << "  \"asset_id\": \"" << json_escape(config.asset_id) << "\",\n"
            << "  \"complement_asset_id\": \""
            << json_escape(config.complement_asset_id) << "\",\n"
            << "  \"window_end_unix_seconds\": "
            << config.window_end_unix_seconds << ",\n"
            << "  \"time_to_expiry_ns\": "
            << tte_ns << ",\n"
            << "  \"tte_skew_start_ns\": "
            << config.tte_skew_start_seconds * kNsPerSecond << ",\n"
            << "  \"tte_puke_start_ns\": "
            << config.tte_puke_start_seconds * kNsPerSecond << ",\n"
            << "  \"tte_max_skew_multiplier\": "
            << config.tte_max_skew_multiplier << ",\n"
            << "  \"starting_cash_tick\": " << config.starting_cash_tick
            << ",\n"
            << "  \"initial_position_lots\": "
            << config.initial_position_lots << ",\n"
            << "  \"initial_position_price_tick\": "
            << config.initial_position_price_tick << ",\n"
            << "  \"seed_complete_set\": "
            << (config.seed_complete_set ? "true" : "false") << ",\n"
            << "  \"initial_complement_position_lots\": "
            << config.initial_complement_position_lots << ",\n"
            << "  \"initial_complement_position_price_tick\": "
            << config.initial_complement_position_price_tick << ",\n"
            << "  \"target_position_lots\": "
            << config.target_position_lots << ",\n"
            << "  \"min_inventory_lots\": "
            << config.min_inventory_lots << ",\n"
            << "  \"max_inventory_lots\": "
            << config.max_inventory_lots << ",\n"
            << "  \"min_book_spread_tick\": "
            << config.min_book_spread_tick << ",\n"
            << "  \"max_quote_fair_deviation_tick\": "
            << config.max_quote_fair_deviation_tick << ",\n"
            << "  \"max_quote_fair_deviation_bps\": "
            << config.max_quote_fair_deviation_bps << ",\n"
            << "  \"complement_fair_weight_bps\": "
            << (!config.complement_asset_id.empty()
                    ? config.complement_fair_weight_bps
                    : 0)
            << ",\n"
            << "  \"external_fair_weight_bps\": "
            << config.external_fair_weight_bps << ",\n"
            << "  \"external_fair_value_tick\": "
            << external_tick << ",\n"
            << "  \"external_fair_raw_tick\": "
            << external_fair.raw_tick << ",\n"
            << "  \"external_fair_basis_tick\": "
            << external_fair.basis_tick << ",\n"
            << "  \"external_fair_basis_applied\": "
            << (external_fair.basis_applied ? "true" : "false") << ",\n"
            << "  \"fair_basis_smoothing_enabled\": "
            << (config.fair_basis_smoothing_enabled ? "true" : "false")
            << ",\n"
            << "  \"fair_basis_seed_tick\": "
            << config.fair_basis_seed_tick << ",\n"
            << "  \"fair_basis_ewma_alpha\": "
            << config.fair_basis_ewma_alpha << ",\n"
            << "  \"fair_basis_update_interval_ms\": "
            << config.fair_basis_update_interval_ms << ",\n"
            << "  \"fair_basis_updates\": "
            << runtime->fair_basis_updates << ",\n"
            << "  \"latest_fair_value_quality\": \""
            << mm::fair_value_quality_name(latest_fair_quality)
            << "\",\n"
            << "  \"latest_fair_confidence_bps\": "
            << stats.latest_fair_confidence_bps.load() << ",\n"
            << "  \"latest_fair_book_spread_tick\": "
            << stats.latest_fair_book_spread_tick.load() << ",\n"
            << "  \"latest_fair_value_tick\": "
            << stats.latest_fair_value_tick.load() << ",\n"
            << "  \"external_fair_invert\": "
            << (config.external_fair_invert ? "true" : "false") << ",\n"
            << "  \"require_external_fair_for_opening_quotes\": "
            << (config.require_external_fair_for_opening_quotes
                    ? "true"
                    : "false")
            << ",\n"
            << "  \"assumed_latency_ms\": "
            << config.assumed_latency_ms << ",\n"
            << "  \"min_quote_edge_tick\": "
            << config.min_quote_edge_tick << ",\n"
            << "  \"adverse_selection_buffer_tick\": "
            << config.adverse_selection_buffer_tick << ",\n"
            << "  \"latency_buffer_tick\": "
            << latency_buffer_tick(config) << ",\n"
            << "  \"latency_buffer_tick_per_ms\": "
            << config.latency_buffer_tick_per_ms << ",\n"
            << "  \"min_requote_interval_ms\": "
            << config.min_requote_interval_ms << ",\n"
            << "  \"min_quote_price_change_tick\": "
            << config.min_quote_price_change_tick << ",\n"
            << "  \"btc_spot\": " << config.btc_spot << ",\n"
            << "  \"btc_threshold\": " << config.btc_threshold << ",\n"
            << "  \"btc_vol_annual_bps\": "
            << config.btc_vol_annual_bps << ",\n"
            << "  \"btc_drift_annual_bps\": "
            << config.btc_drift_annual_bps << ",\n"
            << "  \"btc_use_realized_vol\": "
            << (config.btc_use_realized_vol ? "true" : "false") << ",\n"
            << "  \"btc_realized_vol_annual_bps\": "
            << oracle_snapshot.realized_vol_annual_bps << ",\n"
            << "  \"btc_realized_vol_sample_count\": "
            << oracle_snapshot.realized_vol_sample_count << ",\n"
            << "  \"btc_realized_vol_window_seconds\": "
            << config.btc_realized_vol_window_seconds << ",\n"
            << "  \"btc_oracle_enabled\": "
            << (config.btc_oracle_enabled ? "true" : "false") << ",\n"
            << "  \"btc_oracle_endpoint\": \""
            << json_escape(config.btc_oracle_endpoint) << "\",\n"
            << "  \"btc_oracle_spot\": "
            << oracle_snapshot.spot << ",\n"
            << "  \"btc_oracle_has_spot\": "
            << (oracle_snapshot.has_spot ? "true" : "false") << ",\n"
            << "  \"btc_oracle_stale\": "
            << (oracle_snapshot.stale ? "true" : "false") << ",\n"
            << "  \"btc_oracle_age_ms\": "
            << oracle_snapshot.latest_age_ms << ",\n"
            << "  \"btc_move_1s_bps\": "
            << oracle_snapshot.move_1s_bps << ",\n"
            << "  \"btc_move_500ms_bps\": "
            << oracle_snapshot.move_500ms_bps << ",\n"
            << "  \"btc_toxic_move_1s_bps\": "
            << config.btc_toxic_move_1s_bps << ",\n"
            << "  \"btc_toxic_bid\": "
            << (dynamic_quote.toxic_bid ? "true" : "false") << ",\n"
            << "  \"btc_toxic_ask\": "
            << (dynamic_quote.toxic_ask ? "true" : "false") << ",\n"
            << "  \"momentum_bid_shutoff_enabled\": "
            << (config.momentum_bid_shutoff_enabled ? "true" : "false")
            << ",\n"
            << "  \"momentum_bid_shutoff_500ms_bps\": "
            << config.momentum_bid_shutoff_500ms_bps << ",\n"
            << "  \"momentum_bid_shutoff_1s_bps\": "
            << config.momentum_bid_shutoff_1s_bps << ",\n"
            << "  \"momentum_bid_shutoff_active\": "
            << (bid_momentum_shutoff ? "true" : "false") << ",\n"
            << "  \"momentum_bid_shutoffs\": "
            << stats.momentum_bid_shutoffs.load() << ",\n"
            << "  \"btc_oracle_updates\": "
            << oracle_snapshot.updates << ",\n"
            << "  \"btc_oracle_parse_errors\": "
            << oracle_snapshot.parse_errors << ",\n"
            << "  \"btc_oracle_transport_errors\": "
            << oracle_snapshot.transport_errors << ",\n"
            << "  \"lead_lag_sniping_enabled\": "
            << (config.lead_lag_sniping_enabled ? "true" : "false")
            << ",\n"
            << "  \"macro_divergence_taker_enabled\": "
            << (config.macro_divergence_taker_enabled ? "true" : "false")
            << ",\n"
            << "  \"macro_divergence_ewma_alpha\": "
            << config.macro_divergence_ewma_alpha << ",\n"
            << "  \"basis_ewma_alpha\": "
            << config.basis_ewma_alpha << ",\n"
            << "  \"macro_shock_min_edge_tick\": "
            << config.macro_shock_min_edge_tick << ",\n"
            << "  \"max_allowed_spread_tick\": "
            << config.max_allowed_spread_tick << ",\n"
            << "  \"max_allowed_basis_tick\": "
            << config.max_allowed_basis_tick << ",\n"
            << "  \"macro_divergence_cooldown_ms\": "
            << config.macro_divergence_cooldown_ms << ",\n"
            << "  \"max_taker_fills_per_minute\": "
            << config.max_taker_fills_per_minute << ",\n"
            << "  \"macro_divergence_taker_size_lots\": "
            << config.macro_divergence_taker_size_lots << ",\n"
            << "  \"latest_ewma_fair_yes_tick\": "
            << stats.latest_ewma_fair_yes_tick.load() << ",\n"
            << "  \"latest_ewma_basis_yes_tick\": "
            << stats.latest_ewma_basis_yes_tick.load() << ",\n"
            << "  \"macro_structural_dislocation_blocked\": "
            << stats.macro_structural_dislocation_blocked.load() << ",\n"
            << "  \"macro_basis_uninitialized_blocked\": "
            << stats.macro_basis_uninitialized_blocked.load() << ",\n"
            << "  \"macro_basis_insanity_blocked\": "
            << stats.macro_basis_insanity_blocked.load() << ",\n"
            << "  \"macro_edge_not_crossing_blocked\": "
            << stats.macro_edge_not_crossing_blocked.load() << ",\n"
            << "  \"taker_circuit_breaker_tripped\": "
            << stats.taker_circuit_breaker_tripped.load() << ",\n"
            << "  \"lead_lag_min_move_500ms_bps\": "
            << config.lead_lag_min_move_500ms_bps << ",\n"
            << "  \"lead_lag_min_stale_edge_tick\": "
            << config.lead_lag_min_stale_edge_tick << ",\n"
            << "  \"lead_lag_taker_size_lots\": "
            << config.lead_lag_taker_size_lots << ",\n"
            << "  \"taker_max_entry_mid_slippage_tick\": "
            << config.taker_max_entry_mid_slippage_tick << ",\n"
            << "  \"macro_divergence_sniping_signals\": "
            << stats.macro_divergence_sniping_signals.load() << ",\n"
            << "  \"macro_divergence_taker_ioc_fills_applied\": "
            << stats.macro_divergence_taker_ioc_fills_applied.load()
            << ",\n"
            << "  \"lead_lag_sniping_signals\": "
            << stats.lead_lag_sniping_signals.load() << ",\n"
            << "  \"lead_lag_taker_ioc_fills_applied\": "
            << stats.lead_lag_taker_ioc_fills_applied.load() << ",\n"
            << "  \"latest_lead_lag_side\": \""
            << lead_lag_side_name(stats.latest_lead_lag_side.load())
            << "\",\n"
            << "  \"latest_lead_lag_edge_tick\": "
            << stats.latest_lead_lag_edge_tick.load() << ",\n"
            << "  \"latest_lead_lag_price_tick\": "
            << stats.latest_lead_lag_price_tick.load() << ",\n"
            << "  \"locked_book_taker_hunter_enabled\": "
            << (config.locked_book_taker_hunter_enabled ? "true" : "false")
            << ",\n"
            << "  \"locked_book_taker_min_edge_tick\": "
            << config.locked_book_taker_min_edge_tick << ",\n"
            << "  \"locked_book_taker_effective_min_edge_tick\": "
            << effective_locked_book_taker_min_edge_tick(config) << ",\n"
            << "  \"locked_book_taker_cooldown_ms\": "
            << config.locked_book_taker_cooldown_ms << ",\n"
            << "  \"locked_book_taker_size_lots\": "
            << config.locked_book_taker_size_lots << ",\n"
            << "  \"taker_ioc_signals\": "
            << stats.taker_ioc_signals.load() << ",\n"
            << "  \"taker_ioc_fills_applied\": "
            << stats.taker_ioc_fills_applied.load() << ",\n"
            << "  \"taker_ioc_yes_fills_applied\": "
            << stats.taker_ioc_yes_fills_applied.load() << ",\n"
            << "  \"taker_ioc_no_fills_applied\": "
            << stats.taker_ioc_no_fills_applied.load() << ",\n"
            << "  \"taker_ioc_fills_rejected\": "
            << stats.taker_ioc_fills_rejected.load() << ",\n"
            << "  \"taker_ioc_cooldown_blocked\": "
            << stats.taker_ioc_cooldown_blocked.load() << ",\n"
            << "  \"taker_ioc_inventory_blocked\": "
            << stats.taker_ioc_inventory_blocked.load() << ",\n"
            << "  \"taker_ioc_cash_blocked\": "
            << stats.taker_ioc_cash_blocked.load() << ",\n"
            << "  \"taker_ioc_mid_slippage_blocked\": "
            << stats.taker_ioc_mid_slippage_blocked.load() << ",\n"
            << "  \"taker_ioc_notional_tick\": "
            << stats.taker_ioc_notional_tick.load() << ",\n"
            << "  \"taker_ioc_expected_edge_tick\": "
            << stats.taker_ioc_expected_edge_tick.load() << ",\n"
            << "  \"latest_taker_ioc_edge_tick\": "
            << stats.latest_taker_ioc_edge_tick.load() << ",\n"
            << "  \"latest_taker_ioc_price_tick\": "
            << stats.latest_taker_ioc_price_tick.load() << ",\n"
            << "  \"latest_taker_ioc_qty_lots\": "
            << stats.latest_taker_ioc_qty_lots.load() << ",\n"
            << "  \"latest_taker_ioc_mid_tick\": "
            << stats.latest_taker_ioc_mid_tick.load() << ",\n"
            << "  \"latest_taker_ioc_mid_slippage_tick\": "
            << stats.latest_taker_ioc_mid_slippage_tick.load() << ",\n"
            << "  \"latest_taker_ioc_asset_side\": \""
            << taker_asset_side_name(stats.latest_taker_ioc_asset_side.load())
            << "\",\n"
            << "  \"latest_taker_ioc_source\": \""
            << taker_ioc_source_name(stats.latest_taker_ioc_source.load())
            << "\",\n"
            << "  \"reduce_exit_quotes_submitted\": "
            << stats.reduce_exit_quotes_submitted.load() << ",\n"
            << "  \"reduce_exit_passive_quotes\": "
            << stats.reduce_exit_passive_quotes.load() << ",\n"
            << "  \"reduce_exit_urgent_quotes\": "
            << stats.reduce_exit_urgent_quotes.load() << ",\n"
            << "  \"reduce_exit_puke_quotes\": "
            << stats.reduce_exit_puke_quotes.load() << ",\n"
            << "  \"latest_reduce_exit_asset_side\": \""
            << taker_asset_side_name(
                   stats.latest_reduce_exit_asset_side.load()
               ) << "\",\n"
            << "  \"latest_reduce_exit_stage\": \""
            << reduce_exit_stage_name(
                   stats.latest_reduce_exit_stage.load()
               ) << "\",\n"
            << "  \"latest_reduce_exit_age_ms\": "
            << stats.latest_reduce_exit_age_ms.load() << ",\n"
            << "  \"latest_reduce_exit_excess_lots\": "
            << stats.latest_reduce_exit_excess_lots.load() << ",\n"
            << "  \"latest_reduce_exit_pressure_bps\": "
            << stats.latest_reduce_exit_pressure_bps.load() << ",\n"
            << "  \"latest_reduce_exit_price_tick\": "
            << stats.latest_reduce_exit_price_tick.load() << ",\n"
            << "  \"latest_reduce_exit_qty_lots\": "
            << stats.latest_reduce_exit_qty_lots.load() << ",\n"
            << "  \"urgent_reduce_age_ms\": "
            << config.urgent_reduce_age_ms << ",\n"
            << "  \"puke_reduce_age_ms\": "
            << config.puke_reduce_age_ms << ",\n"
            << "  \"urgent_reduce_pressure_bps\": "
            << config.urgent_reduce_pressure_bps << ",\n"
            << "  \"puke_reduce_pressure_bps\": "
            << config.puke_reduce_pressure_bps << ",\n"
            << "  \"as_model_enabled\": "
            << (config.enable_as_model ? "true" : "false") << ",\n"
            << "  \"as_model_ok\": "
            << (dynamic_quote.as_ok ? "true" : "false") << ",\n"
            << "  \"as_risk_aversion\": "
            << config.as_risk_aversion << ",\n"
            << "  \"as_order_arrival_k\": "
            << config.as_order_arrival_k << ",\n"
            << "  \"as_spread_multiplier\": "
            << dynamic_quote.spread_multiplier << ",\n"
            << "  \"as_half_spread_tick\": "
            << dynamic_quote.half_spread_tick << ",\n"
            << "  \"as_inventory_skew_tick\": "
            << dynamic_quote.max_inventory_skew_tick << ",\n"
            << "  \"as_reservation_risk_tick\": "
            << dynamic_quote.reservation_risk_tick << ",\n"
            << "  \"cash_tick\": " << runtime->pnl.cash_tick << ",\n"
            << "  \"realized_pnl_tick\": "
            << runtime->pnl.maker_realized_pnl_tick << ",\n"
            << "  \"unrealized_pnl_mid_tick\": "
            << runtime->pnl.maker_unrealized_pnl_mid_tick << ",\n"
            << "  \"equity_mid_tick\": " << runtime->pnl.equity_mid_tick
            << ",\n"
            << "  \"open_position_lots\": "
            << open_position_lots
            << ",\n"
            << "  \"complement_position_lots\": "
            << complement_position_lots
            << ",\n"
            << "  \"condition_complete_sets_lots\": "
            << std::min(open_position_lots, complement_position_lots)
            << ",\n"
            << "  \"condition_net_exposure_lots\": "
            << open_position_lots - complement_position_lots
            << ",\n"
            << "  \"seed_position_lots\": "
            << runtime->attribution.seed_position_lots << ",\n"
            << "  \"seed_cost_basis_tick\": "
            << runtime->attribution.seed_cost_basis_tick << ",\n"
            << "  \"seed_realized_pnl_tick\": "
            << runtime->attribution.seed_realized_pnl_tick << ",\n"
            << "  \"seed_unrealized_pnl_mid_tick\": "
            << seed_unrealized_mid_tick << ",\n"
            << "  \"seed_complement_position_lots\": "
            << runtime->attribution.seed_complement_position_lots << ",\n"
            << "  \"seed_complement_cost_basis_tick\": "
            << runtime->attribution.seed_complement_cost_basis_tick
            << ",\n"
            << "  \"seed_complement_unrealized_pnl_mid_tick\": "
            << seed_complement_unrealized_mid_tick << ",\n"
            << "  \"seed_total_pnl_mid_tick\": "
            << seed_total_pnl_mid_tick << ",\n"
            << "  \"strategy_position_lots\": "
            << runtime->attribution.strategy_position_lots << ",\n"
            << "  \"strategy_cost_basis_tick\": "
            << runtime->attribution.strategy_cost_basis_tick << ",\n"
            << "  \"strategy_complement_position_lots\": "
            << runtime->attribution.strategy_complement_position_lots << ",\n"
            << "  \"strategy_complement_cost_basis_tick\": "
            << runtime->attribution.strategy_complement_cost_basis_tick
            << ",\n"
            << "  \"strategy_realized_pnl_tick\": "
            << runtime->attribution.strategy_realized_pnl_tick +
                   runtime->attribution.strategy_complement_realized_pnl_tick
            << ",\n"
            << "  \"strategy_unrealized_pnl_mid_tick\": "
            << strategy_unrealized_mid_tick +
                   strategy_complement_unrealized_mid_tick << ",\n"
            << "  \"strategy_complement_unrealized_pnl_mid_tick\": "
            << strategy_complement_unrealized_mid_tick << ",\n"
            << "  \"strategy_total_pnl_mid_tick\": "
            << strategy_total_pnl_mid_tick << ",\n"
            << "  \"strategy_spread_capture_tick\": "
            << runtime->attribution.strategy_spread_capture_tick << ",\n"
            << "  \"unattributed_pnl_mid_tick\": "
            << unattributed_pnl_mid_tick << ",\n"
            << "  \"maker_fill_count\": " << runtime->pnl.maker_fill_count
            << ",\n"
            << "  \"ws_packets\": " << stats.ws_packets.load() << ",\n"
            << "  \"normalized_events\": "
            << stats.normalized_events.load() << ",\n"
            << "  \"filtered_events\": " << stats.filtered_events.load()
            << ",\n"
            << "  \"book_snapshots\": " << stats.book_snapshots.load()
            << ",\n"
            << "  \"book_deltas\": " << stats.book_deltas.load() << ",\n"
            << "  \"depth_updates\": " << stats.depth_updates.load()
            << ",\n"
            << "  \"book_quarantine_ms\": "
            << config.book_quarantine_ms << ",\n"
            << "  \"book_quarantine_events\": "
            << stats.book_quarantine_events.load() << ",\n"
            << "  \"depth_updates_quarantined\": "
            << stats.depth_updates_quarantined.load() << ",\n"
            << "  \"book_quarantine_active\": "
            << (runtime->book_quarantine_until_ns > now_ns()
                    ? "true"
                    : "false")
            << ",\n"
            << "  \"quote_intents\": " << stats.mm_quote_intents.load()
            << ",\n"
            << "  \"cancel_intents\": " << stats.mm_cancel_intents.load()
            << ",\n"
            << "  \"risk_approved\": " << stats.risk_approved.load()
            << ",\n"
            << "  \"risk_rejected\": " << stats.risk_rejected.load()
            << ",\n"
            << "  \"no_quote_reasons\": " << no_quote_reasons << ",\n"
            << "  \"risk_decisions\": " << risk_decisions << ",\n"
            << "  \"submitted_quotes\": " << stats.submitted_quotes.load()
            << ",\n"
            << "  \"cancelled_quotes\": " << stats.cancelled_quotes.load()
            << ",\n"
            << "  \"maker_reports\": " << stats.maker_reports.load()
            << ",\n"
            << "  \"maker_fills_applied\": "
            << stats.maker_fills_applied.load() << ",\n"
            << "  \"maker_fills_rejected\": "
            << stats.maker_fills_rejected.load() << ",\n"
            << "  \"decode_errors\": " << stats.decode_errors.load()
            << ",\n"
            << "  \"state_errors\": " << stats.state_errors.load()
            << ",\n"
            << "  \"transport_errors\": " << stats.transport_errors.load()
            << ",\n";
    }
    write_latency_json(out, "pipeline_latency_ns", pipeline_stats, false);
    out << "}\n";
    write_atomic(config.out_json, out.str());
}

void print_latency(const char* name, const LatencyStats& stats) {
    std::cout << "  " << name << ":\n"
              << "    count: " << stats.count << "\n"
              << "    min: " << stats.min << "\n"
              << "    p50: " << stats.p50 << "\n"
              << "    p90: " << stats.p90 << "\n"
              << "    p95: " << stats.p95 << "\n"
              << "    p99: " << stats.p99 << "\n"
              << "    max: " << stats.max << "\n"
              << "    mean: " << std::fixed << std::setprecision(2)
              << stats.mean << "\n";
}

void process_depth_update(
    const Config& config,
    const state::MarketDepthView& depth,
    const state::MarketDepthView* complement_depth,
    const BtcOracleSnapshot& oracle_snapshot,
    const mm::ExternalFairRuntime* sol_external_fair_runtime,
    const std::unordered_map<std::string, mm::ExternalFairMarketSpec>*
        sol_external_fair_specs_by_token_id,
    mm::InMemorySpotOracle* sol_spot_oracle,
    mm::FixedVolProvider* sol_vol_provider,
    mm_research::ExternalFairBasisLogger* external_fair_basis_logger,
    mm_research::CanonicalQuoteResearchLogger* canonical_quote_logger,
    mm::MarketMakingEngine* engine,
    risk::QuoteRiskEvaluator* risk_evaluator,
    const risk::QuoteRiskPolicy& policy,
    Stats* stats,
    RuntimeState* runtime
) {
    const auto now = now_ns();
    const auto unix_ms = current_unix_ms();
    if (sol_spot_oracle != nullptr &&
        config.sol_spot_feed == "manual" &&
        config.sol_spot_bid > 0.0 &&
        config.sol_spot_ask > 0.0) {
        sol_spot_oracle->update_sol_book_ticker(
            config.sol_spot_bid,
            config.sol_spot_ask,
            unix_ms,
            unix_ms
        );
    }
    if (sol_vol_provider != nullptr) {
        sol_vol_provider->update(config.sol_fixed_vol_annualized, unix_ms);
    }

    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->last_depth = depth;
    runtime->has_depth = true;
    if (complement_depth) {
        runtime->last_complement_depth = *complement_depth;
        runtime->has_complement_depth = true;
    }
    runtime->seed_initial_position(
        config,
        depth.asset_index,
        complement_depth ? complement_depth->asset_index : 0,
        complement_depth != nullptr
    );
    update_recent_fill_markouts(config, runtime, now);

    const auto crossed_now = locked_or_crossed_book(depth);
    const auto tte_ns = time_to_expiry_ns(config);
    const auto raw_external_fair =
        external_fair_runtime(config, tte_ns, oracle_snapshot);
    const auto external_fair =
        crossed_now
            ? apply_existing_basis(config, *runtime, raw_external_fair)
            : update_basis_and_apply(config, runtime, raw_external_fair, depth);
    stats->latest_external_fair_raw_tick.store(external_fair.raw_tick);
    stats->latest_external_fair_adjusted_tick.store(external_fair.tick);
    stats->latest_fair_basis_tick.store(external_fair.basis_tick);
    update_ewma_fair(config, runtime, external_fair.tick);
    update_ewma_basis_yes(config, runtime, depth, external_fair.tick);
    stats->latest_ewma_fair_yes_tick.store(runtime->ewma_fair_yes_tick);
    stats->latest_ewma_basis_yes_tick.store(
        static_cast<std::int64_t>(std::llround(runtime->ewma_basis_yes_tick))
    );
    const auto dynamic_quote =
        dynamic_quote_runtime(config, external_fair, oracle_snapshot);
    const auto bid_momentum_shutoff =
        momentum_bid_shutoff(config, oracle_snapshot);
    if (bid_momentum_shutoff) {
        stats->momentum_bid_shutoffs.fetch_add(1);
    }

    if (crossed_now && config.book_quarantine_ms > 0) {
        runtime->book_quarantine_until_ns = std::max<std::uint64_t>(
            runtime->book_quarantine_until_ns,
            now + config.book_quarantine_ms * 1'000'000ULL
        );
        stats->book_quarantine_events.fetch_add(1);
    }
    const auto quarantine_active =
        config.book_quarantine_ms > 0 &&
        runtime->book_quarantine_until_ns > now;
    if (quarantine_active) {
        stats->depth_updates_quarantined.fetch_add(1);
        const auto pre_taker_yes_position =
            runtime->ledger.position_ledger().lots(config.asset_id);
        const auto locked_book_yes =
            evaluate_locked_book_taker_buy_for_asset(
                config,
                depth,
                external_fair.tick,
                oracle_snapshot,
                bid_momentum_shutoff,
                dynamic_quote.toxic_bid,
                pre_taker_yes_position,
                false
            );
        LockedBookTakerOpportunity locked_book_no;
        if (complement_depth != nullptr) {
            const auto fair_no_tick = complement_fair_tick(external_fair.tick);
            const auto pre_taker_no_position =
                runtime->ledger.position_ledger().lots(
                    config.complement_asset_id
                );
            locked_book_no = evaluate_locked_book_taker_buy_for_asset(
                config,
                *complement_depth,
                fair_no_tick,
                oracle_snapshot,
                bid_momentum_shutoff,
                dynamic_quote.toxic_bid,
                pre_taker_no_position,
                true
            );
        }
        const auto locked_book_taker =
            better_taker_opportunity(locked_book_yes, locked_book_no);
        const auto& locked_book_depth =
            locked_book_taker.complement_asset && complement_depth != nullptr
                ? *complement_depth
                : depth;
        record_taker_mid_slippage_block(locked_book_taker, 1, stats);
        (void)apply_taker_ioc_buy(
            config,
            locked_book_depth,
            locked_book_taker,
            1,
            locked_book_taker.complement_asset
                ? "locked_book_taker_ioc_buy_no"
                : "locked_book_taker_ioc_buy_yes",
            stats,
            runtime,
            now
        );

        state::MarketDepthView quarantine_depth = depth;
        if (crossed_now) {
            quarantine_depth.crossed = true;
        } else {
            quarantine_depth.recovering = true;
        }

        const auto current_position =
            runtime->ledger.position_ledger().lots(config.asset_id);
        const auto mm_result = engine->on_market_update(mm::MarketMakingInput{
            .market_id = config.market_id,
            .asset_id = config.asset_id,
            .market_index = 1,
            .asset_index = depth.asset_index,
            .depth = &quarantine_depth,
            .complement_depth = complement_depth,
            .current_position_lots = current_position,
            .external_fair_value_tick = 0,
            .disable_bid_quotes = true,
            .disable_ask_quotes = true,
            .now_ns = now,
            .time_to_expiry_ns = tte_ns
        });

        stats->mm_quote_intents.fetch_add(mm_result.quote_count);
        stats->mm_cancel_intents.fetch_add(mm_result.cancel_count);
        stats->mm_rejected_no_quote.fetch_add(mm_result.rejected_no_quote);
        stats->latest_fair_value_quality.store(
            static_cast<std::uint64_t>(mm_result.fair_value_quality)
        );
        stats->latest_fair_confidence_bps.store(
            mm_result.fair_confidence_bps
        );
        stats->latest_fair_book_spread_tick.store(
            mm_result.fair_book_spread_tick
        );
        stats->latest_fair_value_tick.store(mm_result.fair_value_tick);
        if (mm_result.rejected_no_quote > 0) {
            observe_no_quote_reason(stats, mm_result.no_quote_reason);
        }

        for (std::uint16_t i = 0; i < mm_result.cancel_count; ++i) {
            const auto cancelled =
                runtime->execution_adapter.cancel_quote_group(
                    mm_result.cancels[i].quote_group_id,
                    now
                );
            if (cancelled.ok) {
                stats->cancelled_quotes.fetch_add(1);
            } else {
                stats->cancel_errors.fetch_add(1);
            }
        }
        if (config.pure_taker_mode) {
            update_reduce_quote(
                config,
                &depth,
                config.asset_id,
                external_fair.tick,
                false,
                &runtime->yes_reduce_quote,
                stats,
                runtime,
                now
            );
        }
        if (complement_depth != nullptr) {
            const execution::PaperMakerMarketEvent complement_event{
                .ts_ns = now,
                .asset_index = complement_depth->asset_index,
                .depth = complement_depth
            };
            consume_maker_reports(
                config,
                engine,
                stats,
                runtime,
                runtime->execution_adapter.on_market_event(complement_event)
            );
            update_reduce_quote(
                config,
                complement_depth,
                config.complement_asset_id,
                complement_fair_tick(external_fair.tick),
                true,
                &runtime->no_reduce_quote,
                stats,
                runtime,
                now
            );
        }
        update_pnl_snapshot_unlocked(config, runtime, depth, now);
        return;
    }

    const execution::PaperMakerMarketEvent market_event{
        .ts_ns = now,
        .asset_index = depth.asset_index,
        .depth = &depth
    };
    consume_maker_reports(
        config,
        engine,
        stats,
        runtime,
        runtime->execution_adapter.on_market_event(market_event)
    );
    if (complement_depth != nullptr) {
        const execution::PaperMakerMarketEvent complement_event{
            .ts_ns = now,
            .asset_index = complement_depth->asset_index,
            .depth = complement_depth
        };
        consume_maker_reports(
            config,
            engine,
            stats,
            runtime,
            runtime->execution_adapter.on_market_event(complement_event)
        );
    }

    const auto current_position =
        runtime->ledger.position_ledger().lots(config.asset_id);
    const auto macro_divergence =
        evaluate_lead_lag_sniping(
            config,
            oracle_snapshot,
            runtime->ewma_fair_yes_tick,
            depth,
            complement_depth,
            runtime,
            stats
        );
    if (macro_divergence.active) {
        runtime->last_macro_divergence_signal_ns = now;
        stats->macro_divergence_sniping_signals.fetch_add(1);
        stats->latest_lead_lag_side.store(macro_divergence.side);
        stats->latest_lead_lag_edge_tick.store(macro_divergence.edge_tick);
        stats->latest_lead_lag_price_tick.store(macro_divergence.price_tick);
        const auto& macro_depth =
            macro_divergence.complement_asset && complement_depth != nullptr
                ? *complement_depth
                : depth;
        const auto macro_position =
            runtime->ledger.position_ledger().lots(
                macro_divergence.complement_asset
                    ? config.complement_asset_id
                    : config.asset_id
            );
        const auto macro_taker =
            evaluate_lead_lag_taker_buy(
                config,
                macro_depth,
                macro_divergence,
                macro_position
            );
        record_taker_mid_slippage_block(macro_taker, 3, stats);
        (void)apply_taker_ioc_buy(
            config,
            macro_depth,
            macro_taker,
            3,
            macro_taker.complement_asset
                ? "macro_divergence_taker_ioc_buy_no"
                : "macro_divergence_taker_ioc_buy_yes",
            stats,
            runtime,
            now
        );
    }
    if (config.pure_taker_mode) {
        update_reduce_quote(
            config,
            &depth,
            config.asset_id,
            external_fair.tick,
            false,
            &runtime->yes_reduce_quote,
            stats,
            runtime,
            now
        );
    }
    update_reduce_quote(
        config,
        complement_depth,
        config.complement_asset_id,
        complement_fair_tick(external_fair.tick),
        true,
        &runtime->no_reduce_quote,
        stats,
        runtime,
        now
    );
    const auto post_taker_position =
        runtime->ledger.position_ledger().lots(config.asset_id);
    auto market_making_external_fair_tick = external_fair.tick;
    const mm::ExternalFairMarketSpec* active_external_fair_spec = nullptr;
    mm::ExternalFairResult active_external_fair_result;
    if (sol_external_fair_runtime != nullptr &&
        sol_external_fair_specs_by_token_id != nullptr) {
        const auto spec_it =
            sol_external_fair_specs_by_token_id->find(config.asset_id);
        if (spec_it != sol_external_fair_specs_by_token_id->end()) {
            active_external_fair_spec = &spec_it->second;
            active_external_fair_result =
                sol_external_fair_runtime->compute(spec_it->second, unix_ms);
            if (active_external_fair_result.ok) {
                market_making_external_fair_tick =
                    active_external_fair_result.fair_value_tick;
            } else if (config.require_external_fair_for_opening_quotes) {
                market_making_external_fair_tick = 0;
            }
        }
    }
    mm::CanonicalMarketState canonical_state;
    mm::ExternalFairOutput external_fair_output;
    mm::MarketImpliedFairOutput market_implied_output;
    mm::TradableFairOutput tradable_fair_output;
    mm::InventoryTargetOutput inventory_target;
    std::int64_t canonical_yes_position_lots = 0;
    std::int64_t target_asset_position_lots = config.target_position_lots;
    std::int64_t dynamic_min_asset_lots = config.min_inventory_lots;
    std::int64_t dynamic_max_asset_lots = config.max_inventory_lots;
    std::int64_t spot_age_for_risk_ms = 0;
    std::int64_t vol_age_for_risk_ms = 0;
    std::int64_t external_confidence_for_risk_bps = 10'000;
    if (active_external_fair_spec != nullptr &&
        depth.bid_count > 0 &&
        depth.ask_count > 0 &&
        depth.bids[0].price_tick > 0 &&
        depth.asks[0].price_tick > 0 &&
        depth.asks[0].price_tick >= depth.bids[0].price_tick) {
        const auto best_bid_tick = depth.bids[0].price_tick;
        const auto best_ask_tick = depth.asks[0].price_tick;
        const auto canonical_book = mm::canonical_yes_top_of_book(
            active_external_fair_spec->outcome_side,
            best_bid_tick,
            best_ask_tick,
            active_external_fair_spec->price_scale_tick
        );
        const auto spot_snapshot =
            sol_spot_oracle != nullptr
                ? sol_spot_oracle->latest(active_external_fair_spec->symbol)
                : mm::SpotSnapshot{};
        const auto vol_snapshot =
            sol_vol_provider != nullptr
                ? sol_vol_provider->latest(active_external_fair_spec->symbol)
                : mm::VolSnapshot{};
        canonical_yes_position_lots = mm::to_canonical_yes_exposure(
            active_external_fair_spec->outcome_side,
            post_taker_position
        );
        canonical_state.market_id = active_external_fair_spec->market_id;
        canonical_state.token_id = active_external_fair_spec->token_id;
        canonical_state.complement_token_id = config.complement_asset_id;
        canonical_state.event_type = active_external_fair_spec->event_type;
        canonical_state.asset_side = active_external_fair_spec->outcome_side;
        canonical_state.book_bid_tick = best_bid_tick;
        canonical_state.book_ask_tick = best_ask_tick;
        canonical_state.book_mid_tick = (best_bid_tick + best_ask_tick) / 2;
        canonical_state.spread_tick = best_ask_tick - best_bid_tick;
        canonical_state.canonical_yes_bid_tick = canonical_book.bid_tick;
        canonical_state.canonical_yes_ask_tick = canonical_book.ask_tick;
        canonical_state.canonical_yes_mid_tick = canonical_book.mid_tick;
        canonical_state.asset_mid_tick = canonical_state.book_mid_tick;
        canonical_state.spot = spot_snapshot.spot;
        canonical_state.annualized_vol = vol_snapshot.annualized_vol;
        canonical_state.tte_ns = static_cast<std::int64_t>(tte_ns);
        canonical_state.book_age_ms =
            depth.last_ws_recv_ns > 0 && now > depth.last_ws_recv_ns
                ? static_cast<std::int64_t>(
                      (now - depth.last_ws_recv_ns) / 1'000'000ULL
                  )
                : 0;
        canonical_state.spot_age_ms = spot_snapshot.local_recv_ts_ms > 0
            ? unix_ms - spot_snapshot.local_recv_ts_ms
            : 0;
        canonical_state.vol_age_ms = vol_snapshot.update_ts_ms > 0
            ? unix_ms - vol_snapshot.update_ts_ms
            : 0;
        canonical_state.current_asset_position_lots = post_taker_position;
        canonical_state.current_canonical_yes_position_lots =
            canonical_yes_position_lots;

        mm::ExternalFairModel external_fair_model;
        external_fair_output = external_fair_model.compute(
            *sol_external_fair_runtime,
            *active_external_fair_spec,
            unix_ms,
            spot_snapshot,
            vol_snapshot
        );
        mm::MarketImpliedFairModel market_implied_model;
        market_implied_output = market_implied_model.compute(canonical_state);
        mm::TradableFairBuilder tradable_fair_builder;
        tradable_fair_output = tradable_fair_builder.build(mm::TradableFairInput{
            .asset_side = active_external_fair_spec->outcome_side,
            .price_scale_tick = active_external_fair_spec->price_scale_tick,
            .lambda = config.external_fair_tradable_lambda,
            .shadow_only = config.external_fair_shadow_only,
            .external = external_fair_output,
            .market = market_implied_output
        });
        if (tradable_fair_output.ok) {
            canonical_state.asset_external_fair_tick =
                external_fair_output.asset_raw_fair_tick;
            canonical_state.asset_tradable_fair_tick =
                tradable_fair_output.asset_tradable_fair_tick;
            canonical_state.canonical_yes_external_fair_tick =
                external_fair_output.canonical_yes_raw_fair_tick;
            canonical_state.canonical_yes_tradable_fair_tick =
                tradable_fair_output.canonical_yes_tradable_fair_tick;
            market_making_external_fair_tick =
                tradable_fair_output.asset_tradable_fair_tick;
        }
        if (config.dynamic_inventory_targeter_enabled &&
            tradable_fair_output.ok) {
            mm::DynamicInventoryTargeter targeter;
            const auto top_depth =
                state::depth_prefix_level_size_lots(depth.bids[0]) +
                state::depth_prefix_level_size_lots(depth.asks[0]);
            inventory_target = targeter.compute(mm::InventoryTargetInput{
                .event_type = active_external_fair_spec->event_type,
                .canonical_yes_market_mid_tick =
                    market_implied_output.canonical_yes_market_mid_tick,
                .canonical_yes_external_fair_tick =
                    external_fair_output.canonical_yes_raw_fair_tick,
                .canonical_yes_tradable_fair_tick =
                    tradable_fair_output.canonical_yes_tradable_fair_tick,
                .spread_tick = canonical_state.spread_tick,
                .book_depth_lots = top_depth,
                .tte_ns = canonical_state.tte_ns,
                .confidence_bps = tradable_fair_output.confidence_bps,
                .implied_vol = market_implied_output.implied_vol,
                .realized_vol = 0.0,
                .current_canonical_yes_position_lots =
                    canonical_yes_position_lots,
                .portfolio_touch_exposure_lots =
                    std::llabs(canonical_yes_position_lots)
            });
            target_asset_position_lots =
                mm::canonical_yes_target_to_asset_position(
                    active_external_fair_spec->outcome_side,
                    inventory_target.target_canonical_yes_lots
                );
            dynamic_min_asset_lots =
                mm::canonical_yes_target_to_asset_position(
                    active_external_fair_spec->outcome_side,
                    inventory_target.max_canonical_yes_lots
                );
            dynamic_max_asset_lots =
                mm::canonical_yes_target_to_asset_position(
                    active_external_fair_spec->outcome_side,
                    inventory_target.min_canonical_yes_lots
                );
            if (dynamic_min_asset_lots > dynamic_max_asset_lots) {
                std::swap(dynamic_min_asset_lots, dynamic_max_asset_lots);
            }
        }
        spot_age_for_risk_ms = canonical_state.spot_age_ms;
        vol_age_for_risk_ms = canonical_state.vol_age_ms;
        external_confidence_for_risk_bps = tradable_fair_output.confidence_bps;
    }
    if (external_fair_basis_logger != nullptr &&
        active_external_fair_spec != nullptr &&
        active_external_fair_result.ok &&
        depth.bid_count > 0 &&
        depth.ask_count > 0) {
        const auto best_bid_tick = depth.bids[0].price_tick;
        const auto best_ask_tick = depth.asks[0].price_tick;
        const auto bid_size =
            state::depth_prefix_level_size_lots(depth.bids[0]);
        const auto ask_size =
            state::depth_prefix_level_size_lots(depth.asks[0]);
        const auto size_sum = bid_size + ask_size;
        const auto external_tick = active_external_fair_result.fair_value_tick;
        if (best_bid_tick > 0 &&
            best_ask_tick > 0 &&
            best_ask_tick >= best_bid_tick &&
            size_sum > 0 &&
            external_tick >= 0 &&
            external_tick <= active_external_fair_spec->price_scale_tick) {
            const auto book_mid_tick = (best_bid_tick + best_ask_tick) / 2;
            const auto book_micro_tick = static_cast<std::int64_t>(
                (static_cast<__int128>(best_ask_tick) * bid_size +
                 static_cast<__int128>(best_bid_tick) * ask_size) /
                size_sum
            );
            const auto spread_tick = best_ask_tick - best_bid_tick;
            const auto spot_snapshot =
                sol_spot_oracle != nullptr
                    ? sol_spot_oracle->latest(active_external_fair_spec->symbol)
                    : mm::SpotSnapshot{};
            const auto vol_snapshot =
                sol_vol_provider != nullptr
                    ? sol_vol_provider->latest(active_external_fair_spec->symbol)
                    : mm::VolSnapshot{};
            const auto tte_ms =
                active_external_fair_spec->expiry_unix_ms - unix_ms;
            const auto spot_age_ms = spot_snapshot.local_recv_ts_ms > 0
                ? unix_ms - spot_snapshot.local_recv_ts_ms
                : 0;
            if (spot_age_ms <= 1500) {
            const auto tte_years = static_cast<double>(tte_ms) /
                (1000.0 * kSecondsPerYear);
            external_fair_basis_logger->log(
                mm_research::ExternalFairBasisSnapshot{
                    .ts_ms = unix_ms,
                    .market_id = active_external_fair_spec->market_id,
                    .token_id = active_external_fair_spec->token_id,
                    .symbol = active_external_fair_spec->symbol,
                    .event_type = active_external_fair_spec->event_type,
                    .barrier_price = active_external_fair_spec->barrier_price,
                    .outcome_side = active_external_fair_spec->outcome_side,
                    .spot = spot_snapshot.spot,
                    .annualized_vol = vol_snapshot.annualized_vol,
                    .tte_years = tte_years,
                    .external_fair_tick = external_tick,
                    .yes_probability =
                        active_external_fair_result.yes_probability,
                    .best_bid_tick = best_bid_tick,
                    .best_ask_tick = best_ask_tick,
                    .bid_size = bid_size,
                    .ask_size = ask_size,
                    .book_mid_tick = book_mid_tick,
                    .book_micro_tick = book_micro_tick,
                    .spread_tick = spread_tick,
                    .mid_basis_tick = book_mid_tick - external_tick,
                    .micro_basis_tick = book_micro_tick - external_tick,
                    .buy_edge_tick = external_tick - best_ask_tick,
                    .sell_edge_tick = best_bid_tick - external_tick,
                    .book_age_ms =
                        depth.last_ws_recv_ns > 0 && now > depth.last_ws_recv_ns
                            ? static_cast<std::int64_t>(
                                  (now - depth.last_ws_recv_ns) / 1'000'000ULL
                              )
                            : 0,
                    .spot_age_ms = spot_age_ms,
                    .vol_age_ms = vol_snapshot.update_ts_ms > 0
                        ? unix_ms - vol_snapshot.update_ts_ms
                        : 0,
                    .spot_source = config.sol_spot_feed
                }
            );
            }
        }
    }
    const auto mm_result = engine->on_market_update(mm::MarketMakingInput{
        .market_id = config.market_id,
        .asset_id = config.asset_id,
        .market_index = 1,
        .asset_index = depth.asset_index,
        .depth = &depth,
        .complement_depth = complement_depth,
        .current_position_lots = post_taker_position,
        .external_fair_value_tick = market_making_external_fair_tick,
        .dynamic_target_position_lots = target_asset_position_lots,
        .dynamic_min_inventory_lots = dynamic_min_asset_lots,
        .dynamic_max_inventory_lots = dynamic_max_asset_lots,
        .canonical_yes_fair_value_tick =
            tradable_fair_output.canonical_yes_tradable_fair_tick,
        .canonical_yes_position_lots = canonical_yes_position_lots,
        .target_canonical_yes_lots =
            inventory_target.target_canonical_yes_lots,
        .dynamic_half_spread_tick =
            dynamic_quote.half_spread_tick,
        .dynamic_max_inventory_skew_tick =
            dynamic_quote.max_inventory_skew_tick,
        .disable_bid_quotes = config.pure_taker_mode ||
                              dynamic_quote.toxic_bid ||
                              bid_momentum_shutoff,
        .disable_ask_quotes = dynamic_quote.toxic_ask ||
                              config.pure_taker_mode,
        .now_ns = now,
        .time_to_expiry_ns = tte_ns
    });

    stats->mm_quote_intents.fetch_add(mm_result.quote_count);
    stats->mm_cancel_intents.fetch_add(mm_result.cancel_count);
    stats->mm_rejected_no_quote.fetch_add(mm_result.rejected_no_quote);
    stats->latest_fair_value_quality.store(
        static_cast<std::uint64_t>(mm_result.fair_value_quality)
    );
    stats->latest_fair_confidence_bps.store(mm_result.fair_confidence_bps);
    stats->latest_fair_book_spread_tick.store(
        mm_result.fair_book_spread_tick
    );
    stats->latest_fair_value_tick.store(mm_result.fair_value_tick);
    if (mm_result.rejected_no_quote > 0) {
        observe_no_quote_reason(stats, mm_result.no_quote_reason);
        if (canonical_quote_logger != nullptr && tradable_fair_output.ok) {
            canonical_quote_logger->log(mm_research::CanonicalQuoteResearchRow{
                .ts_ms = unix_ms,
                .state = canonical_state,
                .canonical_yes_market_mid_tick =
                    market_implied_output.canonical_yes_market_mid_tick,
                .canonical_yes_external_raw_tick =
                    external_fair_output.canonical_yes_raw_fair_tick,
                .canonical_yes_tradable_fair_tick =
                    tradable_fair_output.canonical_yes_tradable_fair_tick,
                .asset_external_raw_tick =
                    external_fair_output.asset_raw_fair_tick,
                .asset_tradable_fair_tick =
                    tradable_fair_output.asset_tradable_fair_tick,
                .basis_raw_tick =
                    external_fair_output.canonical_yes_raw_fair_tick -
                    market_implied_output.canonical_yes_market_mid_tick,
                .basis_tradable_tick =
                    tradable_fair_output.canonical_yes_tradable_fair_tick -
                    market_implied_output.canonical_yes_market_mid_tick,
                .buy_edge_tick =
                    tradable_fair_output.asset_tradable_fair_tick -
                    canonical_state.book_ask_tick,
                .sell_edge_tick =
                    canonical_state.book_bid_tick -
                    tradable_fair_output.asset_tradable_fair_tick,
                .current_inventory_asset = post_taker_position,
                .current_inventory_canonical_yes = canonical_yes_position_lots,
                .target_inventory_canonical_yes =
                    inventory_target.target_canonical_yes_lots,
                .portfolio_touch_exposure =
                    std::llabs(canonical_yes_position_lots),
                .quote_side = "none",
                .quote_reason =
                    mm::no_quote_reason_name(mm_result.no_quote_reason),
                .risk_decision = "no_quote",
                .risk_reject_reason =
                    mm::no_quote_reason_name(mm_result.no_quote_reason)
            });
        }
    }

    for (std::uint16_t i = 0; i < mm_result.cancel_count; ++i) {
        const auto cancelled =
            runtime->execution_adapter.cancel_quote_group(
                mm_result.cancels[i].quote_group_id,
                now
            );
        if (cancelled.ok) {
            stats->cancelled_quotes.fetch_add(1);
        } else {
            stats->cancel_errors.fetch_add(1);
        }
    }

    for (std::uint16_t i = 0; i < mm_result.quote_count; ++i) {
        const auto& quote = mm_result.quotes[i];
        stats->risk_evaluated.fetch_add(1);
        const auto risk_result = risk_evaluator->evaluate(risk::QuoteRiskInput{
            .quote = &quote,
            .depth = &depth,
            .policy = &policy,
            .current_position_lots = post_taker_position,
            .current_asset_exposure_tick = 0,
            .current_canonical_yes_position_lots = canonical_yes_position_lots,
            .projected_canonical_yes_position_lots =
                quote.target_canonical_yes_lots != 0
                    ? quote.target_canonical_yes_lots
                    : canonical_yes_position_lots,
            .portfolio_touch_exposure_lots =
                std::llabs(canonical_yes_position_lots),
            .spot_age_ms = spot_age_for_risk_ms,
            .vol_age_ms = vol_age_for_risk_ms,
            .external_confidence_bps = external_confidence_for_risk_bps,
            .active_quotes_for_asset = static_cast<std::uint32_t>(
                runtime->execution_adapter.quote_book().active_quote_count()
            ),
            .last_replace_ts_ns = 0,
            .now_ns = now
        });
        observe_risk_decision(stats, risk_result.decision.decision);
        if (canonical_quote_logger != nullptr && tradable_fair_output.ok) {
            auto log_quote_side = std::string{"both"};
            auto log_quote_price = std::int64_t{0};
            auto log_quote_size = std::int64_t{0};
            if (quote.has_bid && !quote.has_ask) {
                log_quote_side = "bid";
                log_quote_price = quote.bid.price_tick;
                log_quote_size = quote.bid.quantity_lots;
            } else if (quote.has_ask && !quote.has_bid) {
                log_quote_side = "ask";
                log_quote_price = quote.ask.price_tick;
                log_quote_size = quote.ask.quantity_lots;
            } else if (quote.has_bid && quote.has_ask) {
                log_quote_price = (quote.bid.price_tick + quote.ask.price_tick) / 2;
                log_quote_size =
                    quote.bid.quantity_lots + quote.ask.quantity_lots;
            }
            canonical_quote_logger->log(mm_research::CanonicalQuoteResearchRow{
                .ts_ms = unix_ms,
                .state = canonical_state,
                .canonical_yes_market_mid_tick =
                    market_implied_output.canonical_yes_market_mid_tick,
                .canonical_yes_external_raw_tick =
                    external_fair_output.canonical_yes_raw_fair_tick,
                .canonical_yes_tradable_fair_tick =
                    tradable_fair_output.canonical_yes_tradable_fair_tick,
                .asset_external_raw_tick =
                    external_fair_output.asset_raw_fair_tick,
                .asset_tradable_fair_tick =
                    tradable_fair_output.asset_tradable_fair_tick,
                .basis_raw_tick =
                    external_fair_output.canonical_yes_raw_fair_tick -
                    market_implied_output.canonical_yes_market_mid_tick,
                .basis_tradable_tick =
                    tradable_fair_output.canonical_yes_tradable_fair_tick -
                    market_implied_output.canonical_yes_market_mid_tick,
                .buy_edge_tick =
                    tradable_fair_output.asset_tradable_fair_tick -
                    canonical_state.book_ask_tick,
                .sell_edge_tick =
                    canonical_state.book_bid_tick -
                    tradable_fair_output.asset_tradable_fair_tick,
                .current_inventory_asset = post_taker_position,
                .current_inventory_canonical_yes = canonical_yes_position_lots,
                .target_inventory_canonical_yes =
                    quote.target_canonical_yes_lots,
                .portfolio_touch_exposure =
                    std::llabs(canonical_yes_position_lots),
                .quote_side = log_quote_side,
                .quote_price_tick = log_quote_price,
                .quote_size_lots = log_quote_size,
                .quote_reason = quote.reason,
                .risk_decision = risk::quote_risk_decision_type_name(
                    risk_result.decision.decision
                ),
                .risk_reject_reason = risk_result.decision.reason
            });
        }
        if (!risk_result.approved_quote) {
            (void)engine->remove_active_quote(quote.asset_index);
            stats->risk_rejected.fetch_add(1);
            continue;
        }

        stats->risk_approved.fetch_add(1);
        const auto submitted =
            runtime->execution_adapter.submit_approved_quote(
                *risk_result.approved_quote,
                now
            );
        if (submitted.ok) {
            stats->submitted_quotes.fetch_add(1);
            if (submitted.replaced) {
                stats->replaced_quotes.fetch_add(1);
            }
            if (submitted.duplicate_ignored) {
                stats->duplicate_ignored.fetch_add(1);
            }
        } else {
            (void)engine->remove_active_quote(quote.asset_index);
            stats->submit_errors.fetch_add(1);
        }
    }

    std::array<state::MarketDepthView, 2> pnl_depths{depth, {}};
    std::span<const state::MarketDepthView> pnl_span{pnl_depths.data(), 1};
    if (runtime->has_complement_depth) {
        pnl_depths[1] = runtime->last_complement_depth;
        pnl_span = std::span<const state::MarketDepthView>{
            pnl_depths.data(),
            2
        };
    }
    runtime->pnl = runtime->pnl_engine.compute(runtime->ledger, pnl_span, now);
    update_drawdown(runtime);
}

int run(const Config& config) {
    decode::DecodePipeline pipeline;
    state::MarketStateStore store;
    state::MarketStateView view(store);
    mm::MarketMakingEngine mm_engine(market_making_config(config));
    risk::QuoteRiskEvaluator risk_evaluator;
    const auto policy = quote_risk_policy(config);
    RuntimeState runtime(config);
    BtcOracleState btc_oracle;
    mm::InMemorySpotOracle sol_spot_oracle;
    const auto startup_unix_ms = current_unix_ms();
    if (config.sol_spot_feed == "manual" &&
        config.sol_spot_bid > 0.0 &&
        config.sol_spot_ask > 0.0) {
        sol_spot_oracle.update_sol_book_ticker(
            config.sol_spot_bid,
            config.sol_spot_ask,
            startup_unix_ms,
            startup_unix_ms
        );
    }
    mm::FixedVolProvider sol_vol_provider(
        config.sol_fixed_vol_annualized,
        startup_unix_ms
    );
    mm::ExternalFairRuntime sol_external_fair_runtime(
        sol_spot_oracle,
        sol_vol_provider
    );
    std::unordered_map<std::string, mm::ExternalFairMarketSpec>
        sol_external_fair_specs_by_token_id;
    const auto asset_outcome_side =
        parse_outcome_side(config.sol_external_fair_outcome_side);
    mm::ExternalFairMarketSpec asset_spec;
    if (populate_sol_external_fair_spec(
            config,
            config.asset_id,
            asset_outcome_side,
            &asset_spec
        )) {
        sol_external_fair_specs_by_token_id.emplace(
            asset_spec.token_id,
            asset_spec
        );
    }
    mm::ExternalFairMarketSpec complement_spec;
    if (populate_sol_external_fair_spec(
            config,
            config.complement_asset_id,
            opposite_outcome_side(asset_outcome_side),
            &complement_spec
        )) {
        sol_external_fair_specs_by_token_id.emplace(
            complement_spec.token_id,
            complement_spec
        );
    }

    Stats stats;
    std::mutex state_mutex;
    std::atomic<bool> fatal{false};
    std::atomic<std::uint64_t> next_packet_id{1};
    std::vector<std::uint64_t> pipeline_latencies_ns;

    feed::WebSocketClient market_ws(config.endpoint);
    std::unique_ptr<feed::WebSocketClient> btc_ws;
    std::unique_ptr<feed::WebSocketClient> sol_spot_ws;
    std::thread btc_thread;
    std::thread sol_spot_thread;
    std::unique_ptr<mm_research::ExternalFairBasisLogger>
        external_fair_basis_logger;
    std::unique_ptr<mm_research::CanonicalQuoteResearchLogger>
        canonical_quote_logger;
    if (config.external_fair_basis_log) {
        external_fair_basis_logger =
            std::make_unique<mm_research::ExternalFairBasisLogger>(
                external_fair_basis_log_path(config)
            );
        if (!external_fair_basis_logger->ok()) {
            fail(
                "failed to open external fair basis log: " +
                external_fair_basis_logger->path().string()
            );
        }
    }
    if (config.canonical_quote_research_log) {
        canonical_quote_logger =
            std::make_unique<mm_research::CanonicalQuoteResearchLogger>(
                canonical_quote_research_log_path(config)
            );
        if (!canonical_quote_logger->ok()) {
            fail(
                "failed to open canonical quote research log: " +
                canonical_quote_logger->path().string()
            );
        }
    }

    market_ws.set_on_open([&]() {
        market_ws.send(
            market_subscription(config.asset_id, config.complement_asset_id)
        );
    });

    market_ws.set_on_message([&](const std::string& payload) {
        const auto started = Clock::now();
        if (is_pong(payload)) {
            stats.pong_received.fetch_add(1);
        }

        const auto packet = feed::make_raw_packet(
            feed::SourceId::PolymarketMarket,
            1,
            next_packet_id.fetch_add(1),
            payload,
            feed::Codec::None,
            is_pong(payload)
                ? static_cast<std::uint32_t>(feed::PacketHeartbeat)
                : static_cast<std::uint32_t>(feed::PacketNone)
        );

        decode::NormalizedEventBatch batch;
        const auto decoded_result = pipeline.decode(
            feed::to_decode_input_view(packet),
            &batch
        );
        stats.ws_packets.fetch_add(1);
        stats.normalized_events.fetch_add(
            static_cast<std::uint64_t>(batch.size())
        );
        if (!decoded_result.ok() &&
            decoded_result.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
            stats.decode_errors.fetch_add(1);
        }

        decode::NormalizedEventBatch asset_batch;
        for (const auto& event : batch.events) {
            if (targets_other_asset(event, config)) {
                stats.filtered_events.fetch_add(1);
                continue;
            }
            if (event.event_type == decode::NormalizedEventType::Snapshot) {
                stats.book_snapshots.fetch_add(1);
            } else if (event.event_type == decode::NormalizedEventType::Delta) {
                stats.book_deltas.fetch_add(1);
            }
            static_cast<void>(asset_batch.push_back(event));
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bool published = false;
            for (const auto& event : state::from_normalized_batch(asset_batch)) {
                const auto result = store.apply(event);
                if (!result.ok()) {
                    stats.state_errors.fetch_add(1);
                }
                if (result.snapshot_published) {
                    stats.snapshots_published.fetch_add(1);
                    published = true;
                }
            }

            if (published) {
                std::array<std::string_view, 2> asset_ids{
                    config.asset_id,
                    config.complement_asset_id
                };
                const auto asset_count =
                    config.complement_asset_id.empty() ? 1U : 2U;
                const auto depth_batch =
                    view.read_depth_batch_by_asset_id(
                        std::span<const std::string_view>{
                            asset_ids.data(),
                            asset_count
                        }
                    );
                if (depth_batch.ok && depth_batch.count > 0 &&
                    depth_batch.views[0].usable_for_depth) {
                    const auto* complement_depth =
                        depth_batch.count > 1 &&
                        depth_batch.views[1].usable_for_depth
                            ? &depth_batch.views[1]
                            : nullptr;
                    stats.depth_updates.fetch_add(1);
                    const auto oracle_snapshot =
                        btc_oracle.snapshot(config);
                    process_depth_update(
                        config,
                        depth_batch.views[0],
                        complement_depth,
                        oracle_snapshot,
                        &sol_external_fair_runtime,
                        &sol_external_fair_specs_by_token_id,
                        &sol_spot_oracle,
                        &sol_vol_provider,
                        external_fair_basis_logger.get(),
                        canonical_quote_logger.get(),
                        &mm_engine,
                        &risk_evaluator,
                        policy,
                        &stats,
                        &runtime
                    );
                }
            }
        }

        const auto latency = elapsed_ns(started, Clock::now());
        stats.latest_pipeline_latency_ns.store(latency);
        pipeline_latencies_ns.push_back(latency);
    });

    market_ws.set_on_error([&](const std::string&) {
        stats.transport_errors.fetch_add(1);
    });

    if (config.btc_oracle_enabled) {
        btc_ws = std::make_unique<feed::WebSocketClient>(
            config.btc_oracle_endpoint
        );
        btc_ws->set_on_message([&](const std::string& payload) {
            double price = 0.0;
            if (parse_binance_trade_price(payload, &price)) {
                btc_oracle.observe_trade(price);
            } else if (!is_pong(payload)) {
                btc_oracle.observe_parse_error();
            }
        });
        btc_ws->set_on_error([&](const std::string&) {
            btc_oracle.observe_transport_error();
        });
        try {
            btc_ws->connect();
            btc_thread = std::thread([&]() {
                try {
                    btc_ws->run();
                } catch (...) {
                    btc_oracle.observe_transport_error();
                }
            });
        } catch (const std::exception& error) {
            btc_oracle.observe_transport_error();
            std::cerr << "btc_oracle_connect_error: "
                      << error.what() << '\n';
        }
    }

    if (config.sol_spot_feed == "binance_book_ticker") {
        sol_spot_ws = std::make_unique<feed::WebSocketClient>(
            config.sol_spot_feed_endpoint
        );
        sol_spot_ws->set_on_message([&](const std::string& payload) {
            double bid = 0.0;
            double ask = 0.0;
            std::int64_t exchange_ts_ms = 0;
            if (parse_binance_book_ticker(
                    payload,
                    &bid,
                    &ask,
                    &exchange_ts_ms
                ) &&
                sol_book_ticker_spread_ok(
                    bid,
                    ask,
                    config.sol_spot_max_spread_bps
                )) {
                const auto recv_ts_ms = current_unix_ms();
                if (exchange_ts_ms <= 0) {
                    exchange_ts_ms = recv_ts_ms;
                }
                sol_spot_oracle.update_sol_book_ticker(
                    bid,
                    ask,
                    exchange_ts_ms,
                    recv_ts_ms
                );
            }
        });
        sol_spot_ws->set_on_error([&](const std::string&) {
            stats.transport_errors.fetch_add(1);
        });
        try {
            sol_spot_ws->connect();
            sol_spot_thread = std::thread([&]() {
                try {
                    sol_spot_ws->run();
                } catch (...) {
                    stats.transport_errors.fetch_add(1);
                }
            });
        } catch (const std::exception& error) {
            stats.transport_errors.fetch_add(1);
            std::cerr << "sol_spot_feed_connect_error: "
                      << error.what() << '\n';
        }
    }

    const auto start_ns = now_ns();
    write_dashboard_snapshot(
        config,
        &stats,
        &runtime,
        0,
        btc_oracle.snapshot(config)
    );
    try {
        market_ws.connect();
    } catch (...) {
        if (btc_ws) {
            btc_ws->disconnect();
        }
        if (sol_spot_ws) {
            sol_spot_ws->disconnect();
        }
        if (btc_thread.joinable()) {
            btc_thread.join();
        }
        if (sol_spot_thread.joinable()) {
            sol_spot_thread.join();
        }
        throw;
    }
    std::thread market_thread([&]() {
        try {
            market_ws.run();
        } catch (...) {
            stats.transport_errors.fetch_add(1);
            fatal.store(true);
        }
    });

    const auto deadline_ns = start_ns + config.seconds * kNsPerSecond;
    std::uint64_t next_ping_ns = start_ns;
    std::uint64_t next_dashboard_ns = start_ns;
    while (now_ns() < deadline_ns && !fatal.load()) {
        const auto now = now_ns();
        if (now >= next_ping_ns) {
            if (market_ws.connected()) {
                market_ws.send("PING");
                stats.ping_sent.fetch_add(1);
            }
            next_ping_ns = now + config.ping_interval_ms * 1'000'000ULL;
        }
        if (now >= next_dashboard_ns) {
            write_dashboard_snapshot(
                config,
                &stats,
                &runtime,
                (now - start_ns) / kNsPerSecond,
                btc_oracle.snapshot(config)
            );
            next_dashboard_ns =
                now + config.dashboard_interval_ms * 1'000'000ULL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    market_ws.disconnect();
    if (market_thread.joinable()) {
        market_thread.join();
    }
    if (btc_ws) {
        btc_ws->disconnect();
    }
    if (sol_spot_ws) {
        sol_spot_ws->disconnect();
    }
    if (btc_thread.joinable()) {
        btc_thread.join();
    }
    if (sol_spot_thread.joinable()) {
        sol_spot_thread.join();
    }

    const auto runtime_seconds = (now_ns() - start_ns) / kNsPerSecond;
    const auto pipeline_stats = summarize(std::move(pipeline_latencies_ns));
    const auto final_oracle_snapshot = btc_oracle.snapshot(config);
    write_dashboard_snapshot(
        config,
        &stats,
        &runtime,
        runtime_seconds,
        final_oracle_snapshot
    );
    write_summary_json(
        config,
        stats,
        &runtime,
        runtime_seconds,
        pipeline_stats,
        final_oracle_snapshot
    );

    std::lock_guard<std::mutex> lock(runtime.mutex);
    const auto final_tte_ns = time_to_expiry_ns(config);
    const auto raw_final_external_fair =
        external_fair_runtime(config, final_tte_ns, final_oracle_snapshot);
    const auto final_external_fair =
        apply_existing_basis(config, runtime, raw_final_external_fair);
    const auto final_external_tick = final_external_fair.tick;
    const auto final_dynamic_quote =
        dynamic_quote_runtime(
            config,
            final_external_fair,
            final_oracle_snapshot
        );
    const auto no_quote_reasons = histogram_json(
        stats.no_quote_reasons,
        stats.no_quote_reasons.size(),
        [](std::size_t index) {
            return mm::no_quote_reason_name(
                static_cast<mm::NoQuoteReason>(index)
            );
        }
    );
    const auto latest_fair_quality = static_cast<mm::FairValueQuality>(
        stats.latest_fair_value_quality.load()
    );
    const auto risk_decisions = histogram_json(
        stats.risk_decisions,
        stats.risk_decisions.size(),
        [](std::size_t index) {
            return risk::quote_risk_decision_type_name(
                static_cast<risk::QuoteRiskDecisionType>(index)
            );
        }
    );
    const auto attribution_mark_tick =
        mark_tick_for_asset(config, runtime, config.asset_id);
    const auto attribution_complement_mark_tick =
        config.complement_asset_id.empty()
            ? 0
            : mark_tick_for_asset(config, runtime, config.complement_asset_id);
    const auto seed_unrealized_mid_tick = attributed_unrealized_tick(
        runtime.attribution.seed_position_lots,
        runtime.attribution.seed_cost_basis_tick,
        attribution_mark_tick
    );
    const auto seed_complement_unrealized_mid_tick =
        attributed_unrealized_tick(
            runtime.attribution.seed_complement_position_lots,
            runtime.attribution.seed_complement_cost_basis_tick,
            attribution_complement_mark_tick
        );
    const auto seed_total_pnl_mid_tick =
        runtime.attribution.seed_realized_pnl_tick +
        runtime.attribution.seed_complement_realized_pnl_tick +
        seed_unrealized_mid_tick +
        seed_complement_unrealized_mid_tick;
    const auto strategy_unrealized_mid_tick = attributed_unrealized_tick(
        runtime.attribution.strategy_position_lots,
        runtime.attribution.strategy_cost_basis_tick,
        attribution_mark_tick
    );
    const auto strategy_complement_unrealized_mid_tick =
        attributed_unrealized_tick(
            runtime.attribution.strategy_complement_position_lots,
            runtime.attribution.strategy_complement_cost_basis_tick,
            attribution_complement_mark_tick
        );
    const auto strategy_total_pnl_mid_tick =
        runtime.attribution.strategy_realized_pnl_tick +
        runtime.attribution.strategy_complement_realized_pnl_tick +
        strategy_unrealized_mid_tick +
        strategy_complement_unrealized_mid_tick;
    const auto final_bid_momentum_shutoff =
        momentum_bid_shutoff(config, final_oracle_snapshot);
    std::cout << "market_maker_dashboard_live:\n"
              << "  mode: read_only_live_market_maker\n"
              << "  fill_mode: " << fill_mode_name(config.fill_mode) << "\n"
              << "  pure_taker_mode: "
              << (config.pure_taker_mode ? "true" : "false") << "\n"
              << "  runtime_seconds: " << runtime_seconds << "\n"
              << "  asset_id: " << config.asset_id << "\n"
              << "  window_end_unix_seconds: "
              << config.window_end_unix_seconds << "\n"
              << "  time_to_expiry_ns: " << time_to_expiry_ns(config)
              << "\n"
              << "  external_fair_value_tick: " << final_external_tick
              << "\n"
              << "  external_fair_raw_tick: "
              << final_external_fair.raw_tick << "\n"
              << "  external_fair_basis_tick: "
              << final_external_fair.basis_tick << "\n"
              << "  fair_basis_updates: "
              << runtime.fair_basis_updates << "\n"
              << "  latest_fair_value_quality: "
              << mm::fair_value_quality_name(latest_fair_quality) << "\n"
              << "  latest_fair_confidence_bps: "
              << stats.latest_fair_confidence_bps.load() << "\n"
              << "  latest_fair_book_spread_tick: "
              << stats.latest_fair_book_spread_tick.load() << "\n"
              << "  latest_fair_value_tick: "
              << stats.latest_fair_value_tick.load() << "\n"
              << "  external_fair_weight_bps: "
              << config.external_fair_weight_bps << "\n"
              << "  require_external_fair_for_opening_quotes: "
              << (config.require_external_fair_for_opening_quotes ? "true"
                                                                  : "false")
              << "\n"
              << "  assumed_latency_ms: " << config.assumed_latency_ms << "\n"
              << "  min_quote_edge_tick: " << config.min_quote_edge_tick
              << "\n"
              << "  adverse_selection_buffer_tick: "
              << config.adverse_selection_buffer_tick << "\n"
              << "  latency_buffer_tick: " << latency_buffer_tick(config)
              << "\n"
              << "  min_requote_interval_ms: "
              << config.min_requote_interval_ms << "\n"
              << "  min_quote_price_change_tick: "
              << config.min_quote_price_change_tick << "\n"
              << "  min_book_spread_tick: "
              << config.min_book_spread_tick << "\n"
              << "  btc_oracle_enabled: "
              << (config.btc_oracle_enabled ? "true" : "false") << "\n"
              << "  btc_oracle_spot: "
              << final_oracle_snapshot.spot << "\n"
              << "  btc_oracle_age_ms: "
              << final_oracle_snapshot.latest_age_ms << "\n"
              << "  btc_move_1s_bps: "
              << final_oracle_snapshot.move_1s_bps << "\n"
              << "  btc_move_500ms_bps: "
              << final_oracle_snapshot.move_500ms_bps << "\n"
              << "  btc_use_realized_vol: "
              << (config.btc_use_realized_vol ? "true" : "false") << "\n"
              << "  btc_realized_vol_annual_bps: "
              << final_oracle_snapshot.realized_vol_annual_bps << "\n"
              << "  btc_toxic_bid: "
              << (final_dynamic_quote.toxic_bid ? "true" : "false")
              << "\n"
              << "  btc_toxic_ask: "
              << (final_dynamic_quote.toxic_ask ? "true" : "false")
              << "\n"
              << "  momentum_bid_shutoff_enabled: "
              << (config.momentum_bid_shutoff_enabled ? "true" : "false")
              << "\n"
              << "  momentum_bid_shutoff_active: "
              << (final_bid_momentum_shutoff ? "true" : "false") << "\n"
              << "  momentum_bid_shutoffs: "
              << stats.momentum_bid_shutoffs.load() << "\n"
              << "  btc_oracle_updates: "
              << final_oracle_snapshot.updates << "\n"
              << "  btc_oracle_parse_errors: "
              << final_oracle_snapshot.parse_errors << "\n"
              << "  btc_oracle_transport_errors: "
              << final_oracle_snapshot.transport_errors << "\n"
              << "  lead_lag_sniping_enabled: "
              << (config.lead_lag_sniping_enabled ? "true" : "false")
              << "\n"
              << "  macro_divergence_taker_enabled: "
              << (config.macro_divergence_taker_enabled ? "true" : "false")
              << "\n"
              << "  macro_divergence_ewma_alpha: "
              << config.macro_divergence_ewma_alpha << "\n"
              << "  basis_ewma_alpha: "
              << config.basis_ewma_alpha << "\n"
              << "  macro_shock_min_edge_tick: "
              << config.macro_shock_min_edge_tick << "\n"
              << "  max_allowed_spread_tick: "
              << config.max_allowed_spread_tick << "\n"
              << "  max_allowed_basis_tick: "
              << config.max_allowed_basis_tick << "\n"
              << "  max_taker_fills_per_minute: "
              << config.max_taker_fills_per_minute << "\n"
              << "  latest_ewma_fair_yes_tick: "
              << stats.latest_ewma_fair_yes_tick.load() << "\n"
              << "  latest_ewma_basis_yes_tick: "
              << stats.latest_ewma_basis_yes_tick.load() << "\n"
              << "  macro_structural_dislocation_blocked: "
              << stats.macro_structural_dislocation_blocked.load() << "\n"
              << "  macro_basis_uninitialized_blocked: "
              << stats.macro_basis_uninitialized_blocked.load() << "\n"
              << "  macro_basis_insanity_blocked: "
              << stats.macro_basis_insanity_blocked.load() << "\n"
              << "  macro_edge_not_crossing_blocked: "
              << stats.macro_edge_not_crossing_blocked.load() << "\n"
              << "  macro_divergence_sniping_signals: "
              << stats.macro_divergence_sniping_signals.load() << "\n"
              << "  macro_divergence_taker_ioc_fills_applied: "
              << stats.macro_divergence_taker_ioc_fills_applied.load()
              << "\n"
              << "  lead_lag_sniping_signals: "
              << stats.lead_lag_sniping_signals.load() << "\n"
              << "  lead_lag_taker_ioc_fills_applied: "
              << stats.lead_lag_taker_ioc_fills_applied.load() << "\n"
              << "  taker_max_entry_mid_slippage_tick: "
              << config.taker_max_entry_mid_slippage_tick << "\n"
              << "  latest_lead_lag_side: "
              << lead_lag_side_name(stats.latest_lead_lag_side.load())
              << "\n"
              << "  latest_lead_lag_edge_tick: "
              << stats.latest_lead_lag_edge_tick.load() << "\n"
              << "  locked_book_taker_hunter_enabled: "
              << (config.locked_book_taker_hunter_enabled ? "true" : "false")
              << "\n"
              << "  locked_book_taker_effective_min_edge_tick: "
              << effective_locked_book_taker_min_edge_tick(config) << "\n"
              << "  locked_book_taker_cooldown_ms: "
              << config.locked_book_taker_cooldown_ms << "\n"
              << "  locked_book_taker_size_lots: "
              << config.locked_book_taker_size_lots << "\n"
              << "  taker_ioc_signals: "
              << stats.taker_ioc_signals.load() << "\n"
              << "  taker_ioc_fills_applied: "
              << stats.taker_ioc_fills_applied.load() << "\n"
              << "  taker_ioc_yes_fills_applied: "
              << stats.taker_ioc_yes_fills_applied.load() << "\n"
              << "  taker_ioc_no_fills_applied: "
              << stats.taker_ioc_no_fills_applied.load() << "\n"
              << "  taker_ioc_fills_rejected: "
              << stats.taker_ioc_fills_rejected.load() << "\n"
              << "  taker_ioc_cooldown_blocked: "
              << stats.taker_ioc_cooldown_blocked.load() << "\n"
              << "  taker_ioc_inventory_blocked: "
              << stats.taker_ioc_inventory_blocked.load() << "\n"
              << "  taker_ioc_cash_blocked: "
              << stats.taker_ioc_cash_blocked.load() << "\n"
              << "  taker_ioc_mid_slippage_blocked: "
              << stats.taker_ioc_mid_slippage_blocked.load() << "\n"
              << "  taker_ioc_notional_tick: "
              << stats.taker_ioc_notional_tick.load() << "\n"
              << "  taker_ioc_expected_edge_tick: "
              << stats.taker_ioc_expected_edge_tick.load() << "\n"
              << "  latest_taker_ioc_edge_tick: "
              << stats.latest_taker_ioc_edge_tick.load() << "\n"
              << "  latest_taker_ioc_price_tick: "
              << stats.latest_taker_ioc_price_tick.load() << "\n"
              << "  latest_taker_ioc_qty_lots: "
              << stats.latest_taker_ioc_qty_lots.load() << "\n"
              << "  latest_taker_ioc_mid_tick: "
              << stats.latest_taker_ioc_mid_tick.load() << "\n"
              << "  latest_taker_ioc_mid_slippage_tick: "
              << stats.latest_taker_ioc_mid_slippage_tick.load() << "\n"
              << "  latest_taker_ioc_asset_side: "
              << taker_asset_side_name(stats.latest_taker_ioc_asset_side.load())
              << "\n"
              << "  latest_taker_ioc_source: "
              << taker_ioc_source_name(stats.latest_taker_ioc_source.load())
              << "\n"
              << "  reduce_exit_quotes_submitted: "
              << stats.reduce_exit_quotes_submitted.load() << "\n"
              << "  reduce_exit_passive_quotes: "
              << stats.reduce_exit_passive_quotes.load() << "\n"
              << "  reduce_exit_urgent_quotes: "
              << stats.reduce_exit_urgent_quotes.load() << "\n"
              << "  reduce_exit_puke_quotes: "
              << stats.reduce_exit_puke_quotes.load() << "\n"
              << "  latest_reduce_exit_asset_side: "
              << taker_asset_side_name(
                     stats.latest_reduce_exit_asset_side.load()
                 ) << "\n"
              << "  latest_reduce_exit_stage: "
              << reduce_exit_stage_name(
                     stats.latest_reduce_exit_stage.load()
                 ) << "\n"
              << "  latest_reduce_exit_age_ms: "
              << stats.latest_reduce_exit_age_ms.load() << "\n"
              << "  latest_reduce_exit_excess_lots: "
              << stats.latest_reduce_exit_excess_lots.load() << "\n"
              << "  latest_reduce_exit_pressure_bps: "
              << stats.latest_reduce_exit_pressure_bps.load() << "\n"
              << "  latest_reduce_exit_price_tick: "
              << stats.latest_reduce_exit_price_tick.load() << "\n"
              << "  latest_reduce_exit_qty_lots: "
              << stats.latest_reduce_exit_qty_lots.load() << "\n"
              << "  as_model_enabled: "
              << (config.enable_as_model ? "true" : "false") << "\n"
              << "  as_model_ok: "
              << (final_dynamic_quote.as_ok ? "true" : "false") << "\n"
              << "  as_half_spread_tick: "
              << final_dynamic_quote.half_spread_tick << "\n"
              << "  as_inventory_skew_tick: "
              << final_dynamic_quote.max_inventory_skew_tick << "\n"
              << "  starting_cash_tick: " << config.starting_cash_tick << "\n"
              << "  initial_position_lots: "
              << config.initial_position_lots << "\n"
              << "  seed_complete_set: "
              << (config.seed_complete_set ? "true" : "false") << "\n"
              << "  initial_complement_position_lots: "
              << config.initial_complement_position_lots << "\n"
              << "  target_position_lots: "
              << config.target_position_lots << "\n"
              << "  cash_tick: " << runtime.pnl.cash_tick << "\n"
              << "  realized_pnl_tick: "
              << runtime.pnl.maker_realized_pnl_tick << "\n"
              << "  unrealized_pnl_mid_tick: "
              << runtime.pnl.maker_unrealized_pnl_mid_tick << "\n"
              << "  equity_mid_tick: " << runtime.pnl.equity_mid_tick << "\n"
              << "  seed_total_pnl_mid_tick: "
              << seed_total_pnl_mid_tick << "\n"
              << "  seed_complement_position_lots: "
              << runtime.attribution.seed_complement_position_lots << "\n"
              << "  seed_complement_unrealized_pnl_mid_tick: "
              << seed_complement_unrealized_mid_tick << "\n"
              << "  strategy_total_pnl_mid_tick: "
              << strategy_total_pnl_mid_tick << "\n"
              << "  strategy_spread_capture_tick: "
              << runtime.attribution.strategy_spread_capture_tick << "\n"
              << "  open_position_lots: "
              << runtime.ledger.position_ledger().lots(config.asset_id) << "\n"
              << "  maker_fill_count: " << runtime.pnl.maker_fill_count
              << "\n"
              << "  ws_packets: " << stats.ws_packets.load() << "\n"
              << "  normalized_events: " << stats.normalized_events.load()
              << "\n"
              << "  filtered_events: " << stats.filtered_events.load() << "\n"
              << "  book_snapshots: " << stats.book_snapshots.load() << "\n"
              << "  book_deltas: " << stats.book_deltas.load() << "\n"
              << "  depth_updates: " << stats.depth_updates.load() << "\n"
              << "  book_quarantine_events: "
              << stats.book_quarantine_events.load() << "\n"
              << "  depth_updates_quarantined: "
              << stats.depth_updates_quarantined.load() << "\n"
              << "  quote_intents: " << stats.mm_quote_intents.load() << "\n"
              << "  cancel_intents: " << stats.mm_cancel_intents.load()
              << "\n"
              << "  risk_approved: " << stats.risk_approved.load() << "\n"
              << "  risk_rejected: " << stats.risk_rejected.load() << "\n"
              << "  no_quote_reasons: " << no_quote_reasons << "\n"
              << "  risk_decisions: " << risk_decisions << "\n"
              << "  submitted_quotes: " << stats.submitted_quotes.load()
              << "\n"
              << "  cancelled_quotes: " << stats.cancelled_quotes.load()
              << "\n"
              << "  maker_reports: " << stats.maker_reports.load() << "\n"
              << "  maker_fills_applied: "
              << stats.maker_fills_applied.load() << "\n"
              << "  maker_fills_rejected: "
              << stats.maker_fills_rejected.load() << "\n"
              << "  decode_errors: " << stats.decode_errors.load() << "\n"
              << "  state_errors: " << stats.state_errors.load() << "\n"
              << "  transport_errors: " << stats.transport_errors.load()
              << "\n"
              << "  dashboard_write_errors: "
              << stats.dashboard_write_errors.load() << "\n";
    print_latency("pipeline_latency_ns", pipeline_stats);
    if (!config.dashboard_file.empty()) {
        std::cout << "  dashboard_file: " << config.dashboard_file << "\n";
    }
    if (!config.out_json.empty()) {
        std::cout << "  out_json: " << config.out_json << "\n";
    }
    return fatal.load() ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "market_maker_dashboard_live_error: "
                  << error.what() << '\n';
        return 1;
    }
}

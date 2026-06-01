#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/execution/public/ReservationDisposition.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/metrics/PerformanceMetricsEngine.h"
#include "engine/paper/pnl/PaperPnLEngine.h"
#include "engine/paper/portfolio/PaperPortfolio.h"
#include "engine/paper/read/DashboardReadStore.h"
#include "engine/paper/regime/RegimeEngine.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trading_engine::paper {

class PaperTradingEngine {
public:
    explicit PaperTradingEngine(
        std::int64_t starting_cash_tick = 0,
        std::size_t dashboard_capacity = 256
    );

    void on_execution_report(
        const trading_engine::execution::ExecutionReport& report
    );
    void on_reservation_disposition(
        const trading_engine::execution::ReservationDisposition& disposition
    );
    void on_opportunity_intent(
        const trading_engine::signal::OpportunityIntent& intent
    );
    void on_risk_decision(const trading_engine::risk::RiskDecision& decision);
    void on_approved_intent(
        const trading_engine::risk::ApprovedIntent& approved
    );
    void on_order_plan(const trading_engine::execution::OrderPlan& plan);
    void on_mark_snapshot(
        const trading_engine::state::MarketStateSnapshot& snapshot
    );

    [[nodiscard]] DashboardSnapshot latest_dashboard_snapshot() const;

private:
    void handle_observed_event(const PaperEventAdapterResult& result);
    void record_order_plan(
        const trading_engine::execution::OrderPlan& plan
    );
    void record_filled_order(const FillApplication& fill);
    void publish_dashboard(std::uint64_t ts_ns);
    [[nodiscard]] PaperPnLResult compute_pnl(std::uint64_t ts_ns) const;
    void record_equity_point(const EquityCurve& equity);
    [[nodiscard]] DashboardSnapshot build_dashboard(std::uint64_t ts_ns) const;
    [[nodiscard]] DashboardSnapshot build_dashboard_from_pnl(
        const PaperPnLResult& pnl,
        std::uint64_t ts_ns
    ) const;
    [[nodiscard]] PerformanceSnapshot build_performance_snapshot(
        const PaperPnLResult& pnl,
        std::uint64_t ts_ns
    ) const;
    [[nodiscard]] RegimeSnapshot build_regime_snapshot(std::uint64_t ts_ns) const;
    [[nodiscard]] std::vector<PaperFilledOrderSnapshot>
    build_filled_order_snapshots() const;
    [[nodiscard]] static std::uint64_t child_key(
        std::uint64_t plan_id,
        std::uint64_t child_order_id
    ) noexcept;

    struct ObservedChildOrder {
        trading_engine::execution::ChildOrder order;
        std::uint64_t source_intent_id = 0;
        std::uint64_t approved_intent_id = 0;
        std::uint64_t reservation_id = 0;
        std::uint64_t bundle_id = 0;
    };

    PaperEventAdapter adapter_;
    PaperLedger ledger_;
    PaperPortfolio portfolio_;
    PaperPnLEngine pnl_engine_;
    PerformanceMetricsEngine performance_engine_;
    RegimeEngine regime_engine_;
    DashboardReadStore dashboard_store_;

    std::uint64_t intents_observed_ = 0;
    std::uint64_t paper_opportunities_observed_ = 0;
    std::uint64_t signal_rejections_observed_ = 0;

    std::uint64_t risk_decisions_observed_ = 0;
    std::uint64_t risk_approved_observed_ = 0;
    std::uint64_t risk_rejected_observed_ = 0;

    std::uint64_t approved_intents_observed_ = 0;
    std::uint64_t plans_created_ = 0;
    std::uint64_t execution_reports_observed_ = 0;
    std::int64_t filled_notional_tick_ = 0;

    std::vector<trading_engine::state::MarketDepthView> depth_views_;
    std::vector<EquityCurve> equity_curve_;
    std::vector<trading_engine::risk::RiskDecision> risk_decisions_;
    std::vector<trading_engine::execution::ExecutionReport> execution_reports_;
    std::vector<trading_engine::execution::PlanStatus> plan_statuses_;
    std::vector<PaperFilledOrderSnapshot> filled_orders_;

    std::unordered_set<std::uint64_t> filled_plan_ids_;
    std::unordered_set<std::uint64_t> failed_plan_ids_;
    std::unordered_map<std::uint64_t, ObservedChildOrder> child_orders_;
    std::unordered_map<std::string, std::uint32_t> asset_index_by_asset_id_;
};

}  // namespace trading_engine::paper

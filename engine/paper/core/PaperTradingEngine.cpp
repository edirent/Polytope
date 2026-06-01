#include "engine/paper/core/PaperTradingEngine.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::paper {
namespace {

[[nodiscard]] bool is_signal_rejection(
    trading_engine::signal::IntentStatus status
) noexcept {
    using trading_engine::signal::IntentStatus;
    return status != IntentStatus::CandidateOnly &&
           status != IntentStatus::PaperOpportunity;
}

[[nodiscard]] bool is_filled_report(
    trading_engine::execution::ChildOrderStatus status
) noexcept {
    using trading_engine::execution::ChildOrderStatus;
    return status == ChildOrderStatus::Filled;
}

[[nodiscard]] bool is_failed_report(
    trading_engine::execution::ChildOrderStatus status
) noexcept {
    using trading_engine::execution::ChildOrderStatus;
    return status == ChildOrderStatus::Failed ||
           status == ChildOrderStatus::Expired ||
           status == ChildOrderStatus::Cancelled;
}

[[nodiscard]] PaperMetricStatus metric_status(
    MetricStatus status
) noexcept {
    switch (status) {
        case MetricStatus::Ok:
            return PaperMetricStatus::Ok;
        case MetricStatus::InvalidInput:
            return PaperMetricStatus::InvalidInput;
        case MetricStatus::InsufficientData:
            return PaperMetricStatus::InsufficientData;
    }
    return PaperMetricStatus::InvalidInput;
}

[[nodiscard]] double metric_value(const MetricValue& metric) noexcept {
    return metric.status == MetricStatus::Ok ? metric.value : 0.0;
}

[[nodiscard]] bool is_buy_side(std::string_view side) noexcept {
    return side == "Buy";
}

}  // namespace

PaperTradingEngine::PaperTradingEngine(
    std::int64_t starting_cash_tick,
    std::size_t dashboard_capacity
)
    : ledger_(starting_cash_tick),
      dashboard_store_(dashboard_capacity) {}

void PaperTradingEngine::on_execution_report(
    const trading_engine::execution::ExecutionReport& report
) {
    ++execution_reports_observed_;
    execution_reports_.push_back(report);

    if (is_filled_report(report.status)) {
        filled_plan_ids_.insert(report.plan_id);
    }
    if (is_failed_report(report.status)) {
        failed_plan_ids_.insert(report.plan_id);
    }

    handle_observed_event(adapter_.observe(report));
}

void PaperTradingEngine::on_reservation_disposition(
    const trading_engine::execution::ReservationDisposition& disposition
) {
    handle_observed_event(adapter_.observe(disposition));
}

void PaperTradingEngine::on_opportunity_intent(
    const trading_engine::signal::OpportunityIntent& intent
) {
    ++intents_observed_;
    if (intent.status == trading_engine::signal::IntentStatus::PaperOpportunity) {
        ++paper_opportunities_observed_;
    } else if (is_signal_rejection(intent.status)) {
        ++signal_rejections_observed_;
    }

    handle_observed_event(adapter_.observe(intent));
}

void PaperTradingEngine::on_risk_decision(
    const trading_engine::risk::RiskDecision& decision
) {
    ++risk_decisions_observed_;
    if (decision.approved()) {
        ++risk_approved_observed_;
    } else {
        ++risk_rejected_observed_;
    }
    risk_decisions_.push_back(decision);

    handle_observed_event(adapter_.observe(decision));
}

void PaperTradingEngine::on_approved_intent(
    const trading_engine::risk::ApprovedIntent& approved
) {
    ++approved_intents_observed_;
    handle_observed_event(adapter_.observe(approved));
}

void PaperTradingEngine::on_order_plan(
    const trading_engine::execution::OrderPlan& plan
) {
    ++plans_created_;
    plan_statuses_.push_back(plan.status);
    record_order_plan(plan);
    handle_observed_event(adapter_.observe(plan));
}

void PaperTradingEngine::on_mark_snapshot(
    const trading_engine::state::MarketStateSnapshot& snapshot
) {
    std::uint32_t asset_index = 0;
    const auto index_it = asset_index_by_asset_id_.find(snapshot.entity_id);
    if (index_it != asset_index_by_asset_id_.end()) {
        asset_index = index_it->second;
    }
    auto view = trading_engine::state::market_depth_view_from_snapshot(
        snapshot,
        asset_index
    );
    auto it = std::find_if(
        depth_views_.begin(),
        depth_views_.end(),
        [&view](const trading_engine::state::MarketDepthView& existing) {
            return existing.asset_index == view.asset_index;
        }
    );
    if (it == depth_views_.end()) {
        depth_views_.push_back(view);
    } else {
        *it = view;
    }

    handle_observed_event(adapter_.observe_mark_update(snapshot));
}

DashboardSnapshot PaperTradingEngine::latest_dashboard_snapshot() const {
    if (const auto latest = dashboard_store_.latest()) {
        return *latest;
    }
    return build_dashboard(0);
}

void PaperTradingEngine::handle_observed_event(
    const PaperEventAdapterResult& result
) {
    if (result.has_fill) {
        const auto ledger_result = ledger_.apply_fill(result.fill);
        if (ledger_result.applied) {
            (void)portfolio_.apply_fill(result.fill);
            record_filled_order(result.fill);
            filled_notional_tick_ +=
                result.fill.report.filled_lots *
                result.fill.report.avg_fill_price_tick;
        }
    }

    publish_dashboard(result.event.ts_ns);
}

void PaperTradingEngine::record_order_plan(
    const trading_engine::execution::OrderPlan& plan
) {
    PlanTerminalState terminal;
    terminal.plan_id = plan.plan_id;
    terminal.bundle_id = plan.bundle_id;
    terminal.source_intent_id = plan.source_intent_id;
    terminal.approved_intent_id = plan.approved_intent_id;
    terminal.reservation_id = plan.reservation_id;
    terminal.expected_child_orders = plan.order_count;
    terminal.chosen_bundle_qty = plan.chosen_bundle_qty;
    terminal.guaranteed_payout_tick = plan.guaranteed_payout_tick;
    terminal.expected_terminal_pnl_tick = plan.expected_terminal_pnl_tick;
    terminal.updated_ts_ns = plan.created_ts_ns;
    terminal_by_plan_.insert_or_assign(plan.plan_id, terminal);

    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        ObservedChildOrder observed;
        observed.order = plan.orders[i];
        observed.source_intent_id = plan.source_intent_id;
        observed.approved_intent_id = plan.approved_intent_id;
        observed.reservation_id = plan.reservation_id;
        observed.bundle_id = plan.bundle_id;
        if (!plan.orders[i].asset_id.empty()) {
            asset_index_by_asset_id_.insert_or_assign(
                plan.orders[i].asset_id,
                plan.orders[i].asset_index
            );
        }
        child_orders_.insert_or_assign(
            child_key(plan.plan_id, plan.orders[i].order_id),
            std::move(observed)
        );
    }
}

void PaperTradingEngine::record_filled_order(const FillApplication& fill) {
    const auto report_id = derive_execution_report_id(fill);
    const auto order_it = child_orders_.find(
        child_key(fill.report.plan_id, fill.report.child_order_id)
    );

    PaperFilledOrderSnapshot filled;
    filled.execution_report_id = report_id;
    filled.plan_id = fill.report.plan_id;
    filled.child_order_id = fill.report.child_order_id;
    filled.market_id = fill.market_id;
    filled.asset_id = fill.asset_id;
    filled.asset_index = fill.asset_index;
    filled.side = trading_engine::execution::to_string(fill.side);
    filled.filled_lots = fill.report.filled_lots;
    filled.remaining_lots = fill.report.remaining_lots;
    filled.avg_fill_price_tick = fill.report.avg_fill_price_tick;
    filled.notional_tick =
        fill.report.filled_lots * fill.report.avg_fill_price_tick;
    filled.event_ts_ns = fill.report.event_ts_ns;
    filled.mark_quality = "MissingMark";

    if (order_it != child_orders_.end()) {
        const auto& observed = order_it->second;
        const auto& order = observed.order;
        filled.bundle_id = observed.bundle_id;
        filled.source_intent_id = observed.source_intent_id;
        filled.approved_intent_id = observed.approved_intent_id;
        filled.reservation_id = observed.reservation_id;
        filled.market_id = order.market_id;
        filled.asset_id = order.asset_id;
        filled.market_index = order.market_index;
        filled.asset_index = order.asset_index;
        filled.side = trading_engine::execution::to_string(order.side);
        filled.limit_price_tick = order.limit_price_tick;
        filled.estimated_vwap_tick = order.estimated_vwap_tick;
        filled.worst_price_tick = order.worst_allowed_price_tick;
    }

    filled_orders_.push_back(std::move(filled));

    auto terminal_it = terminal_by_plan_.find(fill.report.plan_id);
    if (terminal_it != terminal_by_plan_.end()) {
        auto& terminal = terminal_it->second;
        terminal.actual_buy_cost_tick += filled.notional_tick;
        terminal.filled_child_orders =
            static_cast<std::uint16_t>(terminal.filled_child_orders + 1);
        terminal.updated_ts_ns = fill.report.event_ts_ns;
    }
}

void PaperTradingEngine::publish_dashboard(std::uint64_t ts_ns) {
    const auto pnl = compute_pnl(ts_ns);
    record_equity_point(pnl.equity);
    (void)dashboard_store_.publish(build_dashboard_from_pnl(pnl, ts_ns));
}

PaperPnLResult PaperTradingEngine::compute_pnl(std::uint64_t ts_ns) const {
    return pnl_engine_.compute(
        portfolio_,
        ledger_.cash_ledger(),
        std::span<const trading_engine::state::MarketDepthView>{
            depth_views_.data(),
            depth_views_.size()
        },
        ts_ns
    );
}

void PaperTradingEngine::record_equity_point(const EquityCurve& equity) {
    if (!equity_curve_.empty()) {
        const auto& last = equity_curve_.back();
        if (last.ts_ns == equity.ts_ns &&
            last.equity_mid_tick == equity.equity_mid_tick &&
            last.equity_liquidation_tick == equity.equity_liquidation_tick &&
            last.realized_pnl_tick == equity.realized_pnl_tick &&
            last.unrealized_pnl_mid_tick == equity.unrealized_pnl_mid_tick &&
            last.unrealized_pnl_liquidation_tick ==
                equity.unrealized_pnl_liquidation_tick) {
            return;
        }
    }
    equity_curve_.push_back(equity);
}

DashboardSnapshot PaperTradingEngine::build_dashboard(std::uint64_t ts_ns) const {
    return build_dashboard_from_pnl(compute_pnl(ts_ns), ts_ns);
}

DashboardSnapshot PaperTradingEngine::build_dashboard_from_pnl(
    const PaperPnLResult& pnl,
    std::uint64_t ts_ns
) const {

    DashboardSnapshot snapshot;
    snapshot.ts_ns = ts_ns;
    snapshot.account = ledger_.account_snapshot();
    snapshot.account.unrealized_pnl_tick = pnl.unrealized_pnl_mid_tick;
    snapshot.account.version = dashboard_store_.latest_seq() + 1;
    snapshot.account.updated_ts_ns = ts_ns;

    snapshot.performance = build_performance_snapshot(pnl, ts_ns);
    snapshot.regime = build_regime_snapshot(ts_ns);

    snapshot.signal.intents_published = intents_observed_;
    snapshot.signal.paper_opportunities = paper_opportunities_observed_;
    snapshot.signal.rejected = signal_rejections_observed_;

    snapshot.risk.decisions = risk_decisions_observed_;
    snapshot.risk.approved = risk_approved_observed_;
    snapshot.risk.rejected = risk_rejected_observed_;

    snapshot.execution.plans_created = plans_created_;
    snapshot.execution.plans_filled = filled_plan_ids_.size();
    snapshot.execution.plans_failed = failed_plan_ids_.size();
    snapshot.filled_orders = build_filled_order_snapshots();
    snapshot.terminal_pnl = build_terminal_pnl_snapshots();

    return snapshot;
}

PerformanceSnapshot PaperTradingEngine::build_performance_snapshot(
    const PaperPnLResult& pnl,
    std::uint64_t ts_ns
) const {
    auto equity_curve = equity_curve_;
    equity_curve.push_back(pnl.equity);

    PerformanceMetricsInput input;
    input.equity_curve =
        std::span<const EquityCurve>{equity_curve.data(), equity_curve.size()};
    input.intents_observed = intents_observed_;
    input.approvals_observed = risk_approved_observed_;
    input.plans_created = plans_created_;
    input.filled_plans = filled_plan_ids_.size();
    input.filled_notional_tick = filled_notional_tick_;

    const auto metrics = performance_engine_.compute(input);

    PerformanceSnapshot snapshot;
    snapshot.intents_observed = intents_observed_;
    snapshot.approvals_observed = risk_approved_observed_;
    snapshot.plans_observed = plans_created_;
    snapshot.execution_reports_observed = execution_reports_observed_;
    snapshot.filled_plans = filled_plan_ids_.size();
    snapshot.failed_plans = failed_plan_ids_.size();
    snapshot.gross_pnl_tick =
        pnl.realized_pnl_tick + pnl.unrealized_pnl_mid_tick;
    snapshot.net_pnl_tick =
        snapshot.gross_pnl_tick - ledger_.cash_ledger().fees_paid_tick;
    for (const auto& terminal : build_terminal_pnl_snapshots()) {
        if (!terminal.complete) {
            continue;
        }
        snapshot.terminal_payout_tick += terminal.guaranteed_payout_tick;
        snapshot.terminal_cost_tick += terminal.actual_buy_cost_tick;
        snapshot.terminal_pnl_tick += terminal.terminal_pnl_tick;
        ++snapshot.terminal_complete_plans;
    }
    snapshot.max_drawdown_tick = metrics.drawdown.max_drawdown_tick;
    snapshot.max_drawdown_ratio = metrics.drawdown.max_drawdown_ratio;

    snapshot.returns_count = metrics.return_samples;
    snapshot.latest_return = metric_value(metrics.latest_return);
    snapshot.latest_return_status =
        metric_status(metrics.latest_return.status);
    snapshot.volatility = metric_value(metrics.volatility);
    snapshot.volatility_status = metric_status(metrics.volatility.status);
    snapshot.sharpe = metric_value(metrics.sharpe);
    snapshot.sharpe_status = metric_status(metrics.sharpe.status);

    snapshot.fill_rate = metric_value(metrics.fill_rate);
    snapshot.fill_rate_status = metric_status(metrics.fill_rate.status);
    snapshot.risk_approval_rate = metric_value(metrics.risk_approval_rate);
    snapshot.risk_approval_rate_status =
        metric_status(metrics.risk_approval_rate.status);
    snapshot.intent_conversion_rate =
        metric_value(metrics.intent_conversion_rate);
    snapshot.intent_conversion_rate_status =
        metric_status(metrics.intent_conversion_rate.status);
    snapshot.turnover = metric_value(metrics.turnover);
    snapshot.turnover_status = metric_status(metrics.turnover.status);

    snapshot.version = dashboard_store_.latest_seq() + 1;
    snapshot.updated_ts_ns = ts_ns;
    return snapshot;
}

RegimeSnapshot PaperTradingEngine::build_regime_snapshot(
    std::uint64_t ts_ns
) const {
    RegimeInput input;
    input.now_ns = ts_ns;
    input.depth_views =
        std::span<const trading_engine::state::MarketDepthView>{
            depth_views_.data(),
            depth_views_.size()
        };
    input.signal_intents_observed = intents_observed_;
    input.signal_rejections_observed = signal_rejections_observed_;
    input.risk_decisions = std::span<const trading_engine::risk::RiskDecision>{
        risk_decisions_.data(),
        risk_decisions_.size()
    };
    input.execution_reports =
        std::span<const trading_engine::execution::ExecutionReport>{
            execution_reports_.data(),
            execution_reports_.size()
        };
    input.plan_statuses =
        std::span<const trading_engine::execution::PlanStatus>{
            plan_statuses_.data(),
            plan_statuses_.size()
        };
    return regime_engine_.classify(input);
}

std::vector<PaperFilledOrderSnapshot>
PaperTradingEngine::build_filled_order_snapshots() const {
    auto out = filled_orders_;
    for (auto& order : out) {
        const auto view_it = std::find_if(
            depth_views_.begin(),
            depth_views_.end(),
            [&order](const trading_engine::state::MarketDepthView& view) {
                return view.asset_index == order.asset_index;
            }
        );

        if (view_it != depth_views_.end()) {
            const auto& view = *view_it;
            if (view.bid_count > 0 && view.ask_count > 0) {
                order.mark_price_tick =
                    (view.bids[0].price_tick + view.asks[0].price_tick) / 2;
                order.mark_quality = "Mid";
            } else if (view.bid_count > 0) {
                order.mark_price_tick = view.bids[0].price_tick;
                order.mark_quality = "BidOnly";
            } else if (view.ask_count > 0) {
                order.mark_price_tick = view.asks[0].price_tick;
                order.mark_quality = "AskOnly";
            } else {
                order.mark_price_tick = 0;
                order.mark_quality = "MissingMark";
            }
        } else {
            order.mark_price_tick = 0;
            order.mark_quality = "MissingMark";
        }

        const auto price_delta = is_buy_side(order.side)
            ? order.mark_price_tick - order.avg_fill_price_tick
            : order.avg_fill_price_tick - order.mark_price_tick;
        order.unrealized_pnl_tick = order.filled_lots * price_delta;
    }
    return out;
}

std::vector<PaperTerminalPnLSnapshot>
PaperTradingEngine::build_terminal_pnl_snapshots() const {
    std::vector<PaperTerminalPnLSnapshot> out;
    out.reserve(terminal_by_plan_.size());
    for (const auto& [_, terminal] : terminal_by_plan_) {
        PaperTerminalPnLSnapshot snapshot;
        snapshot.plan_id = terminal.plan_id;
        snapshot.bundle_id = terminal.bundle_id;
        snapshot.source_intent_id = terminal.source_intent_id;
        snapshot.approved_intent_id = terminal.approved_intent_id;
        snapshot.reservation_id = terminal.reservation_id;
        snapshot.expected_child_orders = terminal.expected_child_orders;
        snapshot.filled_child_orders = terminal.filled_child_orders;
        snapshot.complete = terminal.expected_child_orders > 0 &&
            terminal.filled_child_orders >= terminal.expected_child_orders;
        snapshot.chosen_bundle_qty = terminal.chosen_bundle_qty;
        snapshot.guaranteed_payout_tick = terminal.guaranteed_payout_tick;
        snapshot.expected_terminal_pnl_tick =
            terminal.expected_terminal_pnl_tick;
        snapshot.actual_buy_cost_tick = terminal.actual_buy_cost_tick;
        snapshot.terminal_pnl_tick = snapshot.complete
            ? terminal.guaranteed_payout_tick - terminal.actual_buy_cost_tick
            : 0;
        snapshot.updated_ts_ns = terminal.updated_ts_ns;
        out.push_back(snapshot);
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const PaperTerminalPnLSnapshot& left,
           const PaperTerminalPnLSnapshot& right) {
            return left.plan_id < right.plan_id;
        }
    );
    return out;
}

std::uint64_t PaperTradingEngine::child_key(
    std::uint64_t plan_id,
    std::uint64_t child_order_id
) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };
    mix(plan_id);
    mix(child_order_id);
    return hash;
}

}  // namespace trading_engine::paper

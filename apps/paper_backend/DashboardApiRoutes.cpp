#include "apps/paper_backend/DashboardApiRoutes.h"

#include <sstream>
#include <string_view>
#include <vector>

namespace trading_engine::paper_backend {
namespace {

using trading_engine::paper::ChainRegime;
using trading_engine::paper::DashboardSnapshot;
using trading_engine::paper::DataRegime;
using trading_engine::paper::ExecutionRegime;
using trading_engine::paper::LatencySnapshot;
using trading_engine::paper::LiquidityRegime;
using trading_engine::paper::PaperFilledOrderSnapshot;
using trading_engine::paper::PaperMetricStatus;
using trading_engine::paper::PerformanceSnapshot;
using trading_engine::paper::RegimeSnapshot;
using trading_engine::paper::RiskRegime;
using trading_engine::paper::SignalRegime;

[[nodiscard]] std::string path_only(std::string_view target) {
    const auto question = target.find('?');
    return std::string{target.substr(0, question)};
}

[[nodiscard]] const char* data_regime_name(DataRegime regime) noexcept {
    switch (regime) {
        case DataRegime::Healthy:
            return "Healthy";
        case DataRegime::Stale:
            return "Stale";
        case DataRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* liquidity_regime_name(
    LiquidityRegime regime
) noexcept {
    switch (regime) {
        case LiquidityRegime::Healthy:
            return "Healthy";
        case LiquidityRegime::Degraded:
            return "Degraded";
        case LiquidityRegime::Crossed:
            return "Crossed";
        case LiquidityRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* chain_regime_name(ChainRegime regime) noexcept {
    switch (regime) {
        case ChainRegime::Healthy:
            return "Healthy";
        case ChainRegime::Lagging:
            return "Lagging";
        case ChainRegime::Error:
            return "Error";
        case ChainRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* signal_regime_name(SignalRegime regime) noexcept {
    switch (regime) {
        case SignalRegime::Healthy:
            return "Healthy";
        case SignalRegime::Quiet:
            return "Quiet";
        case SignalRegime::Rejecting:
            return "Rejecting";
        case SignalRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* risk_regime_name(RiskRegime regime) noexcept {
    switch (regime) {
        case RiskRegime::Healthy:
            return "Healthy";
        case RiskRegime::Constrained:
            return "Constrained";
        case RiskRegime::KillSwitch:
            return "KillSwitch";
        case RiskRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* execution_regime_name(
    ExecutionRegime regime
) noexcept {
    switch (regime) {
        case ExecutionRegime::Healthy:
            return "Healthy";
        case ExecutionRegime::PartialFill:
            return "PartialFill";
        case ExecutionRegime::HedgeRequired:
            return "HedgeRequired";
        case ExecutionRegime::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] const char* metric_status_name(
    PaperMetricStatus status
) noexcept {
    switch (status) {
        case PaperMetricStatus::Ok:
            return "Ok";
        case PaperMetricStatus::InsufficientData:
            return "InsufficientData";
        case PaperMetricStatus::InvalidInput:
            return "InvalidInput";
    }
    return "InvalidInput";
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
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
                out += ch;
                break;
        }
    }
    return out;
}

[[nodiscard]] std::string performance_json(
    const PerformanceSnapshot& performance
) {
    std::ostringstream out;
    out << "{\"intents_observed\":" << performance.intents_observed
        << ",\"approvals_observed\":" << performance.approvals_observed
        << ",\"plans_observed\":" << performance.plans_observed
        << ",\"execution_reports_observed\":"
        << performance.execution_reports_observed
        << ",\"filled_plans\":" << performance.filled_plans
        << ",\"failed_plans\":" << performance.failed_plans
        << ",\"gross_pnl_tick\":" << performance.gross_pnl_tick
        << ",\"net_pnl_tick\":" << performance.net_pnl_tick
        << ",\"max_drawdown_tick\":" << performance.max_drawdown_tick
        << ",\"max_drawdown_ratio\":" << performance.max_drawdown_ratio
        << ",\"returns_count\":" << performance.returns_count
        << ",\"latest_return\":" << performance.latest_return
        << ",\"latest_return_status\":\""
        << metric_status_name(performance.latest_return_status)
        << "\",\"volatility\":" << performance.volatility
        << ",\"volatility_status\":\""
        << metric_status_name(performance.volatility_status)
        << "\",\"sharpe\":" << performance.sharpe
        << ",\"sharpe_status\":\""
        << metric_status_name(performance.sharpe_status)
        << "\",\"fill_rate\":" << performance.fill_rate
        << ",\"fill_rate_status\":\""
        << metric_status_name(performance.fill_rate_status)
        << "\",\"risk_approval_rate\":" << performance.risk_approval_rate
        << ",\"risk_approval_rate_status\":\""
        << metric_status_name(performance.risk_approval_rate_status)
        << "\",\"intent_conversion_rate\":"
        << performance.intent_conversion_rate
        << ",\"intent_conversion_rate_status\":\""
        << metric_status_name(performance.intent_conversion_rate_status)
        << "\",\"turnover\":" << performance.turnover
        << ",\"turnover_status\":\""
        << metric_status_name(performance.turnover_status) << '"'
        << ",\"version\":" << performance.version
        << ",\"updated_ts_ns\":" << performance.updated_ts_ns << '}';
    return out.str();
}

[[nodiscard]] std::string filled_orders_json(
    const std::vector<PaperFilledOrderSnapshot>& orders
) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& order : orders) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << "{\"execution_report_id\":" << order.execution_report_id
            << ",\"plan_id\":" << order.plan_id
            << ",\"child_order_id\":" << order.child_order_id
            << ",\"bundle_id\":" << order.bundle_id
            << ",\"source_intent_id\":" << order.source_intent_id
            << ",\"approved_intent_id\":" << order.approved_intent_id
            << ",\"reservation_id\":" << order.reservation_id
            << ",\"market_id\":\"" << json_escape(order.market_id)
            << "\",\"asset_id\":\"" << json_escape(order.asset_id)
            << "\",\"market_index\":" << order.market_index
            << ",\"asset_index\":" << order.asset_index
            << ",\"side\":\"" << json_escape(order.side)
            << "\",\"filled_lots\":" << order.filled_lots
            << ",\"remaining_lots\":" << order.remaining_lots
            << ",\"avg_fill_price_tick\":" << order.avg_fill_price_tick
            << ",\"limit_price_tick\":" << order.limit_price_tick
            << ",\"estimated_vwap_tick\":" << order.estimated_vwap_tick
            << ",\"worst_price_tick\":" << order.worst_price_tick
            << ",\"notional_tick\":" << order.notional_tick
            << ",\"mark_price_tick\":" << order.mark_price_tick
            << ",\"unrealized_pnl_tick\":" << order.unrealized_pnl_tick
            << ",\"mark_quality\":\"" << json_escape(order.mark_quality)
            << "\",\"event_ts_ns\":" << order.event_ts_ns << '}';
    }
    out << ']';
    return out.str();
}

[[nodiscard]] std::string regime_json(const RegimeSnapshot& regime) {
    std::ostringstream out;
    out << "{\"data\":\"" << data_regime_name(regime.data)
        << "\",\"liquidity\":\"" << liquidity_regime_name(regime.liquidity)
        << "\",\"chain\":\"" << chain_regime_name(regime.chain)
        << "\",\"signal\":\"" << signal_regime_name(regime.signal)
        << "\",\"risk\":\"" << risk_regime_name(regime.risk)
        << "\",\"execution\":\"" << execution_regime_name(regime.execution)
        << "\",\"version\":" << regime.version
        << ",\"ts_ns\":" << regime.ts_ns << '}';
    return out.str();
}

[[nodiscard]] std::string latency_json(const LatencySnapshot& latency) {
    std::ostringstream out;
    out << "{\"feed_to_state_ns\":" << latency.feed_to_state_ns
        << ",\"state_to_signal_ns\":" << latency.state_to_signal_ns
        << ",\"signal_to_risk_ns\":" << latency.signal_to_risk_ns
        << ",\"risk_to_execution_ns\":" << latency.risk_to_execution_ns
        << ",\"end_to_end_ns\":" << latency.end_to_end_ns << '}';
    return out.str();
}

[[nodiscard]] ApiRouteResponse json_ok(std::string body) {
    ApiRouteResponse response;
    response.status = 200;
    response.content_type = "application/json";
    response.body = std::move(body);
    return response;
}

}  // namespace

DashboardApiRoutes::DashboardApiRoutes(
    const trading_engine::paper::DashboardReadStore* store
) noexcept
    : store_(store) {}

ApiRouteResponse DashboardApiRoutes::handle_get(std::string_view target) const {
    const auto path = path_only(target);
    if (path == "/api/v1/health") {
        return json_ok(R"({"ok":true,"mode":"readonly"})");
    }
    if (path == "/api/v1/snapshot/latest") {
        return latest_snapshot();
    }
    if (path == "/api/v1/markets") {
        return json_ok(R"({"markets":[]})");
    }
    if (path == "/api/v1/intents") {
        return json_ok(R"({"intents":[]})");
    }
    if (path == "/api/v1/risk-decisions") {
        return json_ok(R"({"risk_decisions":[]})");
    }
    if (path == "/api/v1/execution-reports") {
        const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
        std::ostringstream out;
        out << "{\"execution_reports\":"
            << (latest ? filled_orders_json(latest->filled_orders)
                       : std::string{"[]"})
            << '}';
        return json_ok(out.str());
    }
    if (path == "/api/v1/pnl/equity") {
        const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
        if (!latest) {
            return json_ok(R"({"equity_mid":0,"equity_liquidation":0})");
        }
        std::ostringstream out;
        const auto equity =
            latest->account.starting_cash_tick +
            latest->account.realized_pnl_tick +
            latest->account.unrealized_pnl_tick;
        out << "{\"equity_mid\":" << equity
            << ",\"equity_liquidation\":" << equity << '}';
        return json_ok(out.str());
    }
    if (path == "/api/v1/performance") {
        return performance();
    }
    if (path == "/api/v1/regime") {
        return regime();
    }
    if (path == "/api/v1/latency") {
        return latency();
    }
    if (path == "/stream/v1/dashboard") {
        ApiRouteResponse response;
        response.status = 200;
        response.content_type = "text/event-stream";
        response.body = ": ready\n\n";
        return response;
    }
    return {};
}

ApiRouteResponse DashboardApiRoutes::latest_snapshot() const {
    const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
    if (!latest) {
        return json_ok(R"({"snapshot":null})");
    }
    return json_ok(dashboard_snapshot_json(*latest));
}

ApiRouteResponse DashboardApiRoutes::performance() const {
    const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
    return json_ok(
        latest ? performance_json(latest->performance) : performance_json({})
    );
}

ApiRouteResponse DashboardApiRoutes::regime() const {
    const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
    return json_ok(latest ? regime_json(latest->regime) : regime_json({}));
}

ApiRouteResponse DashboardApiRoutes::latency() const {
    const auto latest = store_ == nullptr ? std::nullopt : store_->latest();
    return json_ok(latest ? latency_json(latest->latency) : latency_json({}));
}

std::string dashboard_snapshot_json(
    const trading_engine::paper::DashboardSnapshot& snapshot
) {
    std::ostringstream out;
    out << "{\"seq_no\":" << snapshot.seq_no
        << ",\"ts_ns\":" << snapshot.ts_ns
        << ",\"account\":{\"starting_cash_tick\":"
        << snapshot.account.starting_cash_tick
        << ",\"cash_balance_tick\":" << snapshot.account.cash_balance_tick
        << ",\"reserved_cash_tick\":" << snapshot.account.reserved_cash_tick
        << ",\"realized_pnl_tick\":" << snapshot.account.realized_pnl_tick
        << ",\"unrealized_pnl_tick\":"
        << snapshot.account.unrealized_pnl_tick << "},\"performance\":"
        << performance_json(snapshot.performance)
        << ",\"regime\":" << regime_json(snapshot.regime)
        << ",\"latency\":" << latency_json(snapshot.latency)
        << ",\"signal\":{\"intents_published\":"
        << snapshot.signal.intents_published
        << ",\"paper_opportunities\":"
        << snapshot.signal.paper_opportunities
        << ",\"rejected\":" << snapshot.signal.rejected
        << ",\"output_hash\":" << snapshot.signal.output_hash
        << "},\"risk\":{\"decisions\":" << snapshot.risk.decisions
        << ",\"approved\":" << snapshot.risk.approved
        << ",\"rejected\":" << snapshot.risk.rejected
        << ",\"output_hash\":" << snapshot.risk.output_hash
        << "},\"execution\":{\"plans_created\":"
        << snapshot.execution.plans_created
        << ",\"plans_filled\":" << snapshot.execution.plans_filled
        << ",\"plans_failed\":" << snapshot.execution.plans_failed
        << ",\"output_hash\":" << snapshot.execution.output_hash
        << "},\"filled_orders\":"
        << filled_orders_json(snapshot.filled_orders) << '}';
    return out.str();
}

}  // namespace trading_engine::paper_backend

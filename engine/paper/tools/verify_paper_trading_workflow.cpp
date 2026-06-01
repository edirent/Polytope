#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ReservationDisposition.h"
#include "engine/paper/ledger/FillApplication.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/metrics/PerformanceMetricsEngine.h"
#include "engine/paper/pnl/PaperPnLEngine.h"
#include "engine/paper/portfolio/PaperPortfolio.h"
#include "engine/paper/read/DashboardReadStore.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/state/view/MarketDepthView.h"

#include <boost/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace json = boost::json;
namespace execution = trading_engine::execution;
namespace paper = trading_engine::paper;
namespace state = trading_engine::state;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Args {
    std::filesystem::path execution_reports;
    std::filesystem::path reservation_dispositions;
    std::filesystem::path snapshots;
    std::int64_t starting_cash = 0;
    bool check_determinism = false;
};

struct FillRecord {
    paper::FillApplication fill;
};

struct MarkSnapshotRecord {
    state::MarketStateSnapshot snapshot;
    std::uint32_t asset_index = 0;
};

struct WorkflowSummary {
    std::uint64_t reports_loaded = 0;
    std::uint64_t fills_applied = 0;
    std::uint64_t duplicate_reports_ignored = 0;
    std::uint64_t reservation_dispositions_loaded = 0;

    std::int64_t starting_cash = 0;
    std::int64_t ending_cash = 0;
    std::int64_t realized_pnl = 0;
    std::int64_t unrealized_pnl_mid = 0;
    std::int64_t unrealized_pnl_liquidation = 0;
    std::int64_t equity_mid = 0;
    std::int64_t equity_liquidation = 0;

    std::uint64_t returns_count = 0;
    paper::MetricStatus sharpe_status = paper::MetricStatus::InsufficientData;
    double sharpe = 0.0;
    std::int64_t max_drawdown = 0;

    std::uint64_t snapshots_published = 0;
    std::uint64_t latest_seq = 0;

    std::uint64_t paper_ledger_hash = kFnvOffset;
    std::uint64_t equity_curve_hash = kFnvOffset;
    std::uint64_t dashboard_snapshot_hash = kFnvOffset;
    bool determinism_passed = true;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & 0xffU;
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

void mix_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const auto ch : value) {
        *hash ^= static_cast<unsigned char>(ch);
        *hash *= kFnvPrime;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open " + path.string());
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

json::value parse_json_text(
    const std::string& text,
    const std::string& label
) {
    boost::json::error_code error;
    auto parsed = json::parse(text, error);
    if (error) {
        fail("failed to parse " + label + ": " + error.message());
    }
    return parsed;
}

const json::object& as_object(
    const json::value& value,
    const std::string& label
) {
    if (!value.is_object()) {
        fail(label + " must be a JSON object");
    }
    return value.as_object();
}

std::string get_string(
    const json::object& object,
    const char* key,
    std::string fallback = {}
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (!value->is_string()) {
        fail(std::string{"expected string for "} + key);
    }
    return std::string{value->as_string().c_str()};
}

bool get_bool(
    const json::object& object,
    const char* key,
    bool fallback = false
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (!value->is_bool()) {
        fail(std::string{"expected bool for "} + key);
    }
    return value->as_bool();
}

std::uint64_t get_u64(
    const json::object& object,
    const char* key,
    std::uint64_t fallback = 0
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (value->is_uint64()) {
        return value->as_uint64();
    }
    if (value->is_int64() && value->as_int64() >= 0) {
        return static_cast<std::uint64_t>(value->as_int64());
    }
    fail(std::string{"expected non-negative integer for "} + key);
}

std::uint32_t get_u32(
    const json::object& object,
    const char* key,
    std::uint32_t fallback = 0
) {
    const auto value = get_u64(object, key, fallback);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        fail(std::string{"integer too large for "} + key);
    }
    return static_cast<std::uint32_t>(value);
}

std::int64_t get_i64(
    const json::object& object,
    const char* key,
    std::int64_t fallback = 0
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (value->is_int64()) {
        return value->as_int64();
    }
    if (value->is_uint64() &&
        value->as_uint64() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value->as_uint64());
    }
    fail(std::string{"expected integer for "} + key);
}

double get_double(
    const json::object& object,
    const char* key,
    double fallback = 0.0
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (value->is_double()) {
        return value->as_double();
    }
    if (value->is_int64()) {
        return static_cast<double>(value->as_int64());
    }
    if (value->is_uint64()) {
        return static_cast<double>(value->as_uint64());
    }
    fail(std::string{"expected number for "} + key);
}

execution::ChildOrderStatus parse_child_status(const std::string& value) {
    if (value == "Filled") {
        return execution::ChildOrderStatus::Filled;
    }
    if (value == "PartiallyFilled") {
        return execution::ChildOrderStatus::PartiallyFilled;
    }
    if (value == "Failed") {
        return execution::ChildOrderStatus::Failed;
    }
    if (value == "Expired") {
        return execution::ChildOrderStatus::Expired;
    }
    if (value == "Cancelled" || value == "Canceled") {
        return execution::ChildOrderStatus::Cancelled;
    }
    if (value == "Acked") {
        return execution::ChildOrderStatus::Acked;
    }
    if (value == "Sent" || value == "Submitted") {
        return execution::ChildOrderStatus::Sent;
    }
    return execution::ChildOrderStatus::Created;
}

execution::OrderSide parse_side(const std::string& value) {
    if (value == "Sell") {
        return execution::OrderSide::Sell;
    }
    return execution::OrderSide::Buy;
}

execution::ReservationDispositionType parse_disposition_type(
    const std::string& value
) {
    if (value == "Consume") {
        return execution::ReservationDispositionType::Consume;
    }
    if (value == "Release") {
        return execution::ReservationDispositionType::Release;
    }
    if (value == "Expire") {
        return execution::ReservationDispositionType::Expire;
    }
    return execution::ReservationDispositionType::None;
}

state::PriceLevel parse_price_level(const json::object& object) {
    state::PriceLevel level;
    level.price_tick = get_i64(object, "price_tick");
    level.price = get_double(object, "price");
    level.size = get_double(object, "size");
    return level;
}

void parse_price_levels(
    const json::object& object,
    const char* key,
    std::array<state::PriceLevel, state::kMaxSnapshotDepth>* out,
    std::uint32_t* count
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || value->is_null()) {
        return;
    }
    if (!value->is_array()) {
        fail(std::string{"expected array for "} + key);
    }

    std::uint32_t index = 0;
    for (const auto& item : value->as_array()) {
        if (index >= state::kMaxSnapshotDepth) {
            break;
        }
        (*out)[index++] = parse_price_level(as_object(item, key));
    }
    *count = index;
}

FillRecord parse_fill_record(const json::object& object) {
    FillRecord record;
    auto& fill = record.fill;
    fill.report.plan_id = get_u64(object, "plan_id");
    fill.report.child_order_id = get_u64(object, "child_order_id");
    fill.report.status =
        parse_child_status(get_string(object, "status", "Created"));
    fill.report.venue_order_id = get_string(object, "venue_order_id");
    fill.report.reject_reason = get_string(object, "reject_reason");
    fill.report.filled_lots = get_i64(object, "filled_lots");
    fill.report.remaining_lots = get_i64(object, "remaining_lots");
    fill.report.avg_fill_price_tick = get_i64(object, "avg_fill_price_tick");
    fill.report.event_ts_ns = get_u64(object, "event_ts_ns");
    fill.market_id = get_string(object, "market_id");
    fill.asset_id = get_string(object, "asset_id");
    fill.asset_index = get_u32(object, "asset_index");
    fill.side = parse_side(get_string(object, "side", "Buy"));
    fill.fee_tick = get_i64(object, "fee_tick");
    return record;
}

std::vector<FillRecord> load_fill_records(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::vector<FillRecord> records;
    std::string line;
    std::uint64_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        const auto parsed = parse_json_text(
            line,
            path.string() + ":" + std::to_string(line_no)
        );
        records.push_back(parse_fill_record(as_object(parsed, "execution report")));
    }
    return records;
}

std::vector<execution::ReservationDisposition> load_dispositions(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::vector<execution::ReservationDisposition> dispositions;
    std::string line;
    std::uint64_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        const auto parsed = parse_json_text(
            line,
            path.string() + ":" + std::to_string(line_no)
        );
        const auto& object = as_object(parsed, "reservation disposition");
        execution::ReservationDisposition disposition;
        disposition.reservation_id = get_string(object, "reservation_id");
        disposition.plan_id = get_u64(object, "plan_id");
        disposition.type =
            parse_disposition_type(get_string(object, "type", "None"));
        disposition.reason = get_string(object, "reason");
        dispositions.push_back(std::move(disposition));
    }
    return dispositions;
}

std::vector<MarkSnapshotRecord> load_mark_snapshots(
    const std::filesystem::path& path
) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "snapshot fixture");
    const auto* snapshots_value = root.if_contains("snapshots");
    if (snapshots_value == nullptr || !snapshots_value->is_array()) {
        fail("snapshot fixture must contain snapshots array");
    }

    std::vector<MarkSnapshotRecord> records;
    for (const auto& value : snapshots_value->as_array()) {
        const auto& object = as_object(value, "snapshot");
        MarkSnapshotRecord record;
        auto& snapshot = record.snapshot;
        snapshot.entity_id = get_string(object, "entity_id");
        snapshot.market_id = get_string(object, "market_id");
        record.asset_index = get_u32(object, "asset_index");
        snapshot.version = get_u64(object, "version");
        snapshot.last_book_update_ns = get_u64(object, "last_book_update_ns");
        snapshot.live = get_bool(object, "live", true);
        snapshot.recovering = get_bool(object, "recovering");
        snapshot.closed = get_bool(object, "closed");
        snapshot.resolved = get_bool(object, "resolved");
        snapshot.crossed = get_bool(object, "crossed");
        snapshot.usable_for_depth = get_bool(object, "usable_for_depth", true);
        snapshot.usable_for_signal = get_bool(object, "usable_for_signal");
        snapshot.state_hash = get_u64(object, "state_hash");
        snapshot.snapshot_version_hash =
            get_u64(object, "snapshot_version_hash", snapshot.state_hash);
        snapshot.best_bid_tick = get_i64(object, "best_bid_tick");
        snapshot.best_ask_tick = get_i64(object, "best_ask_tick");
        snapshot.has_bid = get_bool(object, "has_bid");
        snapshot.has_ask = get_bool(object, "has_ask");
        snapshot.bid_count = get_u32(object, "bid_count");
        snapshot.ask_count = get_u32(object, "ask_count");

        std::uint32_t parsed_bid_count = 0;
        std::uint32_t parsed_ask_count = 0;
        parse_price_levels(object, "bids", &snapshot.bids, &parsed_bid_count);
        parse_price_levels(object, "asks", &snapshot.asks, &parsed_ask_count);
        if (parsed_bid_count > 0) {
            snapshot.bid_count = parsed_bid_count;
            snapshot.has_bid = true;
            snapshot.best_bid_tick = snapshot.bids[0].price_tick;
        }
        if (parsed_ask_count > 0) {
            snapshot.ask_count = parsed_ask_count;
            snapshot.has_ask = true;
            snapshot.best_ask_tick = snapshot.asks[0].price_tick;
        }

        records.push_back(std::move(record));
    }
    return records;
}

void upsert_depth_view(
    std::vector<state::MarketDepthView>* views,
    const MarkSnapshotRecord& record
) {
    auto view = state::market_depth_view_from_snapshot(
        record.snapshot,
        record.asset_index
    );
    for (auto& existing : *views) {
        if (existing.asset_index == view.asset_index) {
            existing = view;
            return;
        }
    }
    views->push_back(view);
}

std::uint64_t hash_ledger(const paper::PaperLedger& ledger) {
    std::uint64_t hash = kFnvOffset;
    const auto snapshot = ledger.snapshot();
    mix_i64(&hash, snapshot.cash.starting_cash_tick);
    mix_i64(&hash, snapshot.cash.cash_tick);
    mix_i64(&hash, snapshot.cash.reserved_cash_tick);
    mix_i64(&hash, snapshot.cash.realized_pnl_tick);
    mix_i64(&hash, snapshot.cash.fees_paid_tick);
    mix_u64(&hash, snapshot.applied_execution_report_count);
    mix_u64(&hash, snapshot.position_count);
    for (const auto& [asset_id, position] :
         ledger.position_ledger().positions()) {
        mix_string(&hash, asset_id);
        mix_u64(&hash, position.asset_index);
        mix_i64(&hash, position.qty_lots);
        mix_i64(&hash, position.avg_cost_tick);
        mix_i64(&hash, position.cost_basis_tick);
    }
    return hash;
}

std::uint64_t hash_equity_curve(std::span<const paper::EquityCurve> curve) {
    std::uint64_t hash = kFnvOffset;
    for (const auto& point : curve) {
        mix_i64(&hash, point.equity_mid_tick);
        mix_i64(&hash, point.equity_liquidation_tick);
        mix_i64(&hash, point.realized_pnl_tick);
        mix_i64(&hash, point.unrealized_pnl_mid_tick);
        mix_i64(&hash, point.unrealized_pnl_liquidation_tick);
        mix_u64(&hash, point.ts_ns);
    }
    return hash;
}

std::uint64_t hash_dashboard(const paper::DashboardSnapshot& snapshot) {
    std::uint64_t hash = kFnvOffset;
    mix_u64(&hash, snapshot.seq_no);
    mix_u64(&hash, snapshot.ts_ns);
    mix_i64(&hash, snapshot.account.starting_cash_tick);
    mix_i64(&hash, snapshot.account.cash_balance_tick);
    mix_i64(&hash, snapshot.account.realized_pnl_tick);
    mix_i64(&hash, snapshot.account.unrealized_pnl_tick);
    mix_u64(&hash, snapshot.performance.intents_observed);
    mix_u64(&hash, snapshot.performance.execution_reports_observed);
    mix_i64(&hash, snapshot.performance.gross_pnl_tick);
    mix_i64(&hash, snapshot.performance.net_pnl_tick);
    mix_i64(&hash, snapshot.performance.max_drawdown_tick);
    mix_u64(&hash, static_cast<std::uint64_t>(snapshot.regime.data));
    mix_u64(&hash, static_cast<std::uint64_t>(snapshot.regime.liquidity));
    return hash;
}

const char* metric_status_name(paper::MetricStatus status) noexcept {
    switch (status) {
        case paper::MetricStatus::Ok:
            return "Ok";
        case paper::MetricStatus::InvalidInput:
            return "InvalidInput";
        case paper::MetricStatus::InsufficientData:
            return "InsufficientData";
    }
    return "Unknown";
}

paper::DashboardSnapshot build_dashboard(
    const paper::PaperLedger& ledger,
    const paper::PaperPnLResult& pnl,
    const paper::PerformanceMetricsResult& metrics,
    const WorkflowSummary& summary,
    std::uint64_t ts_ns
) {
    paper::DashboardSnapshot dashboard;
    dashboard.ts_ns = ts_ns;
    dashboard.account = ledger.account_snapshot();
    dashboard.account.unrealized_pnl_tick = pnl.unrealized_pnl_mid_tick;
    dashboard.account.updated_ts_ns = ts_ns;
    dashboard.performance.execution_reports_observed = summary.reports_loaded;
    dashboard.performance.gross_pnl_tick =
        pnl.realized_pnl_tick + pnl.unrealized_pnl_mid_tick;
    dashboard.performance.net_pnl_tick =
        dashboard.performance.gross_pnl_tick -
        ledger.cash_ledger().fees_paid_tick;
    dashboard.performance.max_drawdown_tick = metrics.drawdown.max_drawdown_tick;
    dashboard.performance.updated_ts_ns = ts_ns;
    return dashboard;
}

WorkflowSummary run_workflow_once(
    const std::vector<FillRecord>& records,
    const std::vector<execution::ReservationDisposition>& dispositions,
    const std::vector<MarkSnapshotRecord>& snapshots,
    std::int64_t starting_cash
) {
    paper::PaperLedger ledger{starting_cash};
    paper::PaperPortfolio portfolio;
    paper::PaperPnLEngine pnl_engine;
    paper::PerformanceMetricsEngine performance_engine;
    paper::DashboardReadStore dashboard_store{256};

    WorkflowSummary summary;
    summary.reports_loaded = records.size();
    summary.reservation_dispositions_loaded = dispositions.size();
    summary.starting_cash = starting_cash;

    for (const auto& record : records) {
        const auto apply = ledger.apply_fill(record.fill);
        if (apply.applied) {
            ++summary.fills_applied;
            (void)portfolio.apply_fill(record.fill);
        } else if (
            apply.status == paper::PaperLedgerApplyStatus::DuplicateExecutionReport
        ) {
            ++summary.duplicate_reports_ignored;
        }
    }

    std::vector<state::MarketDepthView> depth_views;
    std::vector<paper::EquityCurve> equity_curve;
    paper::PaperPnLResult pnl;
    paper::PerformanceMetricsResult performance;
    std::uint64_t last_ts_ns = 0;

    for (const auto& snapshot : snapshots) {
        upsert_depth_view(&depth_views, snapshot);
        last_ts_ns = snapshot.snapshot.last_book_update_ns;
        pnl = pnl_engine.compute(
            portfolio,
            ledger.cash_ledger(),
            std::span<const state::MarketDepthView>{
                depth_views.data(),
                depth_views.size()
            },
            last_ts_ns
        );
        equity_curve.push_back(pnl.equity);

        paper::PerformanceMetricsInput input;
        input.equity_curve = std::span<const paper::EquityCurve>{
            equity_curve.data(),
            equity_curve.size()
        };
        input.plans_created = summary.fills_applied;
        input.filled_plans = summary.fills_applied;
        input.filled_notional_tick =
            starting_cash - ledger.cash_ledger().cash_tick;
        performance = performance_engine.compute(input);

        (void)dashboard_store.publish(build_dashboard(
            ledger,
            pnl,
            performance,
            summary,
            last_ts_ns
        ));
    }

    if (snapshots.empty()) {
        pnl = pnl_engine.compute(portfolio, ledger.cash_ledger(), {}, 0);
        performance = performance_engine.compute({});
        (void)dashboard_store.publish(build_dashboard(
            ledger,
            pnl,
            performance,
            summary,
            0
        ));
    }

    const auto account = ledger.account_snapshot();
    summary.ending_cash = account.cash_balance_tick;
    summary.realized_pnl = account.realized_pnl_tick;
    summary.unrealized_pnl_mid = pnl.unrealized_pnl_mid_tick;
    summary.unrealized_pnl_liquidation =
        pnl.unrealized_pnl_liquidation_tick;
    summary.equity_mid = pnl.equity.equity_mid_tick;
    summary.equity_liquidation = pnl.equity.equity_liquidation_tick;
    summary.returns_count = performance.return_samples;
    summary.sharpe_status = performance.sharpe.status;
    summary.sharpe = performance.sharpe.value;
    summary.max_drawdown = performance.drawdown.max_drawdown_tick;
    summary.snapshots_published = dashboard_store.latest_seq();
    summary.latest_seq = dashboard_store.latest_seq();
    summary.paper_ledger_hash = hash_ledger(ledger);
    summary.equity_curve_hash = hash_equity_curve(equity_curve);
    if (const auto latest = dashboard_store.latest()) {
        summary.dashboard_snapshot_hash = hash_dashboard(*latest);
    }
    return summary;
}

bool deterministic_equal(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.reports_loaded == rhs.reports_loaded &&
           lhs.fills_applied == rhs.fills_applied &&
           lhs.duplicate_reports_ignored == rhs.duplicate_reports_ignored &&
           lhs.ending_cash == rhs.ending_cash &&
           lhs.unrealized_pnl_mid == rhs.unrealized_pnl_mid &&
           lhs.unrealized_pnl_liquidation ==
               rhs.unrealized_pnl_liquidation &&
           lhs.equity_mid == rhs.equity_mid &&
           lhs.equity_liquidation == rhs.equity_liquidation &&
           lhs.paper_ledger_hash == rhs.paper_ledger_hash &&
           lhs.equity_curve_hash == rhs.equity_curve_hash &&
           lhs.dashboard_snapshot_hash == rhs.dashboard_snapshot_hash;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + name);
            }
            return argv[++index];
        };

        if (arg == "--execution-reports") {
            args.execution_reports = require_value("--execution-reports");
        } else if (arg == "--reservation-dispositions") {
            args.reservation_dispositions =
                require_value("--reservation-dispositions");
        } else if (arg == "--snapshots") {
            args.snapshots = require_value("--snapshots");
        } else if (arg == "--starting-cash") {
            args.starting_cash = std::stoll(require_value("--starting-cash"));
        } else if (arg == "--check-determinism") {
            args.check_determinism = true;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.execution_reports.empty()) {
        fail("--execution-reports is required");
    }
    if (args.reservation_dispositions.empty()) {
        fail("--reservation-dispositions is required");
    }
    if (args.snapshots.empty()) {
        fail("--snapshots is required");
    }
    return args;
}

void print_summary(const WorkflowSummary& summary) {
    std::cout
        << "paper_workflow:\n"
        << "  reports_loaded: " << summary.reports_loaded << "\n"
        << "  fills_applied: " << summary.fills_applied << "\n"
        << "  duplicate_reports_ignored: "
        << summary.duplicate_reports_ignored << "\n\n"
        << "account:\n"
        << "  starting_cash: " << summary.starting_cash << "\n"
        << "  ending_cash: " << summary.ending_cash << "\n"
        << "  realized_pnl: " << summary.realized_pnl << "\n"
        << "  unrealized_pnl_mid: " << summary.unrealized_pnl_mid << "\n"
        << "  unrealized_pnl_liquidation: "
        << summary.unrealized_pnl_liquidation << "\n"
        << "  equity_mid: " << summary.equity_mid << "\n"
        << "  equity_liquidation: " << summary.equity_liquidation << "\n\n"
        << "performance:\n"
        << "  returns_count: " << summary.returns_count << "\n"
        << "  sharpe_status: "
        << metric_status_name(summary.sharpe_status) << "\n"
        << "  sharpe: " << summary.sharpe << "\n"
        << "  max_drawdown: " << summary.max_drawdown << "\n\n"
        << "dashboard:\n"
        << "  snapshots_published: " << summary.snapshots_published << "\n"
        << "  latest_seq: " << summary.latest_seq << "\n\n"
        << "hashes:\n"
        << "  paper_ledger_hash: " << summary.paper_ledger_hash << "\n"
        << "  equity_curve_hash: " << summary.equity_curve_hash << "\n"
        << "  dashboard_snapshot_hash: "
        << summary.dashboard_snapshot_hash << "\n"
        << "  determinism_passed: "
        << (summary.determinism_passed ? "true" : "false") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto reports = load_fill_records(args.execution_reports);
        const auto dispositions =
            load_dispositions(args.reservation_dispositions);
        const auto snapshots = load_mark_snapshots(args.snapshots);

        auto summary = run_workflow_once(
            reports,
            dispositions,
            snapshots,
            args.starting_cash
        );
        if (args.check_determinism) {
            const auto second = run_workflow_once(
                reports,
                dispositions,
                snapshots,
                args.starting_cash
            );
            summary.determinism_passed = deterministic_equal(summary, second);
        }

        print_summary(summary);
        return summary.determinism_passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}

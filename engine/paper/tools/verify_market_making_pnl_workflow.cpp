#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/metrics/MakerPerformanceMetrics.h"
#include "engine/paper/pnl/AdverseSelectionTracker.h"
#include "engine/paper/pnl/MakerPnLEngine.h"
#include "engine/risk/public/ApprovedQuote.h"
#include "engine/state/book/DepthPrefix.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace execution = trading_engine::execution;
namespace json = boost::json;
namespace paper = trading_engine::paper;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;
namespace mm = trading_engine::strategy::market_making;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Args {
    std::filesystem::path approved_quotes;
    std::filesystem::path market_events;
    std::filesystem::path snapshots;
    std::int64_t starting_cash = 0;
    execution::PaperMakerFillMode fill_mode =
        execution::PaperMakerFillMode::Conservative;
    bool check_determinism = false;
};

struct MarketEventRecord {
    execution::PaperMakerMarketEvent event;
    bool duplicate_last_report = false;
};

struct SnapshotRecord {
    state::MarketDepthView view;
    std::uint64_t ts_ns = 0;
};

struct TimedEvent {
    enum class Type : std::uint8_t {
        Snapshot,
        Market
    };

    std::uint64_t ts_ns = 0;
    Type type = Type::Snapshot;
    std::size_t index = 0;
};

struct WorkflowSummary {
    std::uint64_t approved_quotes = 0;
    std::uint64_t maker_reports = 0;
    std::uint64_t maker_fills = 0;
    std::uint64_t duplicate_reports_ignored = 0;

    std::int64_t starting_cash = 0;
    std::int64_t ending_cash = 0;
    std::int64_t fees_paid = 0;
    std::int64_t realized_pnl = 0;
    std::size_t position_count = 0;
    std::int64_t open_position_qty = 0;

    paper::MakerPnLSnapshot pnl;
    paper::MakerPerformanceMetricsSnapshot metrics;

    std::uint64_t maker_ledger_hash = kFnvOffset;
    std::uint64_t maker_pnl_hash = kFnvOffset;
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

mm::QuoteSide parse_quote_side(const std::string& value) {
    return value == "Ask" ? mm::QuoteSide::Ask : mm::QuoteSide::Bid;
}

execution::OrderSide parse_order_side(const std::string& value) {
    return value == "Sell" ? execution::OrderSide::Sell
                           : execution::OrderSide::Buy;
}

execution::PaperMakerFillMode parse_fill_mode(const std::string& value) {
    if (value == "nofill" || value == "NoFill") {
        return execution::PaperMakerFillMode::NoFill;
    }
    if (value == "book-cross" || value == "BookCross") {
        return execution::PaperMakerFillMode::BookCross;
    }
    if (value == "mid-cross" || value == "MidCross") {
        return execution::PaperMakerFillMode::MidCross;
    }
    if (value == "queue-aware" || value == "QueueAware") {
        return execution::PaperMakerFillMode::QueueAware;
    }
    if (value == "conservative" || value == "Conservative") {
        return execution::PaperMakerFillMode::Conservative;
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

mm::QuoteLeg parse_quote_leg(const json::object& object, mm::QuoteSide side) {
    mm::QuoteLeg leg;
    leg.market_id = get_string(object, "market_id");
    leg.asset_id = get_string(object, "asset_id");
    leg.market_index = get_u32(object, "market_index");
    leg.asset_index = get_u32(object, "asset_index");
    leg.side = parse_quote_side(
        get_string(object, "side", side == mm::QuoteSide::Ask ? "Ask" : "Bid")
    );
    leg.price_tick = get_i64(object, "price_tick");
    leg.quantity_lots = get_i64(object, "quantity_lots");
    leg.fair_value_tick = get_i64(object, "fair_value_tick");
    leg.edge_to_fair_tick = get_i64(object, "edge_to_fair_tick");
    leg.book_version = get_u64(object, "book_version");
    leg.snapshot_version_hash = get_u64(object, "snapshot_version_hash");
    return leg;
}

risk::ApprovedQuote parse_approved_quote(const json::object& object) {
    risk::ApprovedQuote quote;
    quote.approved_quote_id = get_u64(object, "approved_quote_id");
    quote.quote_intent_id = get_u64(object, "quote_intent_id");
    quote.quote_group_id = get_u64(object, "quote_group_id");
    quote.has_bid = get_bool(object, "has_bid");
    quote.has_ask = get_bool(object, "has_ask");
    if (quote.has_bid) {
        const auto* bid = object.if_contains("bid");
        if (bid == nullptr) {
            fail("approved quote missing bid object");
        }
        quote.bid = parse_quote_leg(as_object(*bid, "bid"), mm::QuoteSide::Bid);
    }
    if (quote.has_ask) {
        const auto* ask = object.if_contains("ask");
        if (ask == nullptr) {
            fail("approved quote missing ask object");
        }
        quote.ask = parse_quote_leg(as_object(*ask, "ask"), mm::QuoteSide::Ask);
    }
    quote.approved_ts_ns = get_u64(object, "approved_ts_ns");
    quote.expires_at_ns = get_u64(object, "expires_at_ns");
    quote.idempotency_hash = get_u64(object, "idempotency_hash");
    quote.policy_hash = get_u64(object, "policy_hash");
    quote.snapshot_version_hash = get_u64(object, "snapshot_version_hash");
    if (quote.approved_quote_id == 0) {
        quote.approved_quote_id = risk::compute_approved_quote_hash(quote);
    }
    return quote;
}

MarketEventRecord parse_market_event(const json::object& object) {
    MarketEventRecord record;
    record.event.ts_ns = get_u64(object, "ts_ns");
    record.event.asset_index = get_u32(object, "asset_index");
    record.event.has_trade = get_bool(object, "has_trade");
    record.event.trade_price_tick = get_i64(object, "trade_price_tick");
    record.event.trade_qty_lots = get_i64(object, "trade_qty_lots");
    record.event.trade_aggressor_side =
        parse_order_side(get_string(object, "trade_aggressor_side", "Buy"));
    record.duplicate_last_report =
        get_bool(object, "duplicate_last_report");
    return record;
}

state::PriceLevel price_level(std::int64_t price_tick, double size) {
    state::PriceLevel level;
    level.price_tick = price_tick;
    level.price = static_cast<double>(price_tick) / 1'000'000.0;
    level.size = size;
    return level;
}

SnapshotRecord parse_snapshot(const json::object& object) {
    SnapshotRecord record;
    record.ts_ns = get_u64(object, "ts_ns");
    auto& view = record.view;
    view.asset_index = get_u32(object, "asset_index");
    view.book_version = get_u64(object, "book_version", record.ts_ns);
    view.snapshot_version_hash =
        get_u64(object, "snapshot_version_hash", view.book_version);
    view.last_ws_recv_ns = record.ts_ns;
    view.usable_for_depth = get_bool(object, "usable_for_depth", true);
    view.recovering = get_bool(object, "recovering");
    view.crossed = get_bool(object, "crossed");
    view.closed = get_bool(object, "closed");
    view.resolved = get_bool(object, "resolved");

    if (const auto bid = get_i64(object, "bid_tick", 0); bid > 0) {
        view.bid_count = 1;
        view.bids[0] = price_level(bid, get_double(object, "bid_size", 100.0));
    }
    if (const auto ask = get_i64(object, "ask_tick", 0); ask > 0) {
        view.ask_count = 1;
        view.asks[0] = price_level(ask, get_double(object, "ask_size", 100.0));
    }

    state::build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return record;
}

template <typename T, typename Parser>
std::vector<T> load_jsonl(const std::filesystem::path& path, Parser parser) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::vector<T> records;
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
        records.push_back(parser(as_object(parsed, path.string())));
    }
    return records;
}

void upsert_depth_view(
    std::vector<state::MarketDepthView>* views,
    const state::MarketDepthView& view
) {
    for (auto& existing : *views) {
        if (existing.asset_index == view.asset_index) {
            existing = view;
            return;
        }
    }
    views->push_back(view);
}

const state::MarketDepthView* find_depth(
    std::span<const state::MarketDepthView> views,
    std::uint32_t asset_index
) {
    for (const auto& view : views) {
        if (view.asset_index == asset_index) {
            return &view;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> mid_from_depth(
    const state::MarketDepthView* view
) {
    if (view == nullptr || view->bid_count == 0 || view->ask_count == 0) {
        return std::nullopt;
    }
    return (view->bids[0].price_tick + view->asks[0].price_tick) / 2;
}

std::uint64_t hash_ledger(const paper::PaperLedger& ledger) {
    std::uint64_t hash = kFnvOffset;
    const auto snapshot = ledger.snapshot();
    mix_i64(&hash, snapshot.cash.starting_cash_tick);
    mix_i64(&hash, snapshot.cash.cash_tick);
    mix_i64(&hash, snapshot.cash.realized_pnl_tick);
    mix_i64(&hash, snapshot.cash.fees_paid_tick);
    mix_u64(&hash, snapshot.applied_fill_count);
    mix_u64(&hash, snapshot.maker_fill_count);
    for (const auto& [asset_id, position] :
         ledger.position_ledger().positions()) {
        mix_string(&hash, asset_id);
        mix_u64(&hash, position.asset_index);
        mix_i64(&hash, position.qty_lots);
        mix_i64(&hash, position.avg_cost_tick);
        mix_i64(&hash, position.cost_basis_tick);
        mix_i64(&hash, position.realized_pnl_tick);
    }
    return hash;
}

std::uint64_t hash_pnl(const paper::MakerPnLSnapshot& pnl) {
    std::uint64_t hash = kFnvOffset;
    mix_u64(&hash, pnl.ts_ns);
    mix_i64(&hash, pnl.maker_realized_pnl_tick);
    mix_i64(&hash, pnl.maker_unrealized_pnl_mid_tick);
    mix_i64(&hash, pnl.maker_unrealized_pnl_liquidation_tick);
    mix_i64(&hash, pnl.equity_mid_tick);
    mix_i64(&hash, pnl.equity_liquidation_tick);
    mix_i64(&hash, pnl.cash_tick);
    mix_i64(&hash, pnl.fees_paid_tick);
    mix_u64(&hash, pnl.maker_fill_count);
    mix_u64(&hash, static_cast<std::uint64_t>(pnl.mark_quality));
    return hash;
}

void process_report(
    const execution::MakerExecutionReport& report,
    paper::PaperEventAdapter* adapter,
    paper::PaperLedger* ledger,
    paper::AdverseSelectionTracker* adverse_selection,
    std::vector<paper::MakerFillMetricInput>* fill_metrics,
    std::span<const state::MarketDepthView> depth_views,
    WorkflowSummary* summary
) {
    ++summary->maker_reports;
    const auto observed = adapter->observe(report);
    if (!observed.has_paper_fill) {
        return;
    }

    const auto apply = ledger->apply_fill(observed.paper_fill);
    if (apply.applied) {
        ++summary->maker_fills;
        const auto mid = mid_from_depth(
            find_depth(depth_views, observed.paper_fill.asset_index)
        );
        adverse_selection->observe_fill(observed.paper_fill, mid);
        fill_metrics->push_back(
            paper::MakerFillMetricInput{
                .fill = observed.paper_fill,
                .mid_at_fill_tick = mid
            }
        );
    } else if (
        apply.status == paper::PaperLedgerApplyStatus::DuplicateIgnored ||
        apply.status == paper::PaperLedgerApplyStatus::DuplicateExecutionReport
    ) {
        ++summary->duplicate_reports_ignored;
    }
}

WorkflowSummary run_workflow_once(
    const std::vector<risk::ApprovedQuote>& approved_quotes,
    const std::vector<MarketEventRecord>& market_events,
    const std::vector<SnapshotRecord>& snapshots,
    const Args& args
) {
    execution::PaperMakerExecutionAdapter maker_adapter{args.fill_mode};
    paper::PaperEventAdapter paper_adapter;
    paper::PaperLedger ledger{args.starting_cash};
    paper::MakerPnLEngine pnl_engine;
    paper::AdverseSelectionTracker adverse_selection;
    std::vector<paper::MakerFillMetricInput> fill_metrics;
    std::vector<state::MarketDepthView> depth_views;

    WorkflowSummary summary;
    summary.approved_quotes = approved_quotes.size();
    summary.starting_cash = args.starting_cash;

    for (const auto& quote : approved_quotes) {
        (void)paper_adapter.observe(quote);
        const auto submit =
            maker_adapter.submit_approved_quote(quote, quote.approved_ts_ns);
        if (!submit.ok) {
            fail("failed to submit approved quote: " + submit.error);
        }
    }

    std::vector<TimedEvent> events;
    events.reserve(snapshots.size() + market_events.size());
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        events.push_back({snapshots[i].ts_ns, TimedEvent::Type::Snapshot, i});
    }
    for (std::size_t i = 0; i < market_events.size(); ++i) {
        events.push_back({
            market_events[i].event.ts_ns,
            TimedEvent::Type::Market,
            i
        });
    }
    std::sort(events.begin(), events.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.ts_ns != rhs.ts_ns) {
            return lhs.ts_ns < rhs.ts_ns;
        }
        return static_cast<std::uint8_t>(lhs.type) <
               static_cast<std::uint8_t>(rhs.type);
    });

    std::uint64_t last_ts_ns = 0;
    for (const auto& timed : events) {
        last_ts_ns = timed.ts_ns;
        if (timed.type == TimedEvent::Type::Snapshot) {
            const auto& snapshot = snapshots[timed.index];
            upsert_depth_view(&depth_views, snapshot.view);
            adverse_selection.observe_mark(
                snapshot.view.asset_index,
                snapshot.ts_ns,
                mid_from_depth(&snapshot.view)
            );
            continue;
        }

        auto event = market_events[timed.index].event;
        event.depth = find_depth(
            std::span<const state::MarketDepthView>{
                depth_views.data(),
                depth_views.size()
            },
            event.asset_index
        );
        const auto reports = maker_adapter.on_market_event(event);
        for (const auto& report : reports) {
            process_report(
                report,
                &paper_adapter,
                &ledger,
                &adverse_selection,
                &fill_metrics,
                std::span<const state::MarketDepthView>{
                    depth_views.data(),
                    depth_views.size()
                },
                &summary
            );
        }
        if (market_events[timed.index].duplicate_last_report &&
            !reports.empty()) {
            process_report(
                reports.back(),
                &paper_adapter,
                &ledger,
                &adverse_selection,
                &fill_metrics,
                std::span<const state::MarketDepthView>{
                    depth_views.data(),
                    depth_views.size()
                },
                &summary
            );
        }
    }

    summary.pnl = pnl_engine.compute(
        ledger,
        std::span<const state::MarketDepthView>{
            depth_views.data(),
            depth_views.size()
        },
        last_ts_ns
    );

    paper::MakerPerformanceMetricsInput metrics_input;
    metrics_input.fills = std::span<const paper::MakerFillMetricInput>{
        fill_metrics.data(),
        fill_metrics.size()
    };
    metrics_input.adverse_selection_records = adverse_selection.records();
    metrics_input.maker_realized_pnl_tick =
        summary.pnl.maker_realized_pnl_tick;
    metrics_input.maker_unrealized_pnl_mid_tick =
        summary.pnl.maker_unrealized_pnl_mid_tick;
    metrics_input.maker_unrealized_pnl_liquidation_tick =
        summary.pnl.maker_unrealized_pnl_liquidation_tick;
    metrics_input.quote_count = approved_quotes.size();
    metrics_input.cancel_replace_count =
        maker_adapter.quote_book().replaced_quote_count();
    summary.metrics = paper::MakerPerformanceMetrics{}.compute(metrics_input);

    const auto ledger_snapshot = ledger.snapshot();
    summary.ending_cash = ledger_snapshot.cash.cash_tick;
    summary.fees_paid = ledger_snapshot.cash.fees_paid_tick;
    summary.realized_pnl = ledger_snapshot.cash.realized_pnl_tick;
    summary.position_count = ledger_snapshot.position_count;
    for (const auto& [_, position] : ledger.position_ledger().positions()) {
        summary.open_position_qty += position.qty_lots;
    }
    summary.maker_ledger_hash = hash_ledger(ledger);
    summary.maker_pnl_hash = hash_pnl(summary.pnl);
    return summary;
}

bool deterministic_equal(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.approved_quotes == rhs.approved_quotes &&
           lhs.maker_reports == rhs.maker_reports &&
           lhs.maker_fills == rhs.maker_fills &&
           lhs.duplicate_reports_ignored == rhs.duplicate_reports_ignored &&
           lhs.ending_cash == rhs.ending_cash &&
           lhs.fees_paid == rhs.fees_paid &&
           lhs.realized_pnl == rhs.realized_pnl &&
           lhs.open_position_qty == rhs.open_position_qty &&
           lhs.pnl.maker_unrealized_pnl_mid_tick ==
               rhs.pnl.maker_unrealized_pnl_mid_tick &&
           lhs.pnl.equity_mid_tick == rhs.pnl.equity_mid_tick &&
           lhs.metrics.spread_capture_tick ==
               rhs.metrics.spread_capture_tick &&
           lhs.maker_ledger_hash == rhs.maker_ledger_hash &&
           lhs.maker_pnl_hash == rhs.maker_pnl_hash;
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

        if (arg == "--approved-quotes") {
            args.approved_quotes = require_value("--approved-quotes");
        } else if (arg == "--market-events") {
            args.market_events = require_value("--market-events");
        } else if (arg == "--snapshots") {
            args.snapshots = require_value("--snapshots");
        } else if (arg == "--starting-cash") {
            args.starting_cash = std::stoll(require_value("--starting-cash"));
        } else if (arg == "--fill-mode") {
            args.fill_mode = parse_fill_mode(require_value("--fill-mode"));
        } else if (arg == "--check-determinism") {
            args.check_determinism = true;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.approved_quotes.empty()) {
        fail("--approved-quotes is required");
    }
    if (args.market_events.empty()) {
        fail("--market-events is required");
    }
    if (args.snapshots.empty()) {
        fail("--snapshots is required");
    }
    return args;
}

void print_summary(const WorkflowSummary& summary) {
    std::cout
        << "market_making_pnl:\n"
        << "  approved_quotes: " << summary.approved_quotes << "\n"
        << "  maker_reports: " << summary.maker_reports << "\n"
        << "  maker_fills: " << summary.maker_fills << "\n"
        << "  duplicate_reports_ignored: "
        << summary.duplicate_reports_ignored << "\n\n"
        << "ledger:\n"
        << "  starting_cash: " << summary.starting_cash << "\n"
        << "  ending_cash: " << summary.ending_cash << "\n"
        << "  fees_paid: " << summary.fees_paid << "\n"
        << "  realized_pnl: " << summary.realized_pnl << "\n"
        << "  position_count: " << summary.position_count << "\n"
        << "  open_position_qty: " << summary.open_position_qty << "\n\n"
        << "pnl:\n"
        << "  unrealized_pnl_mid: "
        << summary.pnl.maker_unrealized_pnl_mid_tick << "\n"
        << "  unrealized_pnl_liquidation: "
        << summary.pnl.maker_unrealized_pnl_liquidation_tick << "\n"
        << "  equity_mid: " << summary.pnl.equity_mid_tick << "\n"
        << "  equity_liquidation: "
        << summary.pnl.equity_liquidation_tick << "\n"
        << "  mark_quality: "
        << mark_quality_name(summary.pnl.mark_quality) << "\n\n"
        << "maker_metrics:\n"
        << "  spread_capture: " << summary.metrics.spread_capture_tick << "\n"
        << "  adverse_selection_5s: "
        << summary.metrics.adverse_selection_5s_tick << "\n"
        << "  adverse_selection_30s: "
        << summary.metrics.adverse_selection_30s_tick << "\n"
        << "  quote_fill_rate: " << summary.metrics.quote_fill_rate << "\n"
        << "  cancel_replace_rate: "
        << summary.metrics.cancel_replace_rate << "\n\n"
        << "hashes:\n"
        << "  maker_ledger_hash: " << summary.maker_ledger_hash << "\n"
        << "  maker_pnl_hash: " << summary.maker_pnl_hash << "\n"
        << "  determinism_passed: "
        << (summary.determinism_passed ? "true" : "false") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto approved_quotes = load_jsonl<risk::ApprovedQuote>(
            args.approved_quotes,
            parse_approved_quote
        );
        const auto market_events = load_jsonl<MarketEventRecord>(
            args.market_events,
            parse_market_event
        );
        const auto snapshots = load_jsonl<SnapshotRecord>(
            args.snapshots,
            parse_snapshot
        );

        auto summary = run_workflow_once(
            approved_quotes,
            market_events,
            snapshots,
            args
        );
        if (args.check_determinism) {
            const auto second = run_workflow_once(
                approved_quotes,
                market_events,
                snapshots,
                args
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

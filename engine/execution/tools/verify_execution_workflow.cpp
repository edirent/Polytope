#include "engine/execution/adapter/LiveExecutionAdapter.h"
#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/execution/core/ExecutionGateway.h"
#include "engine/execution/metrics/ExecutionMetrics.h"
#include "engine/execution/publish/CapturingExecutionPublisher.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"
#include "engine/state/MarketStateSnapshot.h"
#include "oracle/public/CandidateBundle.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace json = boost::json;
namespace execution = trading_engine::execution;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;
namespace oracle = trading_engine::oracle;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Args {
    std::filesystem::path approved_intents;
    std::filesystem::path source_intents;
    std::filesystem::path snapshots;
    std::filesystem::path config;
    bool check_determinism = false;
};

struct ExecutionConfigFixture {
    std::uint64_t now_ns = 0;
    bool cancel_after_hedge_required = false;
    execution::ExecutionConfig config;
};

struct ApprovedIntentRecord {
    std::uint64_t source_intent_id = 0;
    execution::ExecutionApproval approval;
};

struct WorkflowSummary {
    std::uint64_t approved_intents_loaded = 0;
    std::uint64_t plans_created = 0;
    std::uint64_t child_orders_created = 0;

    std::string adapter_mode = "paper";
    std::uint64_t plans_submitted = 0;
    std::uint64_t plans_filled = 0;
    std::uint64_t plans_failed = 0;
    std::uint64_t plans_expired = 0;
    std::uint64_t plans_hedge_required = 0;

    std::uint64_t child_orders_filled = 0;
    std::uint64_t child_orders_partially_filled = 0;
    std::int64_t total_filled_qty_lots = 0;
    std::int64_t total_filled_cost_tick = 0;

    std::uint64_t execution_reports_published = 0;
    std::uint64_t reservation_consumed = 0;
    std::uint64_t reservation_released = 0;
    std::uint64_t reservation_expired = 0;
    std::uint64_t reservation_hedge_required = 0;

    execution::ExecutionMetrics metrics;
    std::uint64_t execution_output_hash = kFnvOffset;
    bool determinism_passed = true;
};

class CapturingReservationDispositionPublisher final
    : public execution::ReservationDispositionPublisher {
public:
    void publish(
        const execution::ReservationDisposition& disposition
    ) override {
        dispositions_.push_back(disposition);
    }

    [[nodiscard]] const std::vector<execution::ReservationDisposition>&
    dispositions() const noexcept {
        return dispositions_;
    }

private:
    std::vector<execution::ReservationDisposition> dispositions_;
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

signal::IntentStatus parse_intent_status(const std::string& value) {
    if (value == "PaperOpportunity") {
        return signal::IntentStatus::PaperOpportunity;
    }
    if (value == "RejectedInvalidSettlement") {
        return signal::IntentStatus::RejectedInvalidSettlement;
    }
    if (value == "RejectedBadMarketState") {
        return signal::IntentStatus::RejectedBadMarketState;
    }
    if (value == "RejectedMissingSnapshot") {
        return signal::IntentStatus::RejectedMissingSnapshot;
    }
    if (value == "RejectedInsufficientDepth") {
        return signal::IntentStatus::RejectedInsufficientDepth;
    }
    if (value == "RejectedLowEdge") {
        return signal::IntentStatus::RejectedLowEdge;
    }
    if (value == "DuplicateIntent") {
        return signal::IntentStatus::DuplicateIntent;
    }
    return signal::IntentStatus::CandidateOnly;
}

oracle::Side parse_side(const std::string& value) {
    oracle::Side side = oracle::Side::Buy;
    if (!oracle::side_from_string(value, &side)) {
        fail("unsupported side: " + value);
    }
    return side;
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

std::vector<state::MarketStateSnapshot> load_snapshots(
    const std::filesystem::path& path
) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "snapshot fixture");
    const auto* snapshots_value = root.if_contains("snapshots");
    if (snapshots_value == nullptr || !snapshots_value->is_array()) {
        fail("snapshot fixture must contain snapshots array");
    }

    std::vector<state::MarketStateSnapshot> snapshots;
    for (const auto& value : snapshots_value->as_array()) {
        const auto& object = as_object(value, "snapshot");
        state::MarketStateSnapshot snapshot;
        snapshot.entity_id = get_string(object, "entity_id");
        snapshot.market_id = get_string(object, "market_id");
        snapshot.version = get_u64(object, "version");
        snapshot.last_book_update_ns =
            get_u64(object, "last_book_update_ns");
        snapshot.live = get_bool(object, "live");
        snapshot.recovering = get_bool(object, "recovering");
        snapshot.closed = get_bool(object, "closed");
        snapshot.resolved = get_bool(object, "resolved");
        snapshot.crossed = get_bool(object, "crossed");
        snapshot.has_bid = get_bool(object, "has_bid");
        snapshot.has_ask = get_bool(object, "has_ask");
        snapshot.best_bid_tick = get_i64(object, "best_bid_tick");
        snapshot.best_ask_tick = get_i64(object, "best_ask_tick");
        snapshot.bid_count = get_u32(object, "bid_count");
        snapshot.ask_count = get_u32(object, "ask_count");
        snapshot.state_hash = get_u64(object, "state_hash");
        snapshot.usable_for_depth = get_bool(object, "usable_for_depth");
        snapshot.usable_for_signal = get_bool(object, "usable_for_signal");

        std::uint32_t parsed_bid_count = 0;
        std::uint32_t parsed_ask_count = 0;
        parse_price_levels(object, "bids", &snapshot.bids, &parsed_bid_count);
        parse_price_levels(object, "asks", &snapshot.asks, &parsed_ask_count);
        if (parsed_bid_count > 0) {
            snapshot.bid_count = parsed_bid_count;
        }
        if (parsed_ask_count > 0) {
            snapshot.ask_count = parsed_ask_count;
        }

        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

signal::IntentLeg parse_intent_leg(const json::object& object) {
    signal::IntentLeg leg;
    leg.market_id = get_string(object, "market_id");
    leg.asset_id = get_string(object, "asset_id");
    leg.side = parse_side(get_string(object, "side", "Buy"));
    leg.quantity_lots = get_i64(object, "quantity_lots");
    leg.estimated_vwap_tick = get_i64(object, "estimated_vwap_tick");
    leg.worst_price_tick = get_i64(object, "worst_price_tick");
    leg.estimated_cost_tick = get_i64(object, "estimated_cost_tick");
    leg.enough_depth = get_bool(object, "enough_depth");
    return leg;
}

signal::OpportunityIntent parse_intent(const json::object& object) {
    signal::OpportunityIntent intent;
    intent.intent_id = get_u64(object, "intent_id");
    intent.bundle_id = get_u64(object, "bundle_id");
    intent.status = parse_intent_status(get_string(object, "status"));
    intent.valid_under_settlement =
        get_bool(object, "valid_under_settlement");
    intent.passed_quality_gate = get_bool(object, "passed_quality_gate");
    intent.enough_depth = get_bool(object, "enough_depth");
    intent.guaranteed_payout_tick =
        get_i64(object, "guaranteed_payout_tick");
    intent.estimated_cost_tick = get_i64(object, "estimated_cost_tick");
    intent.estimated_fee_tick = get_i64(object, "estimated_fee_tick");
    intent.latency_buffer_tick = get_i64(object, "latency_buffer_tick");
    intent.estimated_edge_tick = get_i64(object, "estimated_edge_tick");
    intent.min_edge_tick = get_i64(object, "min_edge_tick");
    intent.oracle_artifact_hash = get_u64(object, "oracle_artifact_hash");
    intent.constraint_hash = get_u64(object, "constraint_hash");
    intent.bundle_hash = get_u64(object, "bundle_hash");
    intent.snapshot_version = get_u64(object, "snapshot_version");
    intent.snapshot_version_hash = get_u64(object, "snapshot_version_hash");
    intent.bundle_qty = get_i64(object, "bundle_qty");
    intent.unit_edge_tick = get_i64(object, "unit_edge_tick");
    intent.total_edge_tick = get_i64(object, "total_edge_tick");
    intent.edge_bps = get_i64(object, "edge_bps");
    intent.slippage_buffer_tick = get_i64(object, "slippage_buffer_tick");
    intent.created_ts_ns = get_u64(object, "created_ts_ns");
    intent.expires_at_ns = get_u64(object, "expires_at_ns");
    intent.oracle_artifact_version =
        get_u64(object, "oracle_artifact_version");
    intent.idempotency_key = get_string(object, "idempotency_key");
    intent.proof_ref = get_string(object, "proof_ref");
    intent.reject_reason = get_string(object, "reject_reason");

    const auto* legs_value = object.if_contains("legs");
    if (legs_value != nullptr && legs_value->is_array()) {
        std::uint16_t index = 0;
        for (const auto& value : legs_value->as_array()) {
            if (index >= signal::kMaxIntentLegs) {
                fail("too many intent legs");
            }
            intent.legs[index++] =
                parse_intent_leg(as_object(value, "intent leg"));
        }
        intent.leg_count = index;
    } else {
        intent.leg_count = static_cast<std::uint16_t>(
            get_u64(object, "leg_count")
        );
    }

    return intent;
}

std::unordered_map<std::uint64_t, signal::OpportunityIntent>
load_source_intents(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::unordered_map<std::uint64_t, signal::OpportunityIntent> intents;
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
        auto intent = parse_intent(as_object(parsed, "source intent"));
        intents[intent.intent_id] = std::move(intent);
    }
    return intents;
}

ApprovedIntentRecord parse_approval(const json::object& object) {
    ApprovedIntentRecord record;
    record.source_intent_id = get_u64(object, "source_intent_id");
    record.approval.decision_id = get_u64(object, "decision_id");
    record.approval.reservation_id = get_u64(object, "reservation_id");
    record.approval.bundle_id = get_u64(object, "bundle_id");
    record.approval.approved_bundle_qty =
        get_i64(object, "approved_bundle_qty");
    record.approval.idempotency_key = get_string(object, "idempotency_key");
    return record;
}

std::vector<ApprovedIntentRecord> load_approved_intents(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::vector<ApprovedIntentRecord> approvals;
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
        approvals.push_back(parse_approval(as_object(parsed, "approval")));
    }
    return approvals;
}

execution::ExecutionMode parse_execution_mode(const std::string& value) {
    if (value == "paper" || value == "Paper") {
        return execution::ExecutionMode::Paper;
    }
    if (value == "sandbox" || value == "Sandbox") {
        return execution::ExecutionMode::Sandbox;
    }
    if (value == "live" || value == "Live") {
        return execution::ExecutionMode::Live;
    }
    fail("unsupported execution mode: " + value);
}

execution::PaperExecutionMode parse_paper_mode(const std::string& value) {
    if (value == "PaperAtomic" || value == "atomic") {
        return execution::PaperExecutionMode::PaperAtomic;
    }
    if (value == "PaperSequential" || value == "sequential") {
        return execution::PaperExecutionMode::PaperSequential;
    }
    fail("unsupported paper mode: " + value);
}

ExecutionConfigFixture load_execution_config(
    const std::filesystem::path& path
) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "execution config");
    const json::object* config_object = &root;
    if (const auto* value = root.if_contains("execution");
        value != nullptr && value->is_object()) {
        config_object = &value->as_object();
    }

    ExecutionConfigFixture fixture;
    fixture.now_ns = get_u64(root, "now_ns", 1'500);
    fixture.cancel_after_hedge_required =
        get_bool(root, "cancel_after_hedge_required", false);

    auto& config = fixture.config;
    config.mode =
        parse_execution_mode(get_string(*config_object, "mode", "paper"));
    config.execution_enabled =
        get_bool(*config_object, "execution_enabled", true);
    config.live_enabled = get_bool(*config_object, "live_enabled", false);
    config.paper_mode = parse_paper_mode(
        get_string(*config_object, "paper_mode", "PaperAtomic")
    );
    config.allow_partial_fill_paper =
        get_bool(*config_object, "allow_partial_fill_paper", false);
    config.max_child_orders_per_plan =
        get_u32(*config_object, "max_child_orders_per_plan", 16);
    config.max_plans_per_second =
        get_u32(*config_object, "max_plans_per_second", 10);
    config.max_order_age_ns =
        get_i64(*config_object, "max_order_age_ns", 1'000'000'000);
    config.default_time_in_force_ns =
        get_i64(*config_object, "default_time_in_force_ns", 1'000'000'000);
    return fixture;
}

std::unique_ptr<execution::IExecutionAdapter> make_adapter(
    execution::ExecutionMode mode
) {
    if (mode == execution::ExecutionMode::Live) {
        return std::make_unique<execution::LiveExecutionAdapter>();
    }
    return std::make_unique<execution::PaperExecutionAdapter>();
}

void record_report(
    WorkflowSummary* summary,
    const execution::ExecutionReport& report
) {
    ++summary->metrics.report_published;
    if (report.status == execution::ChildOrderStatus::Filled) {
        ++summary->child_orders_filled;
        ++summary->metrics.child_filled;
    } else if (report.status == execution::ChildOrderStatus::PartiallyFilled) {
        ++summary->child_orders_partially_filled;
        ++summary->metrics.child_partial;
    } else if (report.status == execution::ChildOrderStatus::Cancelled) {
        ++summary->metrics.child_cancelled;
    } else if (
        report.status == execution::ChildOrderStatus::Failed ||
        report.status == execution::ChildOrderStatus::Expired
    ) {
        ++summary->metrics.child_failed;
    }

    if (report.filled_lots > 0) {
        summary->total_filled_qty_lots += report.filled_lots;
        summary->total_filled_cost_tick +=
            report.filled_lots * report.avg_fill_price_tick;
    }

    mix_u64(&summary->execution_output_hash, report.plan_id);
    mix_u64(&summary->execution_output_hash, report.child_order_id);
    mix_u64(
        &summary->execution_output_hash,
        static_cast<std::uint64_t>(report.status)
    );
    mix_i64(&summary->execution_output_hash, report.filled_lots);
    mix_i64(&summary->execution_output_hash, report.remaining_lots);
    mix_i64(&summary->execution_output_hash, report.avg_fill_price_tick);
    mix_string(&summary->execution_output_hash, report.reject_reason);
}

void record_disposition(
    WorkflowSummary* summary,
    const execution::ReservationDisposition& disposition
) {
    switch (disposition.type) {
        case execution::ReservationDispositionType::Consume:
            ++summary->reservation_consumed;
            ++summary->metrics.reservation_consume;
            break;
        case execution::ReservationDispositionType::Release:
            ++summary->reservation_released;
            ++summary->metrics.reservation_release;
            break;
        case execution::ReservationDispositionType::Expire:
            ++summary->reservation_expired;
            ++summary->metrics.reservation_expire;
            break;
        case execution::ReservationDispositionType::None:
            break;
    }

    mix_string(&summary->execution_output_hash, disposition.reservation_id);
    mix_u64(&summary->execution_output_hash, disposition.plan_id);
    mix_u64(
        &summary->execution_output_hash,
        static_cast<std::uint64_t>(disposition.type)
    );
    mix_string(&summary->execution_output_hash, disposition.reason);
}

void record_result(
    WorkflowSummary* summary,
    const execution::ExecutionResult& result,
    const signal::OpportunityIntent& source
) {
    if (result.plan_id != 0) {
        ++summary->plans_created;
        ++summary->plans_submitted;
        summary->child_orders_created += source.leg_count;
        ++summary->metrics.plan_created;
        ++summary->metrics.plan_submitted;
        summary->metrics.child_created += source.leg_count;
    }

    switch (result.status) {
        case execution::PlanStatus::Filled:
            ++summary->plans_filled;
            ++summary->metrics.plan_filled;
            break;
        case execution::PlanStatus::Expired:
            ++summary->plans_expired;
            ++summary->metrics.plan_expired;
            break;
        case execution::PlanStatus::HedgeRequired:
            ++summary->plans_hedge_required;
            ++summary->reservation_hedge_required;
            ++summary->metrics.plan_hedge_required;
            ++summary->metrics.reservation_hedge_required;
            break;
        case execution::PlanStatus::Failed:
        case execution::PlanStatus::Cancelled:
            ++summary->plans_failed;
            ++summary->metrics.plan_failed;
            break;
        default:
            break;
    }

    mix_u64(&summary->execution_output_hash, result.plan_id);
    mix_u64(
        &summary->execution_output_hash,
        static_cast<std::uint64_t>(result.status)
    );
    mix_u64(&summary->execution_output_hash, result.child_orders_submitted);
    mix_u64(&summary->execution_output_hash, result.child_orders_rejected);
    mix_string(&summary->execution_output_hash, result.error);
}

WorkflowSummary run_workflow_once(
    const std::vector<ApprovedIntentRecord>& approvals,
    const std::unordered_map<std::uint64_t, signal::OpportunityIntent>& sources,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const ExecutionConfigFixture& fixture
) {
    auto adapter = make_adapter(fixture.config.mode);
    execution::CapturingExecutionPublisher report_publisher;
    CapturingReservationDispositionPublisher reservation_publisher;
    execution::ExecutionGateway gateway(
        adapter.get(),
        &report_publisher,
        &reservation_publisher
    );

    WorkflowSummary summary;
    summary.approved_intents_loaded = approvals.size();
    summary.adapter_mode = execution::to_string(fixture.config.mode);

    std::unordered_set<std::string> seen_idempotency_keys;
    for (const auto& approved : approvals) {
        const auto source_it = sources.find(approved.source_intent_id);
        if (source_it == sources.end()) {
            ++summary.plans_failed;
            ++summary.metrics.plan_failed;
            mix_string(&summary.execution_output_hash, "missing_source_intent");
            mix_u64(&summary.execution_output_hash, approved.source_intent_id);
            continue;
        }

        const auto& source = source_it->second;
        if (!seen_idempotency_keys.insert(
                approved.approval.idempotency_key
            ).second) {
            ++summary.plans_failed;
            ++summary.metrics.plan_failed;
            mix_string(&summary.execution_output_hash, "duplicate_idempotency_key");
            mix_string(
                &summary.execution_output_hash,
                approved.approval.idempotency_key
            );
            continue;
        }

        execution::ApprovedIntentEnvelope envelope;
        envelope.source_intent = source;
        envelope.approval = approved.approval;

        execution::ExecutionContext context;
        context.now_ns = fixture.now_ns;
        context.snapshots = snapshots;
        context.config = fixture.config;

        const auto submit_start = std::chrono::steady_clock::now();
        const auto result = gateway.submit_approved_intent(envelope, context);
        const auto submit_end = std::chrono::steady_clock::now();
        const auto submit_latency_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    submit_end - submit_start
                ).count()
            );
        summary.metrics.observe_submit_latency(submit_latency_ns);
        summary.metrics.observe_fill_simulation_latency(submit_latency_ns);

        if (result.plan_id == 0 && result.error.find("expired") != std::string::npos) {
            ++summary.plans_expired;
            ++summary.reservation_expired;
            ++summary.metrics.plan_expired;
            ++summary.metrics.reservation_expire;
            mix_string(&summary.execution_output_hash, result.error);
            mix_u64(&summary.execution_output_hash, approved.approval.reservation_id);
        } else {
            record_result(&summary, result, source);
        }

        const auto reports = gateway.poll();
        for (const auto& report : reports) {
            record_report(&summary, report);
        }

        if (fixture.cancel_after_hedge_required &&
            result.status == execution::PlanStatus::HedgeRequired &&
            result.plan_id != 0) {
            const auto cancel = gateway.cancel_plan(result.plan_id);
            mix_u64(&summary.execution_output_hash, cancel.plan_id);
            mix_u64(
                &summary.execution_output_hash,
                static_cast<std::uint64_t>(cancel.code)
            );
            mix_string(&summary.execution_output_hash, cancel.error);
            const auto cancel_reports = gateway.poll();
            for (const auto& report : cancel_reports) {
                record_report(&summary, report);
            }
        }
    }

    summary.execution_reports_published = report_publisher.reports().size();
    for (const auto& disposition : reservation_publisher.dispositions()) {
        record_disposition(&summary, disposition);
    }

    return summary;
}

bool deterministic_equal(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.approved_intents_loaded == rhs.approved_intents_loaded &&
           lhs.plans_created == rhs.plans_created &&
           lhs.child_orders_created == rhs.child_orders_created &&
           lhs.adapter_mode == rhs.adapter_mode &&
           lhs.plans_submitted == rhs.plans_submitted &&
           lhs.plans_filled == rhs.plans_filled &&
           lhs.plans_failed == rhs.plans_failed &&
           lhs.plans_expired == rhs.plans_expired &&
           lhs.plans_hedge_required == rhs.plans_hedge_required &&
           lhs.child_orders_filled == rhs.child_orders_filled &&
           lhs.child_orders_partially_filled ==
               rhs.child_orders_partially_filled &&
           lhs.total_filled_qty_lots == rhs.total_filled_qty_lots &&
           lhs.total_filled_cost_tick == rhs.total_filled_cost_tick &&
           lhs.execution_reports_published ==
               rhs.execution_reports_published &&
           lhs.reservation_consumed == rhs.reservation_consumed &&
           lhs.reservation_released == rhs.reservation_released &&
           lhs.reservation_expired == rhs.reservation_expired &&
           lhs.reservation_hedge_required == rhs.reservation_hedge_required &&
           lhs.metrics.plan_created == rhs.metrics.plan_created &&
           lhs.metrics.plan_submitted == rhs.metrics.plan_submitted &&
           lhs.metrics.plan_filled == rhs.metrics.plan_filled &&
           lhs.metrics.plan_failed == rhs.metrics.plan_failed &&
           lhs.metrics.plan_expired == rhs.metrics.plan_expired &&
           lhs.metrics.plan_hedge_required ==
               rhs.metrics.plan_hedge_required &&
           lhs.metrics.child_created == rhs.metrics.child_created &&
           lhs.metrics.child_filled == rhs.metrics.child_filled &&
           lhs.metrics.child_partial == rhs.metrics.child_partial &&
           lhs.metrics.child_cancelled == rhs.metrics.child_cancelled &&
           lhs.metrics.child_failed == rhs.metrics.child_failed &&
           lhs.metrics.report_published == rhs.metrics.report_published &&
           lhs.metrics.reservation_consume ==
               rhs.metrics.reservation_consume &&
           lhs.metrics.reservation_release ==
               rhs.metrics.reservation_release &&
           lhs.metrics.reservation_expire ==
               rhs.metrics.reservation_expire &&
           lhs.metrics.reservation_hedge_required ==
               rhs.metrics.reservation_hedge_required &&
           lhs.metrics.submit_latency_ns.count ==
               rhs.metrics.submit_latency_ns.count &&
           lhs.metrics.fill_simulation_latency_ns.count ==
               rhs.metrics.fill_simulation_latency_ns.count &&
           lhs.execution_output_hash == rhs.execution_output_hash;
}

Args default_args() {
    return {
        .approved_intents =
            "tests/fixtures/execution/approved_intents_positive.jsonl",
        .source_intents =
            "tests/fixtures/execution/opportunity_intents_positive.jsonl",
        .snapshots =
            "tests/fixtures/execution/market_state_snapshots_execution.json",
        .config = "tests/fixtures/execution/execution_config_paper.json",
        .check_determinism = true
    };
}

Args parse_args(int argc, char** argv) {
    if (argc == 1) {
        return default_args();
    }

    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string{"missing value for "} + name);
            }
            return argv[++i];
        };

        if (arg == "--approved-intents") {
            args.approved_intents = require_value("--approved-intents");
        } else if (arg == "--source-intents") {
            args.source_intents = require_value("--source-intents");
        } else if (arg == "--snapshots") {
            args.snapshots = require_value("--snapshots");
        } else if (arg == "--config") {
            args.config = require_value("--config");
        } else if (arg == "--check-determinism") {
            args.check_determinism = true;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.approved_intents.empty()) {
        fail("--approved-intents is required");
    }
    if (args.source_intents.empty()) {
        fail("--source-intents is required");
    }
    if (args.snapshots.empty()) {
        fail("--snapshots is required");
    }
    if (args.config.empty()) {
        fail("--config is required");
    }
    return args;
}

void print_latency_metric(
    const char* name,
    const execution::ExecutionLatencyMetric& metric
) {
    std::cout
        << "  " << name << ":\n"
        << "    count: " << metric.count << "\n"
        << "    last_ns: " << metric.last_ns << "\n"
        << "    min_ns: " << metric.min_ns << "\n"
        << "    max_ns: " << metric.max_ns << "\n"
        << "    total_ns: " << metric.total_ns << "\n";
}

void print_summary(const WorkflowSummary& summary) {
    std::cout
        << "execution_workflow:\n"
        << "  approved_intents_loaded: "
        << summary.approved_intents_loaded << "\n"
        << "  plans_created: " << summary.plans_created << "\n"
        << "  child_orders_created: "
        << summary.child_orders_created << "\n\n"
        << "adapter:\n"
        << "  mode: " << summary.adapter_mode << "\n"
        << "  plans_submitted: " << summary.plans_submitted << "\n"
        << "  plans_filled: " << summary.plans_filled << "\n"
        << "  plans_failed: " << summary.plans_failed << "\n"
        << "  plans_expired: " << summary.plans_expired << "\n"
        << "  plans_hedge_required: "
        << summary.plans_hedge_required << "\n\n"
        << "fills:\n"
        << "  child_orders_filled: "
        << summary.child_orders_filled << "\n"
        << "  child_orders_partially_filled: "
        << summary.child_orders_partially_filled << "\n"
        << "  total_filled_qty_lots: "
        << summary.total_filled_qty_lots << "\n"
        << "  total_filled_cost_tick: "
        << summary.total_filled_cost_tick << "\n\n"
        << "reports:\n"
        << "  execution_reports_published: "
        << summary.execution_reports_published << "\n"
        << "  reservation_consumed: "
        << summary.reservation_consumed << "\n"
        << "  reservation_released: "
        << summary.reservation_released << "\n"
        << "  reservation_expired: "
        << summary.reservation_expired << "\n"
        << "  reservation_hedge_required: "
        << summary.reservation_hedge_required << "\n\n"
        << "metrics:\n"
        << "  execution.plan.created: "
        << summary.metrics.plan_created << "\n"
        << "  execution.plan.submitted: "
        << summary.metrics.plan_submitted << "\n"
        << "  execution.plan.filled: "
        << summary.metrics.plan_filled << "\n"
        << "  execution.plan.failed: "
        << summary.metrics.plan_failed << "\n"
        << "  execution.plan.expired: "
        << summary.metrics.plan_expired << "\n"
        << "  execution.plan.hedge_required: "
        << summary.metrics.plan_hedge_required << "\n"
        << "  execution.child.created: "
        << summary.metrics.child_created << "\n"
        << "  execution.child.filled: "
        << summary.metrics.child_filled << "\n"
        << "  execution.child.partial: "
        << summary.metrics.child_partial << "\n"
        << "  execution.child.cancelled: "
        << summary.metrics.child_cancelled << "\n"
        << "  execution.child.failed: "
        << summary.metrics.child_failed << "\n"
        << "  execution.report.published: "
        << summary.metrics.report_published << "\n"
        << "  execution.reservation.consume: "
        << summary.metrics.reservation_consume << "\n"
        << "  execution.reservation.release: "
        << summary.metrics.reservation_release << "\n"
        << "  execution.reservation.expire: "
        << summary.metrics.reservation_expire << "\n"
        << "  execution.reservation.hedge_required: "
        << summary.metrics.reservation_hedge_required << "\n";
    print_latency_metric(
        "execution.latency.submit_ns",
        summary.metrics.submit_latency_ns
    );
    print_latency_metric(
        "execution.latency.fill_simulation_ns",
        summary.metrics.fill_simulation_latency_ns
    );
    std::cout
        << "\n"
        << "hashes:\n"
        << "  execution_output_hash: "
        << summary.execution_output_hash << "\n"
        << "  determinism_passed: "
        << (summary.determinism_passed ? "true" : "false") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto approvals = load_approved_intents(args.approved_intents);
        const auto sources = load_source_intents(args.source_intents);
        const auto snapshots = load_snapshots(args.snapshots);
        const auto config = load_execution_config(args.config);

        auto summary = run_workflow_once(
            approvals,
            sources,
            snapshots,
            config
        );
        if (args.check_determinism) {
            const auto second = run_workflow_once(
                approvals,
                sources,
                snapshots,
                config
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

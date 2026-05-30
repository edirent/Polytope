#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/metrics/RiskMetrics.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/state/MarketStateSnapshot.h"
#include "oracle/public/CandidateBundle.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace json = boost::json;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;
namespace oracle = trading_engine::oracle;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Args {
    std::filesystem::path intent_fixture;
    std::filesystem::path snapshot_fixture;
    std::filesystem::path risk_config;
    bool check_determinism = false;
};

struct RiskConfigFixture {
    std::uint64_t now_ns = 0;
    risk::RiskPolicySnapshot policy;
};

struct WorkflowSummary {
    std::uint64_t intents_loaded = 0;
    std::uint64_t intents_evaluated = 0;

    std::uint64_t approved = 0;
    std::uint64_t rejected_invalid_intent = 0;
    std::uint64_t rejected_expired = 0;
    std::uint64_t rejected_duplicate = 0;
    std::uint64_t rejected_kill_switch = 0;
    std::uint64_t rejected_stale_book = 0;
    std::uint64_t rejected_insufficient_depth = 0;
    std::uint64_t rejected_cost_drift = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t rejected_exposure_limit = 0;
    std::uint64_t rejected_inventory_limit = 0;
    std::uint64_t rejected_partial_fill_risk = 0;
    std::uint64_t rejected_max_loss = 0;
    std::uint64_t rejected_rate_limited = 0;

    std::uint64_t vwap_recomputed = 0;
    std::int64_t cost_drift_min = 0;
    std::int64_t cost_drift_max = 0;
    bool has_cost_drift = false;

    std::uint64_t reservations_created = 0;
    std::uint64_t reservations_released = 0;
    std::uint64_t reservations_expired = 0;

    risk::RiskMetrics metrics;

    std::uint64_t risk_output_hash = kFnvOffset;
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

std::vector<signal::OpportunityIntent> load_intents_jsonl(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }

    std::vector<signal::OpportunityIntent> intents;
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
        intents.push_back(parse_intent(as_object(parsed, "intent")));
    }
    return intents;
}

RiskConfigFixture load_risk_config(const std::filesystem::path& path) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "risk config");

    const json::object* policy_object = &root;
    if (const auto* value = root.if_contains("policy");
        value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    } else if (const auto* value = root.if_contains("risk");
               value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    }

    RiskConfigFixture fixture;
    fixture.now_ns = get_u64(root, "now_ns", 1'500);

    auto& policy = fixture.policy;
    policy.policy_version = get_u64(*policy_object, "policy_version", 1);
    policy.policy_hash = get_u64(*policy_object, "policy_hash", 0);
    policy.risk_enabled = get_bool(*policy_object, "risk_enabled", true);
    policy.kill_switch_enabled =
        get_bool(*policy_object, "kill_switch_enabled", false);
    policy.min_post_risk_total_edge_tick =
        get_i64(*policy_object, "min_post_risk_total_edge_tick", 0);
    policy.min_post_risk_unit_edge_tick =
        get_i64(*policy_object, "min_post_risk_unit_edge_tick", 0);
    policy.min_edge_bps = get_i64(*policy_object, "min_edge_bps", 0);
    policy.max_total_cost_tick =
        get_i64(*policy_object, "max_total_cost_tick", 0);
    policy.max_single_market_exposure_tick =
        get_i64(*policy_object, "max_single_market_exposure_tick", 0);
    policy.max_total_exposure_tick =
        get_i64(*policy_object, "max_total_exposure_tick", 0);
    policy.max_inventory_lots_per_asset =
        get_i64(*policy_object, "max_inventory_lots_per_asset", 0);
    policy.max_book_age_ns =
        get_i64(*policy_object, "max_book_age_ns", 1'000'000'000);
    policy.max_intent_age_ns =
        get_i64(*policy_object, "max_intent_age_ns", 1'000'000'000);
    policy.max_snapshot_skew_ns =
        get_i64(*policy_object, "max_snapshot_skew_ns", 0);
    policy.max_allowed_cost_drift_tick =
        get_i64(*policy_object, "max_allowed_cost_drift_tick", 0);
    policy.max_slippage_tick =
        get_i64(*policy_object, "max_slippage_tick", 0);
    policy.max_unhedged_loss_tick =
        get_i64(*policy_object, "max_unhedged_loss_tick", 0);
    policy.min_depth_margin_ratio =
        get_double(*policy_object, "min_depth_margin_ratio", 1.20);
    policy.max_pending_intents_per_bundle =
        get_u32(*policy_object, "max_pending_intents_per_bundle", 1);
    policy.max_pending_intents_total =
        get_u32(*policy_object, "max_pending_intents_total", 1024);
    policy.max_approvals_per_second =
        get_u32(*policy_object, "max_approvals_per_second", 100);

    if (policy.policy_hash == 0) {
        policy.policy_hash = risk::compute_policy_hash(policy);
    }
    return fixture;
}

const risk::RiskAuditStep* failing_step(
    const risk::RiskPipelineResult& result
) {
    for (auto it = result.audit_trace.steps.rbegin();
         it != result.audit_trace.steps.rend();
         ++it) {
        if (!it->pass) {
            return &*it;
        }
    }
    return nullptr;
}

void update_cost_drift(
    WorkflowSummary* summary,
    std::int64_t drift
) noexcept {
    if (!summary->has_cost_drift) {
        summary->cost_drift_min = drift;
        summary->cost_drift_max = drift;
        summary->has_cost_drift = true;
        return;
    }
    summary->cost_drift_min = std::min(summary->cost_drift_min, drift);
    summary->cost_drift_max = std::max(summary->cost_drift_max, drift);
}

void record_decision(
    WorkflowSummary* summary,
    const risk::RiskPipelineResult& result,
    std::uint64_t evaluate_latency_ns
) {
    ++summary->intents_evaluated;
    ++summary->metrics.evaluate_count;
    summary->metrics.observe_evaluate_latency(evaluate_latency_ns);
    if (result.cost_revalidated) {
        ++summary->vwap_recomputed;
        ++summary->metrics.vwap_recomputed;
        update_cost_drift(summary, result.cost.cost_drift_tick);
    }
    if (result.reservation.ok) {
        ++summary->reservations_created;
        ++summary->metrics.reservation_created;
    }

    mix_u64(&summary->risk_output_hash, result.output_hash);
    mix_u64(
        &summary->risk_output_hash,
        static_cast<std::uint64_t>(result.decision.status)
    );
    mix_u64(
        &summary->risk_output_hash,
        static_cast<std::uint64_t>(result.decision.reject_reason)
    );
    mix_i64(&summary->risk_output_hash, result.cost.risk_total_cost_tick);
    mix_i64(&summary->risk_output_hash, result.cost.cost_drift_tick);
    mix_string(&summary->risk_output_hash, result.decision.reject_detail);

    if (result.decision.approved()) {
        ++summary->approved;
        ++summary->metrics.approve_count;
        return;
    }
    ++summary->metrics.reject_count;

    const auto* step = failing_step(result);
    const auto step_name = step == nullptr ? std::string{} : step->guard_name;

    switch (result.decision.reject_reason) {
        case risk::RiskRejectReason::InvalidIntent:
        case risk::RiskRejectReason::MissingEvidence:
        case risk::RiskRejectReason::InternalError:
        case risk::RiskRejectReason::MissingReservation:
            ++summary->rejected_invalid_intent;
            ++summary->metrics.reject_invalid_intent;
            break;
        case risk::RiskRejectReason::ExpiredIntent:
            ++summary->rejected_expired;
            ++summary->metrics.reject_expired;
            break;
        case risk::RiskRejectReason::DuplicateIntent:
        case risk::RiskRejectReason::DuplicateReservation:
            ++summary->rejected_duplicate;
            ++summary->metrics.reject_duplicate;
            break;
        case risk::RiskRejectReason::KillSwitch:
            ++summary->rejected_kill_switch;
            ++summary->metrics.reject_kill_switch;
            break;
        case risk::RiskRejectReason::StaleBook:
        case risk::RiskRejectReason::SnapshotSkew:
        case risk::RiskRejectReason::BadMarketState:
            ++summary->rejected_stale_book;
            ++summary->metrics.reject_stale_book;
            break;
        case risk::RiskRejectReason::CostLimit:
            if (step_name == "MaxLossGuard") {
                ++summary->rejected_max_loss;
                ++summary->metrics.reject_max_loss;
            } else {
                ++summary->rejected_insufficient_depth;
                ++summary->metrics.reject_insufficient_depth;
            }
            break;
        case risk::RiskRejectReason::CostDrift:
        case risk::RiskRejectReason::SlippageLimit:
            ++summary->rejected_cost_drift;
            ++summary->metrics.reject_cost_drift;
            break;
        case risk::RiskRejectReason::LowTotalEdge:
        case risk::RiskRejectReason::LowUnitEdge:
        case risk::RiskRejectReason::LowEdgeBps:
            ++summary->rejected_low_edge;
            ++summary->metrics.reject_low_edge;
            break;
        case risk::RiskRejectReason::TotalExposureLimit:
        case risk::RiskRejectReason::SingleMarketExposureLimit:
            ++summary->rejected_exposure_limit;
            ++summary->metrics.reject_exposure;
            break;
        case risk::RiskRejectReason::InventoryLimit:
            ++summary->rejected_inventory_limit;
            ++summary->metrics.reject_inventory;
            break;
        case risk::RiskRejectReason::PartialFillRisk:
            ++summary->rejected_partial_fill_risk;
            ++summary->metrics.reject_partial_fill;
            break;
        case risk::RiskRejectReason::PendingIntentLimit:
        case risk::RiskRejectReason::ApprovalRateLimit:
            ++summary->rejected_rate_limited;
            ++summary->metrics.reject_rate_limited;
            break;
        case risk::RiskRejectReason::None:
        case risk::RiskRejectReason::NotEvaluated:
        case risk::RiskRejectReason::RiskDisabled:
            ++summary->rejected_invalid_intent;
            ++summary->metrics.reject_invalid_intent;
            break;
    }
}

WorkflowSummary run_workflow_once(
    const std::vector<signal::OpportunityIntent>& intents,
    const std::vector<state::MarketStateSnapshot>& snapshots,
    const RiskConfigFixture& config
) {
    risk::RiskEngine engine;
    WorkflowSummary summary;
    summary.intents_loaded = intents.size();

    for (const auto& intent : intents) {
        using Clock = std::chrono::steady_clock;
        risk::RiskEvaluationContext context;
        context.now_ns = config.now_ns;
        context.latest_snapshots = snapshots;
        context.policy = config.policy;
        context.ledger_snapshot = engine.ledger_snapshot();
        const auto started = Clock::now();
        const auto result = engine.evaluate(intent, std::move(context));
        const auto elapsed = Clock::now() - started;
        record_decision(
            &summary,
            result,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    elapsed
                ).count()
            )
        );
    }

    const auto ledger = engine.ledger_snapshot();
    summary.reservations_released = ledger.released_reservations;
    summary.reservations_expired = ledger.expired_reservations;
    summary.metrics.reservation_released = ledger.released_reservations;
    summary.metrics.reservation_expired = ledger.expired_reservations;
    if (!summary.has_cost_drift) {
        summary.cost_drift_min = 0;
        summary.cost_drift_max = 0;
    }
    return summary;
}

bool deterministic_equal(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.intents_loaded == rhs.intents_loaded &&
           lhs.intents_evaluated == rhs.intents_evaluated &&
           lhs.approved == rhs.approved &&
           lhs.rejected_invalid_intent == rhs.rejected_invalid_intent &&
           lhs.rejected_expired == rhs.rejected_expired &&
           lhs.rejected_duplicate == rhs.rejected_duplicate &&
           lhs.rejected_kill_switch == rhs.rejected_kill_switch &&
           lhs.rejected_stale_book == rhs.rejected_stale_book &&
           lhs.rejected_insufficient_depth ==
               rhs.rejected_insufficient_depth &&
           lhs.rejected_cost_drift == rhs.rejected_cost_drift &&
           lhs.rejected_low_edge == rhs.rejected_low_edge &&
           lhs.rejected_exposure_limit == rhs.rejected_exposure_limit &&
           lhs.rejected_inventory_limit == rhs.rejected_inventory_limit &&
           lhs.rejected_partial_fill_risk ==
               rhs.rejected_partial_fill_risk &&
           lhs.rejected_max_loss == rhs.rejected_max_loss &&
           lhs.rejected_rate_limited == rhs.rejected_rate_limited &&
           lhs.vwap_recomputed == rhs.vwap_recomputed &&
           lhs.cost_drift_min == rhs.cost_drift_min &&
           lhs.cost_drift_max == rhs.cost_drift_max &&
           lhs.reservations_created == rhs.reservations_created &&
           lhs.reservations_released == rhs.reservations_released &&
           lhs.reservations_expired == rhs.reservations_expired &&
           lhs.metrics.evaluate_count == rhs.metrics.evaluate_count &&
           lhs.metrics.approve_count == rhs.metrics.approve_count &&
           lhs.metrics.reject_count == rhs.metrics.reject_count &&
           lhs.metrics.reject_invalid_intent ==
               rhs.metrics.reject_invalid_intent &&
           lhs.metrics.reject_expired == rhs.metrics.reject_expired &&
           lhs.metrics.reject_duplicate == rhs.metrics.reject_duplicate &&
           lhs.metrics.reject_kill_switch == rhs.metrics.reject_kill_switch &&
           lhs.metrics.reject_stale_book == rhs.metrics.reject_stale_book &&
           lhs.metrics.reject_insufficient_depth ==
               rhs.metrics.reject_insufficient_depth &&
           lhs.metrics.reject_cost_drift == rhs.metrics.reject_cost_drift &&
           lhs.metrics.reject_low_edge == rhs.metrics.reject_low_edge &&
           lhs.metrics.reject_exposure == rhs.metrics.reject_exposure &&
           lhs.metrics.reject_inventory == rhs.metrics.reject_inventory &&
           lhs.metrics.reject_partial_fill ==
               rhs.metrics.reject_partial_fill &&
           lhs.metrics.reject_max_loss == rhs.metrics.reject_max_loss &&
           lhs.metrics.reject_rate_limited ==
               rhs.metrics.reject_rate_limited &&
           lhs.metrics.reservation_created ==
               rhs.metrics.reservation_created &&
           lhs.metrics.reservation_released ==
               rhs.metrics.reservation_released &&
           lhs.metrics.reservation_expired ==
               rhs.metrics.reservation_expired &&
           lhs.metrics.vwap_recomputed == rhs.metrics.vwap_recomputed &&
           lhs.risk_output_hash == rhs.risk_output_hash;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string{"missing value for "} + name);
            }
            return argv[++i];
        };

        if (arg == "--intent-fixture") {
            args.intent_fixture = require_value("--intent-fixture");
        } else if (arg == "--snapshot-fixture") {
            args.snapshot_fixture = require_value("--snapshot-fixture");
        } else if (arg == "--risk-config") {
            args.risk_config = require_value("--risk-config");
        } else if (arg == "--check-determinism") {
            args.check_determinism = true;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.intent_fixture.empty()) {
        fail("--intent-fixture is required");
    }
    if (args.snapshot_fixture.empty()) {
        fail("--snapshot-fixture is required");
    }
    if (args.risk_config.empty()) {
        fail("--risk-config is required");
    }
    return args;
}

void print_summary(const WorkflowSummary& summary) {
    std::cout
        << "risk_workflow:\n"
        << "  intents_loaded: " << summary.intents_loaded << "\n"
        << "  intents_evaluated: " << summary.intents_evaluated << "\n\n"
        << "decisions:\n"
        << "  approved: " << summary.approved << "\n"
        << "  rejected_invalid_intent: "
        << summary.rejected_invalid_intent << "\n"
        << "  rejected_expired: " << summary.rejected_expired << "\n"
        << "  rejected_duplicate: " << summary.rejected_duplicate << "\n"
        << "  rejected_kill_switch: " << summary.rejected_kill_switch << "\n"
        << "  rejected_stale_book: " << summary.rejected_stale_book << "\n"
        << "  rejected_insufficient_depth: "
        << summary.rejected_insufficient_depth << "\n"
        << "  rejected_cost_drift: " << summary.rejected_cost_drift << "\n"
        << "  rejected_low_edge: " << summary.rejected_low_edge << "\n"
        << "  rejected_exposure_limit: "
        << summary.rejected_exposure_limit << "\n"
        << "  rejected_inventory_limit: "
        << summary.rejected_inventory_limit << "\n"
        << "  rejected_partial_fill_risk: "
        << summary.rejected_partial_fill_risk << "\n"
        << "  rejected_max_loss: " << summary.rejected_max_loss << "\n"
        << "  rejected_rate_limited: "
        << summary.rejected_rate_limited << "\n\n"
        << "repricing:\n"
        << "  vwap_recomputed: " << summary.vwap_recomputed << "\n"
        << "  cost_drift_min: " << summary.cost_drift_min << "\n"
        << "  cost_drift_max: " << summary.cost_drift_max << "\n\n"
        << "reservation:\n"
        << "  reservations_created: "
        << summary.reservations_created << "\n"
        << "  reservations_released: "
        << summary.reservations_released << "\n"
        << "  reservations_expired: "
        << summary.reservations_expired << "\n\n";

    const auto& metrics = summary.metrics;
    std::cout
        << "metrics:\n"
        << "  risk.evaluate.count: " << metrics.evaluate_count << "\n"
        << "  risk.approve.count: " << metrics.approve_count << "\n"
        << "  risk.reject.count: " << metrics.reject_count << "\n"
        << "  risk.reject.invalid_intent: "
        << metrics.reject_invalid_intent << "\n"
        << "  risk.reject.expired: " << metrics.reject_expired << "\n"
        << "  risk.reject.duplicate: " << metrics.reject_duplicate << "\n"
        << "  risk.reject.kill_switch: "
        << metrics.reject_kill_switch << "\n"
        << "  risk.reject.stale_book: "
        << metrics.reject_stale_book << "\n"
        << "  risk.reject.insufficient_depth: "
        << metrics.reject_insufficient_depth << "\n"
        << "  risk.reject.cost_drift: "
        << metrics.reject_cost_drift << "\n"
        << "  risk.reject.low_edge: "
        << metrics.reject_low_edge << "\n"
        << "  risk.reject.exposure: "
        << metrics.reject_exposure << "\n"
        << "  risk.reject.inventory: "
        << metrics.reject_inventory << "\n"
        << "  risk.reject.partial_fill: "
        << metrics.reject_partial_fill << "\n"
        << "  risk.reject.max_loss: "
        << metrics.reject_max_loss << "\n"
        << "  risk.reject.rate_limited: "
        << metrics.reject_rate_limited << "\n"
        << "  risk.reservation.created: "
        << metrics.reservation_created << "\n"
        << "  risk.reservation.expired: "
        << metrics.reservation_expired << "\n"
        << "  risk.reservation.released: "
        << metrics.reservation_released << "\n"
        << "  risk.vwap.recomputed: "
        << metrics.vwap_recomputed << "\n"
        << "  risk.latency.evaluate_ns:\n"
        << "    count: " << metrics.evaluate_latency_ns.count << "\n"
        << "    last: " << metrics.evaluate_latency_ns.last_ns << "\n"
        << "    min: " << metrics.evaluate_latency_ns.min_ns << "\n"
        << "    max: " << metrics.evaluate_latency_ns.max_ns << "\n\n"
        << "hashes:\n"
        << "  risk_output_hash: " << summary.risk_output_hash << "\n"
        << "  determinism_passed: "
        << (summary.determinism_passed ? "true" : "false") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto intents = load_intents_jsonl(args.intent_fixture);
        const auto snapshots = load_snapshots(args.snapshot_fixture);
        const auto config = load_risk_config(args.risk_config);

        auto summary = run_workflow_once(intents, snapshots, config);
        if (args.check_determinism) {
            const auto second = run_workflow_once(intents, snapshots, config);
            summary.determinism_passed =
                deterministic_equal(summary, second);
        }

        print_summary(summary);
        return summary.determinism_passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}

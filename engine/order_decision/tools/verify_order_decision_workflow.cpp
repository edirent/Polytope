#include "engine/order_decision/core/OrderDecisionEngine.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/state/view/MarketDepthView.h"
#include "oracle/bundles/BundleHash.h"
#include "oracle/public/CandidateBundle.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace json = boost::json;
namespace decision = trading_engine::order_decision;
namespace oracle = trading_engine::oracle;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Args {
    std::filesystem::path intent_fixture;
    std::filesystem::path depth_fixture;
    std::filesystem::path bundle_fixture;
    std::filesystem::path risk_policy;
    bool check_determinism = false;
    bool require_decision = false;
};

struct PolicyFixture {
    std::uint64_t now_ns = 0;
    risk::RiskPolicySnapshot policy;
    decision::OrderDecisionConfig order_config;
};

struct DepthFixture {
    std::vector<state::MarketDepthView> depth_views;
    std::unordered_map<std::string, std::uint32_t> asset_indices;
};

struct WorkflowSummary {
    std::uint64_t intents_loaded = 0;
    std::uint64_t decisions_created = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t rejected_insufficient_depth = 0;
    std::int64_t chosen_bundle_qty_total = 0;
    std::int64_t total_expected_edge = 0;
    std::uint64_t decision_hash = kFnvOffset;
    bool determinism_passed = true;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
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
    *hash ^= 0xffU;
    *hash *= kFnvPrime;
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

const json::array& as_array(
    const json::value& value,
    const std::string& label
) {
    if (!value.is_array()) {
        fail(label + " must be a JSON array");
    }
    return value.as_array();
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
        fail("unknown side: " + value);
    }
    return side;
}

state::BookQuality quality_from_string(const std::string& value) {
    using state::BookQuality;
    if (value == "Good") {
        return BookQuality::Good;
    }
    if (value == "Stale") {
        return BookQuality::Stale;
    }
    if (value == "Recovering") {
        return BookQuality::Recovering;
    }
    if (value == "Crossed") {
        return BookQuality::Crossed;
    }
    if (value == "ChainMismatch") {
        return BookQuality::ChainMismatch;
    }
    if (value == "ChainLagging") {
        return BookQuality::ChainLagging;
    }
    if (value == "Closed") {
        return BookQuality::Closed;
    }
    if (value == "Resolved") {
        return BookQuality::Resolved;
    }
    return BookQuality::Unknown;
}

state::PriceLevel parse_price_level(const json::object& object) {
    state::PriceLevel level;
    level.price_tick = get_i64(object, "price_tick");
    level.price = get_double(object, "price", 0.0);
    level.size = get_double(object, "size", 0.0);
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
    const auto& levels = as_array(*value, key);
    std::uint32_t index = 0;
    for (const auto& entry : levels) {
        if (index >= state::kMaxSnapshotDepth) {
            break;
        }
        (*out)[index++] = parse_price_level(as_object(entry, key));
    }
    if (count != nullptr) {
        *count = index;
    }
}

state::MarketStateSnapshot parse_snapshot(const json::object& object) {
    state::MarketStateSnapshot snapshot;
    snapshot.entity_id = get_string(object, "entity_id");
    snapshot.market_id = get_string(object, "market_id");
    snapshot.version = get_u64(object, "version");
    snapshot.last_book_update_ns = get_u64(
        object,
        "last_book_update_ns",
        get_u64(object, "last_ws_recv_ns", 0)
    );
    snapshot.live = get_bool(object, "live", true);
    snapshot.recovering = get_bool(object, "recovering", false);
    snapshot.closed = get_bool(object, "closed", false);
    snapshot.resolved = get_bool(object, "resolved", false);
    snapshot.crossed = get_bool(object, "crossed", false);
    snapshot.has_bid = get_bool(object, "has_bid", false);
    snapshot.has_ask = get_bool(object, "has_ask", false);
    snapshot.best_bid_tick = get_i64(object, "best_bid_tick", 0);
    snapshot.best_ask_tick = get_i64(object, "best_ask_tick", 0);
    snapshot.bid_count = get_u32(object, "bid_count", 0);
    snapshot.ask_count = get_u32(object, "ask_count", 0);
    parse_price_levels(object, "bids", &snapshot.bids, &snapshot.bid_count);
    parse_price_levels(object, "asks", &snapshot.asks, &snapshot.ask_count);
    snapshot.state_hash = get_u64(object, "state_hash", 0);
    snapshot.snapshot_version_hash =
        get_u64(object, "snapshot_version_hash", snapshot.state_hash);
    snapshot.quality = quality_from_string(get_string(object, "quality"));
    snapshot.usable_for_depth = get_bool(object, "usable_for_depth", false);
    snapshot.usable_for_signal = get_bool(object, "usable_for_signal", false);
    return snapshot;
}

DepthFixture load_depth_fixture(const std::filesystem::path& path) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "depth fixture");

    const json::array* entries = nullptr;
    if (const auto* value = root.if_contains("snapshots");
        value != nullptr && value->is_array()) {
        entries = &value->as_array();
    } else if (const auto* value = root.if_contains("depth_views");
               value != nullptr && value->is_array()) {
        entries = &value->as_array();
    }
    if (entries == nullptr) {
        fail("depth fixture missing snapshots or depth_views");
    }

    DepthFixture fixture;
    std::uint32_t next_asset_index = 1;
    for (const auto& entry : *entries) {
        const auto& object = as_object(entry, "snapshot");
        auto snapshot = parse_snapshot(object);
        if (snapshot.entity_id.empty()) {
            fail("snapshot missing entity_id");
        }
        auto asset_index = get_u32(object, "asset_index", 0);
        if (asset_index == 0) {
            asset_index = next_asset_index;
        }
        next_asset_index = std::max(next_asset_index, asset_index + 1U);
        fixture.asset_indices.emplace(snapshot.entity_id, asset_index);
        fixture.depth_views.push_back(
            state::market_depth_view_from_snapshot(snapshot, asset_index)
        );
    }
    return fixture;
}

signal::IntentLeg parse_intent_leg(
    const json::object& object,
    const std::unordered_map<std::string, std::uint32_t>& asset_indices
) {
    signal::IntentLeg leg;
    leg.market_id = get_string(object, "market_id");
    leg.asset_id = get_string(object, "asset_id");
    leg.asset_index = get_u32(object, "asset_index", 0);
    if (leg.asset_index == 0) {
        if (const auto it = asset_indices.find(leg.asset_id);
            it != asset_indices.end()) {
            leg.asset_index = it->second;
        }
    }
    leg.side = parse_side(get_string(object, "side", "Buy"));
    leg.quantity_lots = get_i64(object, "quantity_lots", 0);
    leg.estimated_vwap_tick = get_i64(object, "estimated_vwap_tick", 0);
    leg.worst_price_tick = get_i64(object, "worst_price_tick", 0);
    leg.estimated_cost_tick = get_i64(object, "estimated_cost_tick", 0);
    leg.requested_qty_lots = get_i64(object, "requested_qty_lots", 0);
    leg.executable_qty_lots = get_i64(object, "executable_qty_lots", 0);
    leg.depth_margin_bps = get_i64(object, "depth_margin_bps", 0);
    leg.enough_depth = get_bool(object, "enough_depth", false);
    return leg;
}

signal::OpportunityIntent parse_intent(
    const json::object& object,
    const std::unordered_map<std::string, std::uint32_t>& asset_indices
) {
    signal::OpportunityIntent intent;
    intent.intent_id = get_u64(object, "intent_id", 0);
    intent.bundle_id = get_u64(object, "bundle_id", 0);
    intent.status = parse_intent_status(
        get_string(object, "status", "CandidateOnly")
    );
    intent.valid_under_settlement =
        get_bool(object, "valid_under_settlement", false);
    intent.passed_quality_gate = get_bool(object, "passed_quality_gate", false);
    intent.enough_depth = get_bool(object, "enough_depth", false);
    intent.guaranteed_payout_tick =
        get_i64(object, "guaranteed_payout_tick", 0);
    intent.estimated_cost_tick = get_i64(object, "estimated_cost_tick", 0);
    intent.estimated_fee_tick = get_i64(object, "estimated_fee_tick", 0);
    intent.latency_buffer_tick = get_i64(object, "latency_buffer_tick", 0);
    intent.estimated_edge_tick = get_i64(object, "estimated_edge_tick", 0);
    intent.min_edge_tick = get_i64(object, "min_edge_tick", 0);
    intent.oracle_artifact_hash = get_u64(object, "oracle_artifact_hash", 0);
    intent.constraint_hash = get_u64(object, "constraint_hash", 0);
    intent.bundle_hash = get_u64(object, "bundle_hash", 0);
    intent.snapshot_version = get_u64(object, "snapshot_version", 0);
    intent.snapshot_version_hash = get_u64(object, "snapshot_version_hash", 0);
    intent.idempotency_hash = get_u64(object, "idempotency_hash", 0);
    intent.proof_hash = get_u64(object, "proof_hash", 0);
    intent.bundle_qty = get_i64(object, "bundle_qty", 0);
    intent.original_bundle_qty = get_i64(
        object,
        "original_bundle_qty",
        intent.bundle_qty
    );
    intent.unit_edge_tick = get_i64(object, "unit_edge_tick", 0);
    intent.total_edge_tick = get_i64(object, "total_edge_tick", 0);
    intent.edge_bps = get_i64(object, "edge_bps", 0);
    intent.slippage_buffer_tick = get_i64(object, "slippage_buffer_tick", 0);
    intent.max_leg_slippage_tick =
        get_i64(object, "max_leg_slippage_tick", 0);
    intent.created_ts_ns = get_u64(object, "created_ts_ns", 0);
    intent.expires_at_ns = get_u64(object, "expires_at_ns", 0);
    intent.oracle_artifact_version =
        get_u64(object, "oracle_artifact_version", 0);
    intent.idempotency_key = get_string(object, "idempotency_key");
    intent.proof_ref = get_string(object, "proof_ref");

    if (const auto* legs_value = object.if_contains("legs");
        legs_value != nullptr && legs_value->is_array()) {
        std::uint16_t index = 0;
        for (const auto& entry : legs_value->as_array()) {
            if (index >= signal::kMaxIntentLegs) {
                fail("intent has too many legs");
            }
            intent.legs[index++] =
                parse_intent_leg(as_object(entry, "intent leg"), asset_indices);
        }
        intent.leg_count = index;
    } else {
        intent.leg_count = static_cast<std::uint16_t>(
            get_u64(object, "leg_count", 0)
        );
    }
    return intent;
}

std::vector<signal::OpportunityIntent> load_intents_jsonl(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, std::uint32_t>& asset_indices
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
        intents.push_back(
            parse_intent(as_object(parsed, "intent"), asset_indices)
        );
    }
    return intents;
}

oracle::BundleLeg parse_bundle_leg(const json::object& object) {
    oracle::BundleLeg leg;
    leg.market_id = get_string(object, "market_id");
    leg.asset_id = get_string(object, "asset_id");
    leg.side = parse_side(get_string(object, "side", "Buy"));
    leg.quantity_lots = get_i64(object, "quantity_lots", 0);
    leg.max_price_tick = get_i64(object, "max_price_tick", 0);
    return leg;
}

std::vector<oracle::CandidateBundle> load_bundles(
    const std::filesystem::path& path
) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "bundle fixture");

    const json::array* entries = nullptr;
    if (const auto* value = root.if_contains("bundles");
        value != nullptr && value->is_array()) {
        entries = &value->as_array();
    }
    if (entries == nullptr) {
        fail("bundle fixture missing bundles");
    }

    std::vector<oracle::CandidateBundle> bundles;
    for (const auto& entry : *entries) {
        const auto& object = as_object(entry, "bundle");
        oracle::CandidateBundle bundle;
        bundle.bundle_id = get_u64(object, "bundle_id", 0);
        bundle.required_true_mask = get_u64(object, "required_true_mask", 0);
        bundle.required_false_mask = get_u64(object, "required_false_mask", 0);
        bundle.invalid_mask = get_u64(object, "invalid_mask", 0);
        bundle.guaranteed_payout_tick =
            get_i64(object, "guaranteed_payout_tick", 0);
        bundle.min_edge_tick = get_i64(object, "min_edge_tick", 0);

        const auto* legs_value = object.if_contains("legs");
        if (legs_value == nullptr || !legs_value->is_array()) {
            fail("bundle missing legs");
        }
        std::uint16_t index = 0;
        for (const auto& leg_value : legs_value->as_array()) {
            if (index >= oracle::kMaxBundleLegs) {
                fail("bundle has too many legs");
            }
            bundle.legs[index++] =
                parse_bundle_leg(as_object(leg_value, "bundle leg"));
        }
        bundle.leg_count = index;
        bundles.push_back(std::move(bundle));
    }
    return bundles;
}

PolicyFixture load_policy_fixture(const std::filesystem::path& path) {
    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "risk policy fixture");

    const json::object* policy_object = &root;
    if (const auto* value = root.if_contains("policy");
        value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    } else if (const auto* value = root.if_contains("risk");
               value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    }

    PolicyFixture fixture;
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
    policy.max_slippage_tick = get_i64(*policy_object, "max_slippage_tick", 0);
    policy.max_unhedged_loss_tick =
        get_i64(*policy_object, "max_unhedged_loss_tick", 0);
    policy.min_depth_margin_ratio =
        get_double(*policy_object, "min_depth_margin_ratio", 1.20);
    policy.min_depth_margin_bps = get_i64(
        *policy_object,
        "min_depth_margin_bps",
        static_cast<std::int64_t>(policy.min_depth_margin_ratio * 10'000.0)
    );
    policy.max_pending_intents_per_bundle =
        get_u32(*policy_object, "max_pending_intents_per_bundle", 1);
    policy.max_pending_intents_total =
        get_u32(*policy_object, "max_pending_intents_total", 1024);
    policy.max_approvals_per_second =
        get_u32(*policy_object, "max_approvals_per_second", 100);
    if (policy.policy_hash == 0) {
        policy.policy_hash = risk::compute_policy_hash(policy);
    }

    if (const auto* value = root.if_contains("order_decision");
        value != nullptr && value->is_object()) {
        const auto& object = value->as_object();
        auto& config = fixture.order_config;
        config.min_bundle_qty = get_i64(object, "min_bundle_qty", 1);
        config.max_bundle_qty = get_i64(object, "max_bundle_qty", 0);
        config.fee_per_bundle_tick =
            get_i64(object, "fee_per_bundle_tick", 0);
        config.latency_buffer_per_bundle_tick =
            get_i64(object, "latency_buffer_per_bundle_tick", 0);
        config.slippage_buffer_per_bundle_tick =
            get_i64(object, "slippage_buffer_per_bundle_tick", 0);
        config.price_protection_buffer_tick =
            get_i64(object, "price_protection_buffer_tick", 0);
        config.max_allowed_price_tick =
            get_i64(object, "max_allowed_price_tick", 0);
        config.default_ttl_ns =
            get_u64(object, "default_ttl_ns", config.default_ttl_ns);
    }
    return fixture;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string{"missing value for "} + option);
            }
            return argv[++i];
        };

        if (arg == "--intent-fixture") {
            args.intent_fixture = require_value("--intent-fixture");
        } else if (arg == "--depth-fixture") {
            args.depth_fixture = require_value("--depth-fixture");
        } else if (arg == "--bundle-fixture") {
            args.bundle_fixture = require_value("--bundle-fixture");
        } else if (arg == "--risk-policy") {
            args.risk_policy = require_value("--risk-policy");
        } else if (arg == "--check-determinism") {
            args.check_determinism = true;
        } else if (arg == "--require-decision") {
            args.require_decision = true;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.intent_fixture.empty() || args.depth_fixture.empty() ||
        args.bundle_fixture.empty() || args.risk_policy.empty()) {
        fail(
            "usage: verify_order_decision_workflow --intent-fixture <jsonl> "
            "--depth-fixture <json> --bundle-fixture <json> "
            "--risk-policy <json> [--check-determinism]"
        );
    }
    return args;
}

void mix_reject_reason(
    std::uint64_t* hash,
    decision::OrderDecisionType reason
) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(reason));
}

const oracle::CandidateBundle* find_bundle(
    const std::vector<oracle::CandidateBundle>& bundles,
    std::uint64_t bundle_id
) noexcept {
    for (const auto& bundle : bundles) {
        if (bundle.bundle_id == bundle_id) {
            return &bundle;
        }
    }
    return nullptr;
}

WorkflowSummary run_once(const Args& args) {
    const auto depth = load_depth_fixture(args.depth_fixture);
    auto intents = load_intents_jsonl(args.intent_fixture, depth.asset_indices);
    auto bundles = load_bundles(args.bundle_fixture);
    const auto policy_fixture = load_policy_fixture(args.risk_policy);
    const auto artifact_hash = oracle::hash_candidate_bundles(bundles);

    decision::OrderDecisionEngine engine{policy_fixture.order_config};

    WorkflowSummary summary;
    summary.intents_loaded = intents.size();

    for (auto& intent : intents) {
        const auto* bundle = find_bundle(bundles, intent.bundle_id);
        if (bundle == nullptr) {
            ++summary.rejected_insufficient_depth;
            mix_reject_reason(
                &summary.decision_hash,
                decision::OrderDecisionType::RejectInvalidBundle
            );
            continue;
        }
        if (intent.bundle_hash == 0) {
            intent.bundle_hash = oracle::hash_candidate_bundle(*bundle);
        }
        if (intent.oracle_artifact_hash == 0) {
            intent.oracle_artifact_hash = artifact_hash;
        }
        if (intent.snapshot_version_hash == 0) {
            intent.snapshot_version_hash =
                state::hash_depth_views(depth.depth_views);
        }

        const auto result = engine.decide(
            intent,
            *bundle,
            depth.depth_views,
            policy_fixture.policy,
            policy_fixture.now_ns
        );

        if (!result.ok) {
            if (result.reject_reason ==
                    decision::OrderDecisionType::RejectNoDepth ||
                result.reject_reason ==
                    decision::OrderDecisionType::RejectPartialFillRisk) {
                ++summary.rejected_insufficient_depth;
            } else if (result.reject_reason ==
                       decision::OrderDecisionType::RejectLowEdge) {
                ++summary.rejected_low_edge;
            }
            mix_reject_reason(&summary.decision_hash, result.reject_reason);
            mix_string(&summary.decision_hash, result.error);
            continue;
        }

        ++summary.decisions_created;
        summary.chosen_bundle_qty_total +=
            result.decision.chosen_bundle_qty;
        summary.total_expected_edge += result.decision.total_edge_tick;
        mix_u64(&summary.decision_hash, result.decision.decision_hash);
        mix_i64(&summary.decision_hash, result.decision.chosen_bundle_qty);
        mix_i64(&summary.decision_hash, result.decision.total_edge_tick);
    }

    return summary;
}

bool same_summary(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.intents_loaded == rhs.intents_loaded &&
           lhs.decisions_created == rhs.decisions_created &&
           lhs.rejected_low_edge == rhs.rejected_low_edge &&
           lhs.rejected_insufficient_depth ==
               rhs.rejected_insufficient_depth &&
           lhs.chosen_bundle_qty_total == rhs.chosen_bundle_qty_total &&
           lhs.total_expected_edge == rhs.total_expected_edge &&
           lhs.decision_hash == rhs.decision_hash;
}

void print_summary(const WorkflowSummary& summary) {
    const auto avg_qty = summary.decisions_created == 0
        ? 0
        : summary.chosen_bundle_qty_total /
              static_cast<std::int64_t>(summary.decisions_created);

    std::cout << "order_decision:\n";
    std::cout << "  intents_loaded: " << summary.intents_loaded << "\n";
    std::cout << "  decisions_created: " << summary.decisions_created << "\n";
    std::cout << "  rejected_low_edge: " << summary.rejected_low_edge << "\n";
    std::cout << "  rejected_insufficient_depth: "
              << summary.rejected_insufficient_depth << "\n";
    std::cout << "  avg_chosen_bundle_qty: " << avg_qty << "\n";
    std::cout << "  total_expected_edge: " << summary.total_expected_edge
              << "\n";
    std::cout << "  decision_hash: " << summary.decision_hash << "\n";
    std::cout << "  determinism_passed: "
              << (summary.determinism_passed ? "true" : "false") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        auto summary = run_once(args);
        if (args.check_determinism) {
            const auto second = run_once(args);
            summary.determinism_passed = same_summary(summary, second);
        }

        print_summary(summary);

        if (args.check_determinism && !summary.determinism_passed) {
            return 2;
        }
        if (summary.intents_loaded == 0) {
            return 3;
        }
        if (args.require_decision && summary.decisions_created == 0) {
            return 4;
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}

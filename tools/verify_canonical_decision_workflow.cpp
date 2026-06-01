#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "engine/execution/plan/ExecutionPlanner.h"
#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/publish/CapturingRiskPublisher.h"
#include "engine/risk/public/RiskPolicySnapshot.h"
#include "engine/signal/core/SignalEngine.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/public/SignalRiskHandoff.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketStateViewSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace decode = trading_engine::decode;
namespace execution = trading_engine::execution;
namespace feed = trading_engine::feed;
namespace json = boost::json;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Config {
    std::filesystem::path raw_path{
        "runs/worldcup_feed_to_execute_30m_20260530_170629/feed.raw"
    };
    std::filesystem::path oracle_artifact{
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    std::filesystem::path policy_config;
    std::uint64_t intent_ttl_ms = 5'000;
    bool check_determinism = true;
};

struct CanonicalSummary {
    std::uint64_t raw_packets = 0;
    std::uint64_t normalized_events = 0;
    std::uint64_t decode_errors = 0;
    std::uint64_t state_errors = 0;
    std::uint64_t filter_passed_events = 0;
    std::uint64_t opportunities = 0;
    std::uint64_t approvals = 0;
    std::uint64_t plans_built = 0;

    std::uint64_t output_hash = kFnvOffset;
    std::uint64_t opportunity_hash = kFnvOffset;
    std::uint64_t risk_hash = kFnvOffset;
    std::uint64_t plan_hash = kFnvOffset;

    bool determinism_checked = false;
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
    for (const unsigned char c : value) {
        *hash ^= c;
        *hash *= kFnvPrime;
    }
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        fail("failed to open " + path.string());
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

[[nodiscard]] json::value parse_json_text(
    const std::string& text,
    const std::string& label
) {
    boost::system::error_code error;
    auto parsed = json::parse(text, error);
    if (error) {
        fail("failed to parse json " + label + ": " + error.message());
    }
    return parsed;
}

[[nodiscard]] const json::object& as_object(
    const json::value& value,
    const std::string& label
) {
    if (!value.is_object()) {
        fail(label + " is not an object");
    }
    return value.as_object();
}

[[nodiscard]] bool get_bool(
    const json::object& object,
    const char* key,
    bool fallback
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_bool()) {
        fail(std::string("expected bool for ") + key);
    }
    return value->as_bool();
}

[[nodiscard]] std::uint64_t get_u64(
    const json::object& object,
    const char* key,
    std::uint64_t fallback
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_uint64()) {
        return value->as_uint64();
    }
    if (value->is_int64() && value->as_int64() >= 0) {
        return static_cast<std::uint64_t>(value->as_int64());
    }
    fail(std::string("expected u64 for ") + key);
}

[[nodiscard]] std::uint32_t get_u32(
    const json::object& object,
    const char* key,
    std::uint32_t fallback
) {
    const auto value = get_u64(object, key, fallback);
    if (value > UINT32_MAX) {
        fail(std::string("u32 overflow for ") + key);
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::int64_t get_i64(
    const json::object& object,
    const char* key,
    std::int64_t fallback
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_int64()) {
        return value->as_int64();
    }
    if (value->is_uint64() && value->as_uint64() <= INT64_MAX) {
        return static_cast<std::int64_t>(value->as_uint64());
    }
    fail(std::string("expected i64 for ") + key);
}

[[nodiscard]] double get_double(
    const json::object& object,
    const char* key,
    double fallback
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
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
    fail(std::string("expected number for ") + key);
}

[[nodiscard]] risk::RiskPolicySnapshot default_worldcup_policy() {
    risk::RiskPolicySnapshot policy;
    policy.max_book_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    policy.max_intent_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    policy.max_snapshot_skew_ns = 1'000'000;
    policy.max_allowed_cost_drift_tick = 1'000'000'000;
    policy.min_depth_margin_ratio = 1.0;
    policy.min_depth_margin_bps = 10'000;
    policy.max_approvals_per_second = 1'000'000;
    return risk::with_computed_policy_hash(policy);
}

[[nodiscard]] risk::RiskPolicySnapshot load_policy_config(
    const std::filesystem::path& path
) {
    if (path.empty()) {
        return default_worldcup_policy();
    }

    const auto parsed = parse_json_text(read_file(path), path.string());
    const auto& root = as_object(parsed, "policy config");

    const json::object* policy_object = &root;
    if (const auto* value = root.if_contains("policy");
        value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    } else if (const auto* value = root.if_contains("risk");
               value != nullptr && value->is_object()) {
        policy_object = &value->as_object();
    }

    risk::RiskPolicySnapshot policy;
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
    return policy;
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string(option) + " requires value");
            }
            return argv[++i];
        };

        if (arg == "--raw") {
            config.raw_path = require_value("--raw");
        } else if (arg == "--artifact") {
            config.oracle_artifact = require_value("--artifact");
        } else if (arg == "--policy-config" || arg == "--risk-config") {
            config.policy_config = require_value(arg.c_str());
        } else if (arg == "--intent-ttl-ms") {
            config.intent_ttl_ms =
                std::stoull(require_value("--intent-ttl-ms"));
        } else if (arg == "--check-determinism") {
            config.check_determinism = true;
        } else if (arg == "--no-determinism") {
            config.check_determinism = false;
        } else if (arg == "--help" || arg == "-h") {
            fail(
                "usage: verify_canonical_decision_workflow "
                "--raw PATH --artifact PATH [--policy-config PATH] "
                "[--check-determinism|--no-determinism]"
            );
        } else {
            fail("unknown argument: " + arg);
        }
    }
    return config;
}

[[nodiscard]] std::vector<feed::RawPacket> load_packets(
    const std::filesystem::path& raw_path
) {
    feed::RawLogReader reader(raw_path.string());
    std::vector<feed::RawPacket> packets;

    while (true) {
        auto raw = reader.next();
        if (raw.eof()) {
            break;
        }
        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }
        packets.push_back(std::move(*raw.packet));
    }

    return packets;
}

[[nodiscard]] state::StateUniverse state_universe_from_artifact(
    const signal::OracleArtifactReader& reader
) {
    state::StateUniverse universe;
    for (const auto& bundle : reader.active_bundles()) {
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            const auto& leg = bundle.legs[i];
            if (!leg.asset_id.empty()) {
                universe.active_asset_ids.insert(leg.asset_id);
            }
            if (!leg.market_id.empty()) {
                universe.active_market_ids.insert(leg.market_id);
            }
        }
    }
    return universe;
}

void mix_intent(
    std::uint64_t* hash,
    const signal::OpportunityIntent& intent
) noexcept {
    mix_u64(hash, intent.intent_id);
    mix_u64(hash, intent.bundle_id);
    mix_u64(hash, static_cast<std::uint64_t>(intent.status));
    mix_u64(hash, intent.oracle_artifact_hash);
    mix_u64(hash, intent.constraint_hash);
    mix_u64(hash, intent.bundle_hash);
    mix_u64(hash, intent.snapshot_version);
    mix_u64(hash, intent.snapshot_version_hash);
    mix_i64(hash, intent.bundle_qty);
    mix_i64(hash, intent.unit_edge_tick);
    mix_i64(hash, intent.total_edge_tick);
    mix_i64(hash, intent.edge_bps);
    mix_u64(hash, intent.idempotency_hash);
    mix_u64(hash, intent.proof_hash);
    mix_u64(hash, intent.leg_count);
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        mix_string(hash, leg.market_id);
        mix_string(hash, leg.asset_id);
        mix_u64(hash, static_cast<std::uint64_t>(leg.side));
        mix_i64(hash, leg.quantity_lots);
        mix_i64(hash, leg.estimated_vwap_tick);
        mix_i64(hash, leg.worst_price_tick);
        mix_i64(hash, leg.estimated_cost_tick);
        mix_i64(hash, leg.requested_qty_lots);
        mix_i64(hash, leg.executable_qty_lots);
        mix_i64(hash, leg.depth_margin_bps);
        mix_u64(hash, leg.enough_depth ? 1ULL : 0ULL);
    }
}

void mix_risk_result(
    std::uint64_t* hash,
    const risk::RiskPipelineResult& result
) noexcept {
    mix_u64(hash, result.output_hash);
    mix_u64(hash, result.decision.decision_id);
    mix_u64(hash, static_cast<std::uint64_t>(result.decision.status));
    mix_u64(hash, static_cast<std::uint64_t>(result.decision.reject_reason));
    mix_u64(hash, result.decision.intent_id);
    mix_u64(hash, result.decision.bundle_id);
    mix_u64(hash, result.decision.policy_hash);
    mix_u64(hash, result.reservation.reservation_id);
    mix_i64(hash, result.cost.risk_total_cost_tick);
    mix_i64(hash, result.cost.risk_bundle_qty);
    mix_i64(hash, result.cost.cost_drift_tick);
    mix_u64(hash, static_cast<std::uint64_t>(result.cost.vwap_mode));
}

void mix_plan(
    std::uint64_t* hash,
    const execution::OrderPlan& plan
) noexcept {
    mix_u64(hash, plan.plan_id);
    mix_u64(hash, plan.source_intent_id);
    mix_u64(hash, plan.approved_intent_id);
    mix_u64(hash, plan.reservation_id);
    mix_u64(hash, plan.bundle_id);
    mix_u64(hash, static_cast<std::uint64_t>(plan.status));
    mix_u64(hash, plan.order_count);
    mix_i64(hash, plan.max_total_cost_tick);
    mix_i64(hash, plan.min_expected_edge_tick);
    mix_i64(hash, plan.max_slippage_tick);
    mix_string(hash, plan.idempotency_key);
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        const auto& order = plan.orders[i];
        mix_u64(hash, order.order_id);
        mix_u64(hash, order.plan_id);
        mix_string(hash, order.client_order_id);
        mix_string(hash, order.market_id);
        mix_string(hash, order.asset_id);
        mix_u64(hash, static_cast<std::uint64_t>(order.side));
        mix_i64(hash, order.quantity_lots);
        mix_i64(hash, order.limit_price_tick);
        mix_i64(hash, order.estimated_vwap_tick);
        mix_i64(hash, order.worst_allowed_price_tick);
        mix_u64(hash, static_cast<std::uint64_t>(order.status));
    }
}

[[nodiscard]] bool same_summary(
    const CanonicalSummary& lhs,
    const CanonicalSummary& rhs
) noexcept {
    return lhs.raw_packets == rhs.raw_packets &&
           lhs.normalized_events == rhs.normalized_events &&
           lhs.decode_errors == rhs.decode_errors &&
           lhs.state_errors == rhs.state_errors &&
           lhs.filter_passed_events == rhs.filter_passed_events &&
           lhs.opportunities == rhs.opportunities &&
           lhs.approvals == rhs.approvals &&
           lhs.plans_built == rhs.plans_built &&
           lhs.output_hash == rhs.output_hash &&
           lhs.opportunity_hash == rhs.opportunity_hash &&
           lhs.risk_hash == rhs.risk_hash &&
           lhs.plan_hash == rhs.plan_hash;
}

CanonicalSummary run_canonical(
    const Config& config,
    const risk::RiskPolicySnapshot& policy
) {
    signal::OracleArtifactReader artifact_reader;
    const auto artifact = artifact_reader.load(config.oracle_artifact);
    if (!artifact.ok) {
        fail("failed to load oracle artifact: " + artifact.error);
    }

    const auto packets = load_packets(config.raw_path);
    if (packets.empty()) {
        fail("raw log contains no packets");
    }

    const auto universe = state_universe_from_artifact(artifact_reader);
    state::MarketStateEventFilter filter(universe);

    decode::DecodePipeline decode_pipeline;
    state::MarketStateStore market_store;
    state::MarketStateView market_view(market_store);
    signal::MarketStateViewSnapshotReader snapshot_reader(market_view);
    signal::SettlementMaskChecker settlement_checker;
    signal::VWAPPrecheck vwap;
    signal::FeeModel fee_model(0);
    signal::LatencyBufferModel latency_model(0);
    signal::TheoreticalEdgeCalculator edge_calculator(
        fee_model,
        latency_model
    );
    signal::OpportunityRanker ranker;
    signal::CapturingIntentPublisher signal_publisher;

    signal::SignalConfig signal_config;
    signal_config.emit_rejections = true;
    signal_config.max_lob_age_ns = 30 * static_cast<std::int64_t>(kNsPerSecond);
    signal_config.max_snapshot_version_skew = 1'000'000;
    signal_config.max_intents_per_second = 1'000'000;
    signal_config.max_intents_per_scan = 1024;
    signal_config.intent_ttl_ns = config.intent_ttl_ms * kNsPerMs;

    signal::SignalEngine signal_engine(
        signal_config,
        &snapshot_reader,
        &artifact_reader,
        &settlement_checker,
        &vwap,
        &edge_calculator,
        &ranker,
        &signal_publisher
    );

    risk::CapturingRiskPublisher risk_publisher;
    risk::RiskRuntimeContext risk_runtime;
    risk_runtime.policy = &policy;
    risk_runtime.publisher = &risk_publisher;
    risk::RiskEngine risk_engine(risk_runtime);

    execution::ExecutionPlanner planner;
    execution::ExecutionConfig execution_config;
    execution_config.execution_enabled = true;
    execution_config.mode = execution::ExecutionMode::Paper;
    execution_config.paper_mode = execution::PaperExecutionMode::PaperAtomic;
    execution_config.max_child_orders_per_plan = 16;
    execution_config.max_order_age_ns =
        static_cast<std::int64_t>(config.intent_ttl_ms * kNsPerMs);

    CanonicalSummary summary;
    std::uint64_t scan_id = 1;

    for (const auto& packet : packets) {
        ++summary.raw_packets;
        decode::NormalizedEventBatch batch;
        const auto decoded = decode_pipeline.decode(
            feed::to_decode_input_view(packet),
            &batch
        );
        summary.normalized_events +=
            static_cast<std::uint64_t>(batch.size());
        if (!decoded.ok() &&
            decoded.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
            ++summary.decode_errors;
        }

        for (const auto& event : state::from_normalized_batch(batch)) {
            const auto filter_result = filter.filter(event);
            if (!filter_result.passed()) {
                continue;
            }
            ++summary.filter_passed_events;

            const auto apply_result = market_store.apply(event);
            if (!apply_result.ok()) {
                ++summary.state_errors;
            }

            signal::SignalScanContext scan_context;
            scan_context.scan_id = scan_id++;
            scan_context.now_monotonic_ns =
                event.recv_monotonic_ns != 0
                    ? event.recv_monotonic_ns
                    : packet.header.recv_monotonic_ns;
            scan_context.settlement_masks_available = false;

            const auto signal_result = signal_engine.scan_once(scan_context);
            summary.opportunities += signal_result.paper_opportunities;

            const auto intents = signal_engine.last_published_intents();
            for (std::size_t intent_index = 0; intent_index < intents.size();
                 ++intent_index) {
                const auto& intent = intents[intent_index];
                const auto evidence =
                    signal_engine.last_published_evidence_at(intent_index);
                if (intent.status != signal::IntentStatus::PaperOpportunity) {
                    continue;
                }
                mix_intent(&summary.opportunity_hash, intent);

                const auto handoff = signal::make_signal_risk_handoff(
                    intent,
                    evidence,
                    scan_context.now_monotonic_ns
                );
                const auto risk_input = risk::make_risk_input_view(handoff);
                const auto risk_result = risk_engine.evaluate(risk_input);
                mix_risk_result(&summary.risk_hash, risk_result);
                if (!risk_result.decision.approved()) {
                    continue;
                }
                ++summary.approvals;

                auto execution_intent = intent;
                if (execution_intent.idempotency_key.empty() &&
                    execution_intent.idempotency_hash != 0) {
                    execution_intent.idempotency_key =
                        std::to_string(execution_intent.idempotency_hash);
                }

                execution::ApprovedIntentEnvelope envelope;
                envelope.source_intent = execution_intent;
                envelope.approval.decision_id =
                    risk_result.output_hash != 0
                        ? risk_result.output_hash
                        : intent.intent_id;
                envelope.approval.reservation_id =
                    risk_result.reservation.reservation_id;
                envelope.approval.bundle_id = intent.bundle_id;
                envelope.approval.approved_bundle_qty =
                    risk_result.cost.risk_bundle_qty > 0
                        ? risk_result.cost.risk_bundle_qty
                        : intent.bundle_qty;
                envelope.approval.idempotency_key =
                    execution_intent.idempotency_key;

                const auto plan_result = planner.build_plan(
                    envelope,
                    scan_context.now_monotonic_ns,
                    execution_config
                );
                if (!plan_result.ok) {
                    continue;
                }
                ++summary.plans_built;
                mix_plan(&summary.plan_hash, plan_result.plan);
            }
        }
    }

    mix_u64(&summary.output_hash, summary.filter_passed_events);
    mix_u64(&summary.output_hash, summary.opportunities);
    mix_u64(&summary.output_hash, summary.approvals);
    mix_u64(&summary.output_hash, summary.plans_built);
    mix_u64(&summary.output_hash, summary.opportunity_hash);
    mix_u64(&summary.output_hash, summary.risk_hash);
    mix_u64(&summary.output_hash, summary.plan_hash);
    return summary;
}

void print_report(
    const Config& config,
    const risk::RiskPolicySnapshot& policy,
    const CanonicalSummary& summary
) {
    std::cout << "canonical_decision_workflow:\n";
    std::cout << "  raw: " << config.raw_path.string() << '\n';
    std::cout << "  oracle_artifact: "
              << config.oracle_artifact.string() << '\n';
    std::cout << "  policy_config: "
              << (config.policy_config.empty()
                      ? std::string("default_worldcup_policy")
                      : config.policy_config.string())
              << '\n';
    std::cout << "  policy_hash: " << policy.policy_hash << "\n\n";

    std::cout << "canonical:\n";
    std::cout << "  filter_passed_events: "
              << summary.filter_passed_events << '\n';
    std::cout << "  opportunities: " << summary.opportunities << '\n';
    std::cout << "  approvals: " << summary.approvals << '\n';
    std::cout << "  plans_built: " << summary.plans_built << '\n';
    std::cout << "  output_hash: " << summary.output_hash << '\n';
    std::cout << "  opportunity_hash: "
              << summary.opportunity_hash << '\n';
    std::cout << "  risk_hash: " << summary.risk_hash << '\n';
    std::cout << "  plan_hash: " << summary.plan_hash << '\n';
    std::cout << "  determinism_passed: "
              << (summary.determinism_passed ? "true" : "false") << '\n';
    std::cout << "  decode_errors: " << summary.decode_errors << '\n';
    std::cout << "  state_errors: " << summary.state_errors << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parse_args(argc, argv);
        const auto policy = load_policy_config(config.policy_config);
        auto summary = run_canonical(config, policy);
        if (config.check_determinism) {
            const auto verifier = run_canonical(config, policy);
            summary.determinism_checked = true;
            summary.determinism_passed = same_summary(summary, verifier);
        }
        print_report(config, policy, summary);
        const bool passed =
            summary.determinism_passed &&
            summary.decode_errors == 0 &&
            summary.state_errors == 0;
        return passed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "verify_canonical_decision_workflow failed: "
                  << error.what() << '\n';
        return 1;
    }
}

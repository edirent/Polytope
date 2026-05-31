#include "engine/signal/publish/JsonlIntentWriter.h"

#include "oracle/bundles/CandidateBundle.h"

#include <ostream>
#include <string_view>

namespace trading_engine::signal {

namespace {

[[nodiscard]] const char* intent_status_to_string(IntentStatus status) noexcept {
    switch (status) {
        case IntentStatus::CandidateOnly:
            return "CandidateOnly";
        case IntentStatus::RejectedInvalidSettlement:
            return "RejectedInvalidSettlement";
        case IntentStatus::RejectedBadMarketState:
            return "RejectedBadMarketState";
        case IntentStatus::RejectedMissingSnapshot:
            return "RejectedMissingSnapshot";
        case IntentStatus::RejectedInsufficientDepth:
            return "RejectedInsufficientDepth";
        case IntentStatus::RejectedLowEdge:
            return "RejectedLowEdge";
        case IntentStatus::DuplicateIntent:
            return "DuplicateIntent";
        case IntentStatus::PaperOpportunity:
            return "PaperOpportunity";
    }
    return "CandidateOnly";
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const char ch : value) {
        switch (ch) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                output << ch;
                break;
        }
    }
    output << '"';
}

void write_bool_field(
    std::ostream& output,
    const char* name,
    bool value
) {
    output << ',';
    write_json_string(output, name);
    output << ':' << (value ? "true" : "false");
}

void write_i64_field(
    std::ostream& output,
    const char* name,
    std::int64_t value
) {
    output << ',';
    write_json_string(output, name);
    output << ':' << value;
}

void write_u64_field(
    std::ostream& output,
    const char* name,
    std::uint64_t value
) {
    output << ',';
    write_json_string(output, name);
    output << ':' << value;
}

}  // namespace

JsonlIntentWriter::JsonlIntentWriter(std::ostream* output)
    : output_(output) {}

bool JsonlIntentWriter::write(const OpportunityIntent& intent) {
    if (!output_ || !*output_) {
        return false;
    }

    auto& output = *output_;
    output << '{';
    write_json_string(output, "intent_id");
    output << ':' << intent.intent_id;
    write_u64_field(output, "bundle_id", intent.bundle_id);
    output << ',';
    write_json_string(output, "status");
    output << ':';
    write_json_string(output, intent_status_to_string(intent.status));
    write_bool_field(
        output,
        "valid_under_settlement",
        intent.valid_under_settlement
    );
    write_bool_field(output, "passed_quality_gate", intent.passed_quality_gate);
    write_bool_field(output, "enough_depth", intent.enough_depth);
    write_i64_field(
        output,
        "guaranteed_payout_tick",
        intent.guaranteed_payout_tick
    );
    write_i64_field(output, "estimated_cost_tick", intent.estimated_cost_tick);
    write_i64_field(output, "estimated_fee_tick", intent.estimated_fee_tick);
    write_i64_field(
        output,
        "latency_buffer_tick",
        intent.latency_buffer_tick
    );
    write_i64_field(output, "estimated_edge_tick", intent.estimated_edge_tick);
    write_i64_field(output, "min_edge_tick", intent.min_edge_tick);
    write_u64_field(
        output,
        "oracle_artifact_hash",
        intent.oracle_artifact_hash
    );
    write_u64_field(output, "constraint_hash", intent.constraint_hash);
    write_u64_field(output, "bundle_hash", intent.bundle_hash);
    write_u64_field(output, "snapshot_version", intent.snapshot_version);
    write_i64_field(output, "bundle_qty", intent.bundle_qty);
    write_i64_field(output, "original_bundle_qty", intent.original_bundle_qty);
    write_i64_field(output, "unit_edge_tick", intent.unit_edge_tick);
    write_i64_field(output, "total_edge_tick", intent.total_edge_tick);
    write_i64_field(output, "edge_bps", intent.edge_bps);
    write_i64_field(
        output,
        "slippage_buffer_tick",
        intent.slippage_buffer_tick
    );
    write_i64_field(
        output,
        "max_leg_slippage_tick",
        intent.max_leg_slippage_tick
    );
    write_u64_field(output, "created_ts_ns", intent.created_ts_ns);
    write_u64_field(output, "expires_at_ns", intent.expires_at_ns);
    write_u64_field(
        output,
        "snapshot_version_hash",
        intent.snapshot_version_hash
    );
    write_u64_field(
        output,
        "oracle_artifact_version",
        intent.oracle_artifact_version
    );
    output << ',';
    write_json_string(output, "idempotency_key");
    output << ':';
    write_json_string(output, intent.idempotency_key);
    output << ',';
    write_json_string(output, "proof_ref");
    output << ':';
    write_json_string(output, intent.proof_ref);
    output << ',';
    write_json_string(output, "reject_reason");
    output << ':';
    write_json_string(output, intent.reject_reason);
    output << ",\"legs\":[";
    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        if (i > 0) {
            output << ',';
        }
        const auto& leg = intent.legs[i];
        output << '{';
        write_json_string(output, "market_id");
        output << ':';
        write_json_string(output, leg.market_id);
        output << ',';
        write_json_string(output, "asset_id");
        output << ':';
        write_json_string(output, leg.asset_id);
        output << ',';
        write_json_string(output, "side");
        output << ':';
        write_json_string(output, oracle::side_to_string(leg.side));
        write_i64_field(output, "quantity_lots", leg.quantity_lots);
        write_i64_field(output, "estimated_vwap_tick", leg.estimated_vwap_tick);
        write_i64_field(output, "worst_price_tick", leg.worst_price_tick);
        write_i64_field(output, "estimated_cost_tick", leg.estimated_cost_tick);
        write_i64_field(output, "requested_qty_lots", leg.requested_qty_lots);
        write_i64_field(
            output,
            "executable_qty_lots",
            leg.executable_qty_lots
        );
        write_i64_field(output, "depth_margin_bps", leg.depth_margin_bps);
        write_bool_field(output, "enough_depth", leg.enough_depth);
        output << '}';
    }
    output << "]}\n";
    return static_cast<bool>(output);
}

}  // namespace trading_engine::signal

#include "engine/risk/publish/JsonlRiskDecisionWriter.h"

#include <ostream>
#include <string_view>

namespace trading_engine::risk {

namespace {

[[nodiscard]] const char* decision_status_to_string(
    RiskDecisionStatus status
) noexcept {
    switch (status) {
        case RiskDecisionStatus::Rejected:
            return "Rejected";
        case RiskDecisionStatus::Approved:
            return "Approved";
    }
    return "Rejected";
}

[[nodiscard]] const char* reject_reason_to_string(
    RiskRejectReason reason
) noexcept {
    switch (reason) {
        case RiskRejectReason::None:
            return "None";
        case RiskRejectReason::NotEvaluated:
            return "NotEvaluated";
        case RiskRejectReason::RiskDisabled:
            return "RiskDisabled";
        case RiskRejectReason::KillSwitch:
            return "KillSwitch";
        case RiskRejectReason::LowTotalEdge:
            return "LowTotalEdge";
        case RiskRejectReason::LowUnitEdge:
            return "LowUnitEdge";
        case RiskRejectReason::LowEdgeBps:
            return "LowEdgeBps";
        case RiskRejectReason::CostLimit:
            return "CostLimit";
        case RiskRejectReason::SingleMarketExposureLimit:
            return "SingleMarketExposureLimit";
        case RiskRejectReason::TotalExposureLimit:
            return "TotalExposureLimit";
        case RiskRejectReason::InventoryLimit:
            return "InventoryLimit";
        case RiskRejectReason::BadMarketState:
            return "BadMarketState";
        case RiskRejectReason::StaleBook:
            return "StaleBook";
        case RiskRejectReason::ExpiredIntent:
            return "ExpiredIntent";
        case RiskRejectReason::SnapshotSkew:
            return "SnapshotSkew";
        case RiskRejectReason::CostDrift:
            return "CostDrift";
        case RiskRejectReason::SlippageLimit:
            return "SlippageLimit";
        case RiskRejectReason::PendingIntentLimit:
            return "PendingIntentLimit";
        case RiskRejectReason::ApprovalRateLimit:
            return "ApprovalRateLimit";
        case RiskRejectReason::MissingReservation:
            return "MissingReservation";
        case RiskRejectReason::InvalidIntent:
            return "InvalidIntent";
        case RiskRejectReason::MissingEvidence:
            return "MissingEvidence";
        case RiskRejectReason::DuplicateIntent:
            return "DuplicateIntent";
        case RiskRejectReason::DuplicateReservation:
            return "DuplicateReservation";
        case RiskRejectReason::PartialFillRisk:
            return "PartialFillRisk";
        case RiskRejectReason::InternalError:
            return "InternalError";
    }
    return "InternalError";
}

[[nodiscard]] const char* decision_type_to_string(
    RiskDecisionType type
) noexcept {
    switch (type) {
        case RiskDecisionType::Pass:
            return "Pass";
        case RiskDecisionType::RejectKillSwitch:
            return "RejectKillSwitch";
        case RiskDecisionType::RejectExpiredIntent:
            return "RejectExpiredIntent";
        case RiskDecisionType::RejectDuplicateIntent:
            return "RejectDuplicateIntent";
        case RiskDecisionType::RejectRateLimited:
            return "RejectRateLimited";
        case RiskDecisionType::RejectBadMarketState:
            return "RejectBadMarketState";
        case RiskDecisionType::RejectStaleSnapshot:
            return "RejectStaleSnapshot";
        case RiskDecisionType::RejectInsufficientDepth:
            return "RejectInsufficientDepth";
        case RiskDecisionType::RejectCostDrift:
            return "RejectCostDrift";
        case RiskDecisionType::RejectReducedBundleQty:
            return "RejectReducedBundleQty";
        case RiskDecisionType::RejectLowTotalEdge:
            return "RejectLowTotalEdge";
        case RiskDecisionType::RejectLowUnitEdge:
            return "RejectLowUnitEdge";
        case RiskDecisionType::RejectLowEdgeBps:
            return "RejectLowEdgeBps";
        case RiskDecisionType::RejectCostLimit:
            return "RejectCostLimit";
        case RiskDecisionType::RejectTotalExposureLimit:
            return "RejectTotalExposureLimit";
        case RiskDecisionType::RejectSingleMarketExposureLimit:
            return "RejectSingleMarketExposureLimit";
        case RiskDecisionType::RejectInventoryLimit:
            return "RejectInventoryLimit";
        case RiskDecisionType::RejectPartialFillRisk:
            return "RejectPartialFillRisk";
        case RiskDecisionType::RejectInternalError:
            return "RejectInternalError";
    }
    return "RejectInternalError";
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

JsonlRiskDecisionWriter::JsonlRiskDecisionWriter(std::ostream* output)
    : output_(output) {}

bool JsonlRiskDecisionWriter::write(
    const RiskDecision& decision,
    const RiskAuditTrace& trace
) {
    if (!output_ || !*output_) {
        return false;
    }

    auto& output = *output_;
    output << '{';
    write_json_string(output, "decision_id");
    output << ':' << decision.decision_id;
    write_u64_field(output, "trace_id", trace.trace_id);
    write_u64_field(
        output,
        "intent_id",
        decision.intent_id != 0 ? decision.intent_id : trace.intent_id
    );
    write_u64_field(
        output,
        "bundle_id",
        decision.bundle_id != 0 ? decision.bundle_id : trace.bundle_id
    );
    write_u64_field(output, "idempotency_hash", decision.idempotency_hash);
    write_u64_field(
        output,
        "oracle_artifact_hash",
        decision.oracle_artifact_hash
    );
    write_u64_field(output, "constraint_hash", decision.constraint_hash);
    write_u64_field(output, "bundle_hash", decision.bundle_hash);
    write_u64_field(
        output,
        "snapshot_version_hash",
        decision.snapshot_version_hash
    );
    write_u64_field(output, "policy_version", decision.policy_version);
    write_u64_field(output, "policy_hash", decision.policy_hash);
    output << ',';
    write_json_string(output, "status");
    output << ':';
    write_json_string(output, decision_status_to_string(decision.status));
    output << ',';
    write_json_string(output, "reject_reason");
    output << ':';
    write_json_string(output, reject_reason_to_string(decision.reject_reason));
    output << ',';
    write_json_string(output, "reject_detail");
    output << ':';
    write_json_string(output, decision.reject_detail);
    output << ",\"steps\":[";
    if (!trace.steps.empty()) {
        for (std::size_t i = 0; i < trace.steps.size(); ++i) {
            if (i > 0) {
                output << ',';
            }
            const auto& step = trace.steps[i];
            output << '{';
            write_json_string(output, "guard_name");
            output << ':';
            write_json_string(output, step.guard_name);
            output << ',';
            write_json_string(output, "pass");
            output << ':' << (step.pass ? "true" : "false");
            output << ',';
            write_json_string(output, "rejection");
            output << ':';
            write_json_string(output, decision_type_to_string(step.rejection));
            output << ',';
            write_json_string(output, "reason");
            output << ':';
            write_json_string(output, step.reason);
            output << '}';
        }
    } else {
        const auto& lite = trace.lite;
        for (std::uint8_t i = 0; i < lite.step_count; ++i) {
            if (i > 0) {
                output << ',';
            }
            const auto& step = lite.steps[i];
            output << '{';
            write_json_string(output, "guard_name");
            output << ':';
            write_json_string(output, risk_audit_step_name(step.step));
            output << ',';
            write_json_string(output, "pass");
            output << ':' << (step.pass ? "true" : "false");
            output << ',';
            write_json_string(output, "rejection");
            output << ':';
            write_json_string(output, decision_type_to_string(step.rejection));
            output << ',';
            write_json_string(output, "detail_code");
            output << ':' << step.detail_code;
            output << '}';
        }
    }
    output << "]}\n";
    return static_cast<bool>(output);
}

}  // namespace trading_engine::risk

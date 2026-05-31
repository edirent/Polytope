#include "engine/risk/public/RiskInputView.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalRiskHandoff.h"
#include "engine/state/MarketStateSnapshot.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::make_risk_input_view;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::SignalEvidenceView;
using trading_engine::signal::make_signal_risk_handoff;
using trading_engine::state::MarketStateSnapshot;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

OpportunityIntent intent() {
    OpportunityIntent out;
    out.intent_id = 11;
    out.bundle_id = 22;
    out.status = IntentStatus::PaperOpportunity;
    out.created_ts_ns = 1'000;
    out.expires_at_ns = 2'000;
    out.idempotency_hash = 333;
    out.snapshot_version_hash = 444;
    out.bundle_qty = 5;
    return out;
}

SignalEvidenceView evidence_for(MarketStateSnapshot* snapshot) {
    SignalEvidenceView evidence;
    evidence.snapshots = snapshot;
    evidence.snapshot_count = 1;
    evidence.snapshot_version_hash = 444;
    evidence.read_ts_ns = 1'234;
    return evidence;
}

void SignalToRiskHandoff_PreservesSnapshotVersionHash() {
    auto in = intent();
    MarketStateSnapshot snapshot;
    auto evidence = evidence_for(&snapshot);

    const auto handoff = make_signal_risk_handoff(in, evidence, 1'500);
    const auto view = make_risk_input_view(handoff);

    expect_equal(handoff.snapshot_version_hash, 444ULL, "handoff hash");
    expect_equal(view.snapshot_version_hash, 444ULL, "view hash");
    expect_true(view.snapshots == &snapshot, "snapshot pointer");
    expect_equal(view.snapshot_count, static_cast<std::uint16_t>(1), "count");
}

void SignalToRiskHandoff_DoesNotPatchIntentLifecycle() {
    auto in = intent();
    const auto original_created = in.created_ts_ns;
    const auto original_expires = in.expires_at_ns;
    const auto original_idempotency_hash = in.idempotency_hash;
    MarketStateSnapshot snapshot;
    auto evidence = evidence_for(&snapshot);

    const auto handoff = make_signal_risk_handoff(in, evidence, 1'500);
    const auto view = make_risk_input_view(handoff);

    expect_true(handoff.intent == &in, "handoff intent pointer");
    expect_true(view.intent == &in, "view intent pointer");
    expect_equal(in.created_ts_ns, original_created, "created unchanged");
    expect_equal(in.expires_at_ns, original_expires, "expires unchanged");
    expect_equal(
        in.idempotency_hash,
        original_idempotency_hash,
        "idempotency hash unchanged"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "SignalToRiskHandoff_PreservesSnapshotVersionHash",
            &SignalToRiskHandoff_PreservesSnapshotVersionHash
        },
        {
            "SignalToRiskHandoff_DoesNotPatchIntentLifecycle",
            &SignalToRiskHandoff_DoesNotPatchIntentLifecycle
        },
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& ex) {
        std::cerr << argv[1] << " failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}

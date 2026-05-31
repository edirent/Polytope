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

using trading_engine::risk::RiskInputView;
using trading_engine::risk::make_risk_input_view;
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

void RiskInputView_DoesNotOwnIntent() {
    OpportunityIntent intent;
    MarketStateSnapshot snapshot;
    SignalEvidenceView evidence;
    evidence.snapshots = &snapshot;
    evidence.snapshot_count = 1;
    evidence.snapshot_version_hash = 777;

    const auto handoff = make_signal_risk_handoff(intent, evidence, 1'000);
    const auto view = make_risk_input_view(handoff);

    expect_true(handoff.intent == &intent, "handoff intent pointer");
    expect_true(view.intent == &intent, "view intent pointer");
}

void RiskInputView_DoesNotCopySnapshots() {
    OpportunityIntent intent;
    MarketStateSnapshot snapshots[2];
    SignalEvidenceView evidence;
    evidence.snapshots = snapshots;
    evidence.snapshot_count = 2;
    evidence.snapshot_version_hash = 1234;

    const auto handoff = make_signal_risk_handoff(intent, evidence, 2'000);
    const auto view = make_risk_input_view(handoff);

    expect_true(view.snapshots == snapshots, "snapshot pointer");
    expect_equal(view.snapshot_count, static_cast<std::uint16_t>(2), "count");
}

void RiskInputView_ReferencesSignalSnapshotEvidence() {
    OpportunityIntent intent;
    MarketStateSnapshot snapshot;
    SignalEvidenceView evidence;
    evidence.snapshots = &snapshot;
    evidence.snapshot_count = 1;
    evidence.snapshot_version_hash = 987654321;

    const auto handoff = make_signal_risk_handoff(intent, evidence, 3'000);
    const RiskInputView view = make_risk_input_view(handoff);

    expect_equal(view.snapshot_version_hash, 987654321ULL, "snapshot hash");
    expect_equal(view.latest_snapshot_version_hash, 0ULL, "legacy hash unused");
    expect_equal(view.now_ns, 3'000ULL, "now");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RiskInputView_DoesNotOwnIntent", &RiskInputView_DoesNotOwnIntent},
        {"RiskInputView_DoesNotCopySnapshots", &RiskInputView_DoesNotCopySnapshots},
        {
            "RiskInputView_ReferencesSignalSnapshotEvidence",
            &RiskInputView_ReferencesSignalSnapshotEvidence
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

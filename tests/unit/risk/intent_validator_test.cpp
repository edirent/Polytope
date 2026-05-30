#include "engine/risk/validate/IntentEvidenceVerifier.h"
#include "engine/risk/validate/IntentValidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::IntentEvidenceVerifier;
using trading_engine::risk::IntentValidator;
using trading_engine::risk::RiskRejectReason;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
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

OpportunityIntent make_complete_intent() {
    OpportunityIntent intent;
    intent.intent_id = 101;
    intent.bundle_id = 202;
    intent.status = IntentStatus::PaperOpportunity;
    intent.oracle_artifact_hash = 303;
    intent.bundle_hash = 404;
    intent.snapshot_version = 7;
    intent.snapshot_version_hash = 505;
    intent.bundle_qty = 10;
    intent.total_edge_tick = 1'000;
    intent.created_ts_ns = 1'000;
    intent.expires_at_ns = 2'000;
    intent.idempotency_key = "intent-101";
    intent.leg_count = 1;
    intent.legs[0].market_id = "market-1";
    intent.legs[0].asset_id = "asset-1";
    intent.legs[0].quantity_lots = 10;
    return intent;
}

void IntentValidator_AcceptsCompleteOpportunity() {
    const IntentValidator validator;
    const auto result = validator.validate(make_complete_intent(), 1'500);

    expect_true(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::None, "reason");
}

void IntentValidator_RejectsCandidateOnly() {
    auto intent = make_complete_intent();
    intent.status = IntentStatus::CandidateOnly;

    const IntentValidator validator;
    const auto result = validator.validate(intent, 1'500);

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::InvalidIntent, "reason");
}

void IntentValidator_RejectsExpiredIntent() {
    auto intent = make_complete_intent();
    intent.expires_at_ns = 1'499;

    const IntentValidator validator;
    const auto result = validator.validate(intent, 1'500);

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::ExpiredIntent, "reason");
}

void IntentEvidenceVerifier_RejectsMissingArtifactHash() {
    auto intent = make_complete_intent();
    intent.oracle_artifact_hash = 0;

    const IntentEvidenceVerifier verifier;
    const auto result = verifier.verify(intent);

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::MissingEvidence, "reason");
}

void IntentEvidenceVerifier_RejectsMissingSnapshotHash() {
    auto intent = make_complete_intent();
    intent.snapshot_version_hash = 0;

    const IntentEvidenceVerifier verifier;
    const auto result = verifier.verify(intent);

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::MissingEvidence, "reason");
}

void IntentEvidenceVerifier_RejectsEmptyIdempotencyKey() {
    auto intent = make_complete_intent();
    intent.idempotency_key.clear();

    const IntentEvidenceVerifier verifier;
    const auto result = verifier.verify(intent);

    expect_false(result.ok, "ok");
    expect_equal(result.reject_reason, RiskRejectReason::MissingEvidence, "reason");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "IntentValidator_AcceptsCompleteOpportunity",
            &IntentValidator_AcceptsCompleteOpportunity
        },
        {
            "IntentValidator_RejectsCandidateOnly",
            &IntentValidator_RejectsCandidateOnly
        },
        {
            "IntentValidator_RejectsExpiredIntent",
            &IntentValidator_RejectsExpiredIntent
        },
        {
            "IntentEvidenceVerifier_RejectsMissingArtifactHash",
            &IntentEvidenceVerifier_RejectsMissingArtifactHash
        },
        {
            "IntentEvidenceVerifier_RejectsMissingSnapshotHash",
            &IntentEvidenceVerifier_RejectsMissingSnapshotHash
        },
        {
            "IntentEvidenceVerifier_RejectsEmptyIdempotencyKey",
            &IntentEvidenceVerifier_RejectsEmptyIdempotencyKey
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}

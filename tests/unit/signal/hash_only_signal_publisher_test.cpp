#include "engine/signal/publish/HashOnlySignalPublisher.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::HashOnlySignalPublisher;
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

OpportunityIntent intent(std::uint64_t id = 11) {
    OpportunityIntent out;
    out.intent_id = id;
    out.bundle_id = 22;
    out.status = IntentStatus::PaperOpportunity;
    out.idempotency_hash = 33;
    out.proof_hash = 44;
    out.snapshot_version_hash = 55;
    out.oracle_artifact_hash = 66;
    out.bundle_hash = 77;
    out.bundle_qty = 10;
    out.unit_edge_tick = 100;
    out.total_edge_tick = 1'000;
    out.edge_bps = 125;
    out.idempotency_key = "expensive-key";
    out.proof_ref = "proof-ref";
    out.reject_reason = "not-used";
    return out;
}

void HashOnlySignalPublisher_PublishesNumericRecord() {
    HashOnlySignalPublisher publisher;
    publisher.publish(intent());

    expect_equal(publisher.records().size(), 1U, "record count");
    const auto& record = publisher.records().front();
    expect_equal(record.intent_id, 11ULL, "intent_id");
    expect_equal(record.bundle_id, 22ULL, "bundle_id");
    expect_equal(record.status, IntentStatus::PaperOpportunity, "status");
    expect_equal(record.idempotency_hash, 33ULL, "idempotency_hash");
    expect_equal(record.proof_hash, 44ULL, "proof_hash");
    expect_equal(record.total_edge_tick, 1'000LL, "total edge");
}

void HashOnlySignalPublisher_DoesNotMaterializeStrings() {
    HashOnlySignalPublisher publisher;
    expect_true(!publisher.requires_materialized_strings(), "materialization");

    auto first = intent(1);
    auto second = first;
    second.idempotency_key = "different";
    second.proof_ref = "different-proof";
    second.reject_reason = "different-reason";

    publisher.publish(first);
    publisher.publish(second);

    expect_equal(
        publisher.records()[0].idempotency_hash,
        publisher.records()[1].idempotency_hash,
        "string-independent idempotency hash"
    );
    expect_equal(
        publisher.records()[0].proof_hash,
        publisher.records()[1].proof_hash,
        "string-independent proof hash"
    );
}

void HashOnlySignalPublisher_PreservesIntentIdBundleId() {
    HashOnlySignalPublisher publisher;
    publisher.publish(intent(101));
    publisher.publish(intent(102));

    expect_equal(publisher.records()[0].intent_id, 101ULL, "first intent");
    expect_equal(publisher.records()[1].intent_id, 102ULL, "second intent");
    expect_equal(publisher.records()[1].bundle_id, 22ULL, "bundle");
}

void HashOnlySignalPublisher_DeterministicOutputHash() {
    HashOnlySignalPublisher left;
    HashOnlySignalPublisher right;
    left.publish(intent(1));
    left.publish(intent(2));
    right.publish(intent(1));
    right.publish(intent(2));

    expect_true(left.output_hash() != 0, "hash nonzero");
    expect_equal(left.output_hash(), right.output_hash(), "hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "HashOnlySignalPublisher_PublishesNumericRecord",
            &HashOnlySignalPublisher_PublishesNumericRecord
        },
        {
            "HashOnlySignalPublisher_DoesNotMaterializeStrings",
            &HashOnlySignalPublisher_DoesNotMaterializeStrings
        },
        {
            "HashOnlySignalPublisher_PreservesIntentIdBundleId",
            &HashOnlySignalPublisher_PreservesIntentIdBundleId
        },
        {
            "HashOnlySignalPublisher_DeterministicOutputHash",
            &HashOnlySignalPublisher_DeterministicOutputHash
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
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " TEST_NAME\n";
        return 2;
    }
    return run_test(argv[1]);
}

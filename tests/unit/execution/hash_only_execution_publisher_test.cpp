#include "engine/execution/publish/HashOnlyExecutionPublisher.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::HashOnlyExecutionPublisher;
using trading_engine::execution::ReservationDisposition;
using trading_engine::execution::ReservationDispositionType;

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

ExecutionReport report() {
    ExecutionReport out;
    out.plan_id = 11;
    out.child_order_id = 22;
    out.status = ChildOrderStatus::Filled;
    out.venue_order_id = "venue-order";
    out.reject_reason = "not-used";
    out.filled_lots = 10;
    out.avg_fill_price_tick = 800;
    return out;
}

void HashOnlyExecutionPublisher_PublishesNumericRecord() {
    HashOnlyExecutionPublisher publisher;
    publisher.publish(report());

    expect_equal(publisher.records().size(), 1U, "record count");
    const auto& record = publisher.records().front();
    expect_equal(record.plan_id, 11ULL, "plan id");
    expect_equal(record.status, ChildOrderStatus::Filled, "status");
    expect_equal(record.filled_qty_lots, 10LL, "filled");
    expect_equal(record.total_cost_tick, 8'000LL, "cost");
}

void HashOnlyExecutionPublisher_DoesNotMaterializeStrings() {
    HashOnlyExecutionPublisher publisher;
    auto first = report();
    auto second = first;
    second.venue_order_id = "different";
    second.reject_reason = "different";

    publisher.publish(first);
    publisher.publish(second);

    expect_equal(
        publisher.records()[0].total_cost_tick,
        publisher.records()[1].total_cost_tick,
        "string-independent cost"
    );
}

void HashOnlyExecutionPublisher_PreservesIntentIdBundleId() {
    HashOnlyExecutionPublisher publisher;
    publisher.publish(report());
    publisher.publish(ReservationDisposition{
        .reservation_id = "77",
        .plan_id = 11,
        .type = ReservationDispositionType::Consume,
        .reason = "filled"
    });

    expect_equal(publisher.records()[0].plan_id, 11ULL, "report plan");
    expect_equal(publisher.records()[1].reservation_id, 77ULL, "reservation");
    expect_equal(
        publisher.records()[1].disposition,
        ReservationDispositionType::Consume,
        "disposition"
    );
}

void HashOnlyExecutionPublisher_DeterministicOutputHash() {
    HashOnlyExecutionPublisher left;
    HashOnlyExecutionPublisher right;
    left.publish(report());
    right.publish(report());

    expect_true(left.output_hash() != 0, "hash nonzero");
    expect_equal(left.output_hash(), right.output_hash(), "hash");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "HashOnlyExecutionPublisher_PublishesNumericRecord",
            &HashOnlyExecutionPublisher_PublishesNumericRecord
        },
        {
            "HashOnlyExecutionPublisher_DoesNotMaterializeStrings",
            &HashOnlyExecutionPublisher_DoesNotMaterializeStrings
        },
        {
            "HashOnlyExecutionPublisher_PreservesIntentIdBundleId",
            &HashOnlyExecutionPublisher_PreservesIntentIdBundleId
        },
        {
            "HashOnlyExecutionPublisher_DeterministicOutputHash",
            &HashOnlyExecutionPublisher_DeterministicOutputHash
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

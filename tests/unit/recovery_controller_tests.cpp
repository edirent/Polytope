#include "feed/integrity/RecoveryController.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::feed::ConsistencyCode;
using trading_engine::feed::ConsistencyResult;
using trading_engine::feed::RecoveryAction;
using trading_engine::feed::RecoveryController;
using trading_engine::feed::StaleLevel;
using trading_engine::feed::StaleResult;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal(
    RecoveryAction actual,
    RecoveryAction expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

ConsistencyResult consistency(ConsistencyCode code) {
    return ConsistencyResult{
        .code = code,
        .entity_id = "asset-a",
        .reason = "test consistency result",
        .checked_ns = 1
    };
}

StaleResult stale(StaleLevel level) {
    return StaleResult{
        .level = level,
        .entity_id = "asset-a",
        .reason = "test stale result",
        .age_ns = 1
    };
}

void DeltaBeforeSnapshotRequestsSnapshot() {
    RecoveryController controller;

    const auto action =
        controller.decide(consistency(ConsistencyCode::DeltaBeforeSnapshot));

    expect_equal(
        action,
        RecoveryAction::RequestSnapshot,
        "delta before snapshot"
    );
}

void CrossedBookRequestsSnapshot() {
    RecoveryController controller;

    const auto action =
        controller.decide(consistency(ConsistencyCode::CrossedBook));

    expect_equal(action, RecoveryAction::RequestSnapshot, "crossed book");
}

void SourceStaleReconnectsSource() {
    RecoveryController controller;

    const auto action = controller.decide(stale(StaleLevel::SourceStale));

    expect_equal(action, RecoveryAction::ReconnectSource, "source stale");
}

void DecodeErrorMarksUnsafe() {
    RecoveryController controller;

    const auto action =
        controller.decide(consistency(ConsistencyCode::DecodeError));

    expect_equal(action, RecoveryAction::MarkUnsafe, "decode error");
}

void OkDoesNothing() {
    RecoveryController controller;

    const auto action = controller.decide(consistency(ConsistencyCode::Ok));

    expect_equal(action, RecoveryAction::None, "ok consistency");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"DeltaBeforeSnapshotRequestsSnapshot", &DeltaBeforeSnapshotRequestsSnapshot},
        {"CrossedBookRequestsSnapshot", &CrossedBookRequestsSnapshot},
        {"SourceStaleReconnectsSource", &SourceStaleReconnectsSource},
        {"DecodeErrorMarksUnsafe", &DecodeErrorMarksUnsafe},
        {"OkDoesNothing", &OkDoesNothing}
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

#include "tests/integration/signal/signal_workflow_test_utils.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

using namespace signal_workflow_test;

void SignalEngine_RejectsInsufficientDepth() {
    EngineHarness harness(
        {two_leg_bundle(10)},
        {snapshot("asset_yes", 400'000, 5.0), snapshot("asset_no", 400'000, 10.0)}
    );
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.rejected_insufficient_depth, 1ULL, "depth");
    expect_equal(harness.publisher.intents()[0].status, IntentStatus::RejectedInsufficientDepth, "status");
}

void SignalEngine_RejectsLowEdge() {
    EngineHarness harness(
        {two_leg_bundle()},
        {snapshot("asset_yes", 600'000, 10.0), snapshot("asset_no", 600'000, 10.0)}
    );
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.rejected_low_edge, 1ULL, "low edge");
    expect_equal(harness.publisher.intents()[0].status, IntentStatus::RejectedLowEdge, "status");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "SignalEngine_RejectsInsufficientDepth",
            &SignalEngine_RejectsInsufficientDepth
        },
        {"SignalEngine_RejectsLowEdge", &SignalEngine_RejectsLowEdge}
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
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}

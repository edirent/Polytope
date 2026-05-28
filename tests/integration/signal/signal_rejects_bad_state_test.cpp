#include "tests/integration/signal/signal_workflow_test_utils.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

using namespace signal_workflow_test;

void SignalEngine_PublishesRejections() {
    EngineHarness harness(
        {two_leg_bundle()},
        {
            snapshot("asset_yes", 400'000, 10.0, true),
            snapshot("asset_no", 400'000, 10.0)
        }
    );
    harness.config.emit_rejections = true;
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.intents_published, 1ULL, "published");
    expect_equal(harness.publisher.intents().size(), 1U, "intent count");
    expect_equal(
        harness.publisher.intents()[0].status,
        IntentStatus::RejectedBadMarketState,
        "status"
    );
}

void SignalEngine_RejectsBadMarketState() {
    EngineHarness harness(
        {two_leg_bundle()},
        {
            snapshot("asset_yes", 400'000, 10.0, false, true),
            snapshot("asset_no", 400'000, 10.0)
        }
    );
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.rejected_bad_market_state, 1ULL, "bad state");
    expect_equal(result.paper_opportunities, 0ULL, "paper");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SignalEngine_PublishesRejections", &SignalEngine_PublishesRejections},
        {"SignalEngine_RejectsBadMarketState", &SignalEngine_RejectsBadMarketState}
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

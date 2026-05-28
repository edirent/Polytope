#include "tests/integration/signal/signal_workflow_test_utils.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

using namespace signal_workflow_test;

void SignalEngine_ScansBundles() {
    EngineHarness harness(
        {two_leg_bundle()},
        {snapshot("asset_yes", 400'000, 10.0), snapshot("asset_no", 400'000, 10.0)}
    );
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.bundles_scanned, 1ULL, "bundles");
    expect_equal(result.intents_published, 1ULL, "published");
}

void SignalEngine_PublishesPaperOpportunityWhenEdgePositive() {
    EngineHarness harness(
        {two_leg_bundle()},
        {snapshot("asset_yes", 400'000, 10.0), snapshot("asset_no", 400'000, 10.0)}
    );
    auto engine = harness.make_engine();

    const auto result = engine.scan_once(context());

    expect_equal(result.paper_opportunities, 1ULL, "paper count");
    expect_equal(harness.publisher.intents().size(), 1U, "intent count");
    const auto& intent = harness.publisher.intents()[0];
    expect_equal(intent.status, IntentStatus::PaperOpportunity, "status");
    expect_equal(intent.estimated_cost_tick, 800'000LL, "cost");
    expect_equal(intent.estimated_edge_tick, 200'000LL, "edge");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SignalEngine_ScansBundles", &SignalEngine_ScansBundles},
        {
            "SignalEngine_PublishesPaperOpportunityWhenEdgePositive",
            &SignalEngine_PublishesPaperOpportunityWhenEdgePositive
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

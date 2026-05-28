#include "tests/integration/signal/signal_workflow_test_utils.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

using namespace signal_workflow_test;

void SignalEngine_OutputHashDeterministic() {
    EngineHarness first(
        {two_leg_bundle()},
        {snapshot("asset_yes", 400'000, 10.0), snapshot("asset_no", 400'000, 10.0)}
    );
    EngineHarness second(
        {two_leg_bundle()},
        {snapshot("asset_yes", 400'000, 10.0), snapshot("asset_no", 400'000, 10.0)}
    );

    auto first_engine = first.make_engine();
    auto second_engine = second.make_engine();
    const auto first_result = first_engine.scan_once(context());
    const auto second_result = second_engine.scan_once(context());

    expect_true(first_result.output_hash != 0, "output hash nonzero");
    expect_equal(first_result.output_hash, second_result.output_hash, "hash");
    expect_equal(
        first.publisher.intents()[0].intent_id,
        second.publisher.intents()[0].intent_id,
        "intent id"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "SignalEngine_OutputHashDeterministic",
            &SignalEngine_OutputHashDeterministic
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

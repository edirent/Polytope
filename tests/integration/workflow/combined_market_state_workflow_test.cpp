#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void CombinedMarketStateWorkflow_VerifierPasses() {
    std::string command;

    if (std::filesystem::exists("./verify_market_state_workflow")) {
        command =
            "./verify_market_state_workflow "
            "--raw ../tests/fixtures/polymarket/market_39.raw "
            "--chain-fixture "
            "../tests/fixtures/chain_confirm/synthetic_order_filled.jsonl "
            "--repeat 2 --check-determinism";
    } else if (
        std::filesystem::exists("build/verify_market_state_workflow")) {
        command =
            "build/verify_market_state_workflow "
            "--raw tests/fixtures/polymarket/market_39.raw "
            "--chain-fixture "
            "tests/fixtures/chain_confirm/synthetic_order_filled.jsonl "
            "--repeat 2 --check-determinism";
    } else {
        fail("verify_market_state_workflow executable not found");
    }

    const int code = std::system(command.c_str());
    if (code != 0) {
        fail("verify_market_state_workflow returned non-zero exit code");
    }
}

}  // namespace

int main() {
    try {
        CombinedMarketStateWorkflow_VerifierPasses();
        std::cout << "CombinedMarketStateWorkflow_VerifierPasses passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CombinedMarketStateWorkflow_VerifierPasses failed: "
                  << error.what() << '\n';
        return 1;
    }
}

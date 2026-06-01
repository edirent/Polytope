#include "engine/paper/public/PaperModule.h"
#include "engine/paper/tools/PaperTools.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

int test_defaults_to_no_network() {
    const auto status = trading_engine::paper::paper_module_status();
    if (status.schema_version != 1) {
        return fail("unexpected paper schema version");
    }
    if (status.network_enabled) {
        return fail("paper skeleton must not enable network access");
    }
    if (status.mode != trading_engine::paper::PaperMode::Disabled) {
        return fail("paper skeleton must default to disabled mode");
    }
    return 0;
}

int test_public_contracts_compile() {
    if (!trading_engine::paper::public_contracts_available()) {
        return fail("paper public contract compile anchor failed");
    }
    return 0;
}

int test_tools_target_available() {
    if (std::string{trading_engine::paper::paper_tools_name()} != "paper_tools") {
        return fail("paper tools target returned unexpected name");
    }
    return 0;
}

int test_paper_event_is_observation_only() {
    using trading_engine::paper::PaperEvent;
    static_assert(std::is_trivially_copyable_v<PaperEvent>);
    static_assert(std::is_standard_layout_v<PaperEvent>);

    PaperEvent event;
    event.seq_no = 7;
    event.ts_ns = 42;
    event.type = trading_engine::paper::PaperEventType::ExecutionReportObserved;

    if (event.seq_no != 7 || event.ts_ns != 42) {
        return fail("paper event did not preserve observation metadata");
    }
    if (event.type != trading_engine::paper::PaperEventType::ExecutionReportObserved) {
        return fail("paper event did not preserve observation type");
    }
    return 0;
}

int test_paper_snapshot_defaults_safe() {
    trading_engine::paper::PaperSnapshot snapshot;
    if (snapshot.snapshot_id != 0 || snapshot.ts_ns != 0 || snapshot.source_seq_no != 0) {
        return fail("paper snapshot ids should default to zero");
    }
    if (snapshot.account.cash_balance_tick != 0 ||
        snapshot.pnl.mark_to_market_pnl_tick != 0 ||
        snapshot.performance.intents_observed != 0) {
        return fail("paper snapshot nested counters should default to zero");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "PaperModule_DefaultsToNoNetwork") {
        return test_defaults_to_no_network();
    }
    if (test_case == "PaperModule_PublicContractsCompile") {
        return test_public_contracts_compile();
    }
    if (test_case == "PaperTools_TargetAvailable") {
        return test_tools_target_available();
    }
    if (test_case == "PaperEvent_IsObservationOnly") {
        return test_paper_event_is_observation_only();
    }
    if (test_case == "PaperSnapshot_DefaultsSafe") {
        return test_paper_snapshot_defaults_safe();
    }

    return fail("unknown test case");
}

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open output: " + path.string());
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        fail("failed to write file: " + path.string());
    }
    output << text;
}

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

std::string base_command(
    const std::filesystem::path& out,
    const std::filesystem::path& output
) {
    return std::string{VERIFY_ORACLE_WORKFLOW_BIN} +
           " --market-snapshot " +
           source_path("tests/fixtures/oracle/raw_markets_small.jsonl").string() +
           " --rulebook " +
           source_path("tests/fixtures/oracle/rulebook_small.json").string() +
           " --candidate-bundles " +
           source_path("tests/fixtures/oracle/candidate_bundles_small.json").string() +
           " --out " + out.string() +
           " --check-determinism > " + output.string() + " 2>&1";
}

void OracleWorkflow_SmallFixtureCompiles() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_small_fixture";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto output = root / "workflow.out";

    const int rc = std::system(base_command(root / "artifact", output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("markets_loaded: 1") != std::string::npos, "markets");
    expect_true(text.find("assets_loaded: 2") != std::string::npos, "assets");
    expect_true(text.find("approved_rules: 1") != std::string::npos, "rules");
    expect_true(text.find("variables: 2") != std::string::npos, "variables");
    expect_true(text.find("constraints: 2") != std::string::npos, "constraints");
    expect_true(text.find("feasible_states: 2") != std::string::npos, "states");
    expect_true(text.find("manifest_ok: true") != std::string::npos, "manifest");
    expect_true(text.find("checksums_ok: true") != std::string::npos, "checksums");
}

void OracleWorkflow_LlmDisabledStillPasses() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_llm_disabled";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto output = root / "workflow.out";

    const int rc = std::system(base_command(root / "artifact", output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("enabled: false") != std::string::npos, "llm enabled");
    expect_true(text.find("provider: none") != std::string::npos, "llm provider");
    expect_true(
        text.find("outputs_used: false") != std::string::npos,
        "llm outputs"
    );
}

void OracleWorkflow_GeneratesCombinatorialBundlesFromRulebook() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_combinatorial_bundles";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto markets = root / "markets.jsonl";
    const auto rulebook = root / "rulebook.json";
    const auto output = root / "workflow.out";

    write_file(
        markets,
        R"({"market_id":"m1","event_id":"harvey","title":"Bracket 1","description":"Sentence bracket 1","outcomes":["Yes","No"],"asset_ids":["m1_yes","m1_no"],"resolution_source":"fixture","end_time":"2030-01-01T00:00:00Z","tags":["test"],"fetched_at_ns":1,"source":"fixture"}
{"market_id":"m2","event_id":"harvey","title":"Bracket 2","description":"Sentence bracket 2","outcomes":["Yes","No"],"asset_ids":["m2_yes","m2_no"],"resolution_source":"fixture","end_time":"2030-01-01T00:00:00Z","tags":["test"],"fetched_at_ns":1,"source":"fixture"}
{"market_id":"m3","event_id":"harvey","title":"Bracket 3","description":"Sentence bracket 3","outcomes":["Yes","No"],"asset_ids":["m3_yes","m3_no"],"resolution_source":"fixture","end_time":"2030-01-01T00:00:00Z","tags":["test"],"fetched_at_ns":1,"source":"fixture"}
)"
    );

    write_file(
        rulebook,
        R"({"rules":[{"rule_id":"r_harvey_exactly_one","type":"ExactlyOne","variable_ids":["m1:Yes","m2:Yes","m3:Yes"],"approved":true,"approved_by":"fixture","approved_at_ns":1,"source_rule_draft_id":"draft_harvey"}]})"
    );

    const std::string command =
        std::string{VERIFY_ORACLE_WORKFLOW_BIN} +
        " --market-snapshot " + markets.string() +
        " --rulebook " + rulebook.string() +
        " --generate-combinatorial-bundles" +
        " --out " + (root / "artifact").string() +
        " --check-determinism > " + output.string() + " 2>&1";

    const int rc = std::system(command.c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("markets_loaded: 3") != std::string::npos, "markets");
    expect_true(text.find("assets_loaded: 6") != std::string::npos, "assets");
    expect_true(text.find("approved_rules: 1") != std::string::npos, "rules");
    expect_true(
        text.find("candidate_bundles: 2") != std::string::npos,
        "candidate bundles"
    );
    expect_true(
        text.find("rejected_bundles: 0") != std::string::npos,
        "rejected bundles"
    );
    expect_true(
        text.find("determinism_passed: true") != std::string::npos,
        "determinism"
    );
}

void OracleWorkflow_LargeAtMostOneUsesComponentOracle() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_large_at_most_one";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto markets = root / "markets.jsonl";
    const auto rulebook = root / "rulebook.json";
    const auto output = root / "workflow.out";

    std::string market_text;
    std::string rule_text = R"({"rules":[{"rule_id":"r_large_at_most_one","type":"AtMostOne","variable_ids":[)";
    for (int i = 0; i < 40; ++i) {
        if (i != 0) {
            rule_text += ',';
        }
        rule_text += "\"m" + std::to_string(i) + ":Yes\"";

        market_text +=
            "{\"market_id\":\"m" + std::to_string(i) +
            "\",\"event_id\":\"world_cup\",\"title\":\"Team " +
            std::to_string(i) +
            "\",\"description\":\"At most one team wins.\",\"outcomes\":[\"Yes\",\"No\"],\"asset_ids\":[\"m" +
            std::to_string(i) + "_yes\",\"m" + std::to_string(i) +
            "_no\"],\"resolution_source\":\"fixture\",\"end_time\":\"2030-01-01T00:00:00Z\",\"tags\":[\"test\"],\"fetched_at_ns\":1,\"source\":\"fixture\"}\n";
    }
    rule_text += R"(],"approved":true,"approved_by":"fixture","approved_at_ns":1,"source_rule_draft_id":"draft_large"}]})";

    write_file(markets, market_text);
    write_file(rulebook, rule_text);

    const std::string command =
        std::string{VERIFY_ORACLE_WORKFLOW_BIN} +
        " --market-snapshot " + markets.string() +
        " --rulebook " + rulebook.string() +
        " --generate-combinatorial-bundles" +
        " --out " + (root / "artifact").string() +
        " --check-determinism > " + output.string() + " 2>&1";

    const int rc = std::system(command.c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(text.find("markets_loaded: 40") != std::string::npos, "markets");
    expect_true(text.find("assets_loaded: 80") != std::string::npos, "assets");
    expect_true(text.find("variables: 80") != std::string::npos, "variables");
    expect_true(
        text.find("max_component_variable_count: 80") != std::string::npos,
        "max component"
    );
    expect_true(
        text.find("at_most_one_components: 1") != std::string::npos,
        "at most one"
    );
    expect_true(
        text.find("skipped_full_enumeration_count: 1") != std::string::npos,
        "skipped enumeration"
    );
    expect_true(
        text.find("large_component_without_oracle_count: 0") != std::string::npos,
        "oracle backend"
    );
    expect_true(
        text.find("enumeration_mode: component_oracle") != std::string::npos,
        "component oracle mode"
    );
    expect_true(
        text.find("determinism_passed: true") != std::string::npos,
        "determinism"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OracleWorkflow_SmallFixtureCompiles",
            &OracleWorkflow_SmallFixtureCompiles
        },
        {
            "OracleWorkflow_LlmDisabledStillPasses",
            &OracleWorkflow_LlmDisabledStillPasses
        },
        {
            "OracleWorkflow_GeneratesCombinatorialBundlesFromRulebook",
            &OracleWorkflow_GeneratesCombinatorialBundlesFromRulebook
        },
        {
            "OracleWorkflow_LargeAtMostOneUsesComponentOracle",
            &OracleWorkflow_LargeAtMostOneUsesComponentOracle
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

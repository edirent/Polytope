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

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
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

void write_contradictory_rulebook(const std::filesystem::path& path) {
    std::ofstream output(path);
    if (!output) {
        fail("failed to write rulebook");
    }

    output
        << "{\n"
        << "  \"rules\": [\n"
        << "    {\"rule_id\":\"r_exactly_one\",\"type\":\"ExactlyOne\","
        << "\"variable_ids\":[\"m1:YES\",\"m1:NO\"],\"approved\":true,"
        << "\"approved_by\":\"fixture\",\"approved_at_ns\":1,"
        << "\"source_rule_draft_id\":\"draft_1\"},\n"
        << "    {\"rule_id\":\"r_yes_implies_no\",\"type\":\"Implies\","
        << "\"variable_ids\":[\"m1:YES\",\"m1:NO\"],\"approved\":true,"
        << "\"approved_by\":\"fixture\",\"approved_at_ns\":1,"
        << "\"source_rule_draft_id\":\"draft_2\"},\n"
        << "    {\"rule_id\":\"r_no_implies_yes\",\"type\":\"Implies\","
        << "\"variable_ids\":[\"m1:NO\",\"m1:YES\"],\"approved\":true,"
        << "\"approved_by\":\"fixture\",\"approved_at_ns\":1,"
        << "\"source_rule_draft_id\":\"draft_3\"}\n"
        << "  ]\n"
        << "}\n";
}

void OracleWorkflow_ContradictionFails() {
    const auto root =
        std::filesystem::temp_directory_path() /
        "oracle_workflow_contradiction";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto rulebook = root / "contradictory_rulebook.json";
    const auto output = root / "workflow.out";
    write_contradictory_rulebook(rulebook);

    const std::string command =
        std::string{VERIFY_ORACLE_WORKFLOW_BIN} +
        " --market-snapshot " +
        source_path("tests/fixtures/oracle/raw_markets_small.jsonl").string() +
        " --rulebook " + rulebook.string() +
        " --candidate-bundles " +
        source_path("tests/fixtures/oracle/candidate_bundles_small.json").string() +
        " --out " + (root / "artifact").string() +
        " --check-determinism > " + output.string() + " 2>&1";

    const int rc = std::system(command.c_str());
    expect_true(rc != 0, "workflow should fail");

    const auto text = read_file(output);
    expect_true(
        text.find("contradictions: 1") != std::string::npos,
        "contradiction count"
    );
    expect_true(
        text.find("feasible_states: 0") != std::string::npos,
        "feasible states"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OracleWorkflow_ContradictionFails",
            &OracleWorkflow_ContradictionFails
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

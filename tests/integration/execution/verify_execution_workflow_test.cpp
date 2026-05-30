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

void expect_true(bool value, const std::string& label) {
    if (!value) {
        fail("expected true: " + label);
    }
}

std::filesystem::path source_path(const std::string& relative) {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} / relative;
}

std::string shell_quote(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

std::filesystem::path temp_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("verify_execution_workflow_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open " + path.string());
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

void expect_contains(
    const std::string& text,
    const std::string& needle
) {
    expect_true(text.find(needle) != std::string::npos, needle);
}

std::string command(
    const std::filesystem::path& output,
    const std::filesystem::path& approved_intents,
    const std::filesystem::path& source_intents,
    const std::filesystem::path& config
) {
    return std::string{VERIFY_EXECUTION_WORKFLOW_BIN} +
           " --approved-intents " + shell_quote(approved_intents) +
           " --source-intents " + shell_quote(source_intents) +
           " --snapshots " +
           shell_quote(source_path(
               "tests/fixtures/execution/market_state_snapshots_execution.json"
           )) +
           " --config " + shell_quote(config) +
           " --check-determinism > " + shell_quote(output) + " 2>&1";
}

std::string positive_command(const std::filesystem::path& output) {
    return command(
        output,
        source_path("tests/fixtures/execution/approved_intents_positive.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_positive.jsonl"),
        source_path("tests/fixtures/execution/execution_config_paper.json")
    );
}

std::string extract_value(
    const std::string& text,
    const std::string& key
) {
    const auto pos = text.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    auto start = pos + key.size();
    while (start < text.size() && text[start] == ' ') {
        ++start;
    }
    auto end = text.find('\n', start);
    if (end == std::string::npos) {
        end = text.size();
    }
    return text.substr(start, end - start);
}

void VerifyExecutionWorkflow_PositiveFillsPlan() {
    const auto root = temp_root("positive");
    const auto output = root / "execution.out";

    const int rc = std::system(positive_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "approved_intents_loaded: 1");
    expect_contains(text, "plans_created: 1");
    expect_contains(text, "child_orders_created: 1");
    expect_contains(text, "plans_filled: 1");
    expect_contains(text, "child_orders_filled: 1");
    expect_contains(text, "total_filled_qty_lots: 10");
    expect_contains(text, "total_filled_cost_tick: 8000");
    expect_contains(text, "execution_reports_published: 1");
    expect_contains(text, "reservation_consumed: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_InsufficientDepthFailsAndReleases() {
    const auto root = temp_root("insufficient");
    const auto output = root / "execution.out";

    const int rc = std::system(command(
        output,
        source_path("tests/fixtures/execution/approved_intents_insufficient_depth.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_insufficient_depth.jsonl"),
        source_path("tests/fixtures/execution/execution_config_paper.json")
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "plans_failed: 1");
    expect_contains(text, "child_orders_filled: 0");
    expect_contains(text, "reservation_released: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_ExpiredIntentDoesNotSubmit() {
    const auto root = temp_root("expired");
    const auto output = root / "execution.out";

    const int rc = std::system(command(
        output,
        source_path("tests/fixtures/execution/approved_intents_expired.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_expired.jsonl"),
        source_path("tests/fixtures/execution/execution_config_paper.json")
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "plans_created: 0");
    expect_contains(text, "plans_submitted: 0");
    expect_contains(text, "plans_expired: 1");
    expect_contains(text, "reservation_expired: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_DuplicateIdempotencyRejected() {
    const auto root = temp_root("duplicate");
    const auto output = root / "execution.out";

    const int rc = std::system(command(
        output,
        source_path("tests/fixtures/execution/approved_intents_duplicate.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_duplicate.jsonl"),
        source_path("tests/fixtures/execution/execution_config_paper.json")
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "approved_intents_loaded: 2");
    expect_contains(text, "plans_created: 1");
    expect_contains(text, "plans_failed: 1");
    expect_contains(text, "plans_filled: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_PartialSequentialRequiresHedge() {
    const auto root = temp_root("partial");
    const auto output = root / "execution.out";

    const int rc = std::system(command(
        output,
        source_path("tests/fixtures/execution/approved_intents_partial.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_partial.jsonl"),
        source_path("tests/fixtures/execution/execution_config_paper_sequential.json")
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "plans_hedge_required: 1");
    expect_contains(text, "child_orders_filled: 1");
    expect_contains(text, "child_orders_partially_filled: 1");
    expect_contains(text, "reservation_hedge_required: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_LiveAdapterDisabledDoesNotFill() {
    const auto root = temp_root("live_disabled");
    const auto output = root / "execution.out";

    const int rc = std::system(command(
        output,
        source_path("tests/fixtures/execution/approved_intents_positive.jsonl"),
        source_path("tests/fixtures/execution/opportunity_intents_positive.jsonl"),
        source_path("tests/fixtures/execution/execution_config_live_disabled.json")
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "mode: Live");
    expect_contains(text, "plans_failed: 1");
    expect_contains(text, "child_orders_filled: 0");
    expect_contains(text, "reservation_released: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyExecutionWorkflow_DeterministicHash() {
    const auto root = temp_root("deterministic");
    const auto output_a = root / "execution_a.out";
    const auto output_b = root / "execution_b.out";

    const int rc_a = std::system(positive_command(output_a).c_str());
    const int rc_b = std::system(positive_command(output_b).c_str());
    expect_true(rc_a == 0, "workflow a exit");
    expect_true(rc_b == 0, "workflow b exit");

    const auto text_a = read_file(output_a);
    const auto text_b = read_file(output_b);
    expect_true(
        extract_value(text_a, "execution_output_hash:") ==
            extract_value(text_b, "execution_output_hash:"),
        "stable execution output hash"
    );
}

void VerifyExecutionWorkflow_EmitsSummary() {
    const auto root = temp_root("summary");
    const auto output = root / "execution.out";

    const int rc = std::system(positive_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    for (const std::string section : {
             "execution_workflow:",
             "adapter:",
             "fills:",
             "reports:",
             "metrics:",
             "hashes:"
         }) {
        expect_contains(text, section);
    }
    for (const std::string field : {
             "approved_intents_loaded:",
             "plans_created:",
             "child_orders_created:",
             "mode:",
             "plans_submitted:",
             "plans_filled:",
             "plans_failed:",
             "plans_expired:",
             "plans_hedge_required:",
             "child_orders_filled:",
             "child_orders_partially_filled:",
             "total_filled_qty_lots:",
             "total_filled_cost_tick:",
             "execution_reports_published:",
             "reservation_consumed:",
             "reservation_released:",
             "reservation_expired:",
             "reservation_hedge_required:",
             "execution.plan.created:",
             "execution.plan.submitted:",
             "execution.plan.filled:",
             "execution.plan.failed:",
             "execution.plan.expired:",
             "execution.plan.hedge_required:",
             "execution.child.created:",
             "execution.child.filled:",
             "execution.child.partial:",
             "execution.child.cancelled:",
             "execution.child.failed:",
             "execution.report.published:",
             "execution.reservation.consume:",
             "execution.reservation.release:",
             "execution.reservation.expire:",
             "execution.reservation.hedge_required:",
             "execution.latency.submit_ns:",
             "execution.latency.fill_simulation_ns:",
             "execution_output_hash:",
             "determinism_passed:"
         }) {
        expect_contains(text, field);
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "VerifyExecutionWorkflow_PositiveFillsPlan",
            &VerifyExecutionWorkflow_PositiveFillsPlan
        },
        {
            "VerifyExecutionWorkflow_InsufficientDepthFailsAndReleases",
            &VerifyExecutionWorkflow_InsufficientDepthFailsAndReleases
        },
        {
            "VerifyExecutionWorkflow_ExpiredIntentDoesNotSubmit",
            &VerifyExecutionWorkflow_ExpiredIntentDoesNotSubmit
        },
        {
            "VerifyExecutionWorkflow_DuplicateIdempotencyRejected",
            &VerifyExecutionWorkflow_DuplicateIdempotencyRejected
        },
        {
            "VerifyExecutionWorkflow_PartialSequentialRequiresHedge",
            &VerifyExecutionWorkflow_PartialSequentialRequiresHedge
        },
        {
            "VerifyExecutionWorkflow_LiveAdapterDisabledDoesNotFill",
            &VerifyExecutionWorkflow_LiveAdapterDisabledDoesNotFill
        },
        {
            "VerifyExecutionWorkflow_DeterministicHash",
            &VerifyExecutionWorkflow_DeterministicHash
        },
        {
            "VerifyExecutionWorkflow_EmitsSummary",
            &VerifyExecutionWorkflow_EmitsSummary
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
        std::cerr << "usage: verify_execution_workflow_tests <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
        ("verify_paper_trading_workflow_" + name);
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

std::string command(const std::filesystem::path& output) {
    return std::string{VERIFY_PAPER_TRADING_WORKFLOW_BIN} +
           " --execution-reports " +
           shell_quote(source_path(
               "tests/fixtures/paper/execution_reports_positive.jsonl"
           )) +
           " --reservation-dispositions " +
           shell_quote(source_path(
               "tests/fixtures/paper/reservation_dispositions.jsonl"
           )) +
           " --snapshots " +
           shell_quote(source_path(
               "tests/fixtures/paper/market_state_snapshots_mark.json"
           )) +
           " --starting-cash 1000000000 --check-determinism > " +
           shell_quote(output) + " 2>&1";
}

void VerifyPaperTradingWorkflow_PositiveFixtureRuns() {
    const auto root = temp_root("positive");
    const auto output = root / "paper.out";

    const int rc = std::system(command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "reports_loaded: 2");
    expect_contains(text, "fills_applied: 1");
    expect_contains(text, "duplicate_reports_ignored: 1");
    expect_contains(text, "starting_cash: 1000000000");
    expect_contains(text, "ending_cash: 999992950");
    expect_contains(text, "unrealized_pnl_mid: 2000");
    expect_contains(text, "unrealized_pnl_liquidation: 1000");
    expect_contains(text, "equity_mid: 1000001950");
    expect_contains(text, "equity_liquidation: 1000000950");
    expect_contains(text, "determinism_passed: true");
}

void VerifyPaperTradingWorkflow_DeterministicHashes() {
    const auto root = temp_root("deterministic");
    const auto output_a = root / "paper_a.out";
    const auto output_b = root / "paper_b.out";

    const int rc_a = std::system(command(output_a).c_str());
    const int rc_b = std::system(command(output_b).c_str());
    expect_true(rc_a == 0, "workflow a exit");
    expect_true(rc_b == 0, "workflow b exit");

    const auto text_a = read_file(output_a);
    const auto text_b = read_file(output_b);
    expect_true(
        extract_value(text_a, "paper_ledger_hash:") ==
            extract_value(text_b, "paper_ledger_hash:"),
        "stable paper ledger hash"
    );
    expect_true(
        extract_value(text_a, "equity_curve_hash:") ==
            extract_value(text_b, "equity_curve_hash:"),
        "stable equity curve hash"
    );
    expect_true(
        extract_value(text_a, "dashboard_snapshot_hash:") ==
            extract_value(text_b, "dashboard_snapshot_hash:"),
        "stable dashboard hash"
    );
}

void VerifyPaperTradingWorkflow_EmitsSummary() {
    const auto root = temp_root("summary");
    const auto output = root / "paper.out";

    const int rc = std::system(command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    for (const std::string section : {
             "paper_workflow:",
             "account:",
             "performance:",
             "dashboard:",
             "hashes:"
         }) {
        expect_contains(text, section);
    }
    expect_contains(text, "sharpe_status:");
    expect_contains(text, "snapshots_published:");
    expect_contains(text, "determinism_passed: true");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            fail("expected one test case");
        }
        const std::string test_case{argv[1]};
        if (test_case == "VerifyPaperTradingWorkflow_PositiveFixtureRuns") {
            VerifyPaperTradingWorkflow_PositiveFixtureRuns();
        } else if (
            test_case == "VerifyPaperTradingWorkflow_DeterministicHashes"
        ) {
            VerifyPaperTradingWorkflow_DeterministicHashes();
        } else if (test_case == "VerifyPaperTradingWorkflow_EmitsSummary") {
            VerifyPaperTradingWorkflow_EmitsSummary();
        } else {
            fail("unknown test case: " + test_case);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}

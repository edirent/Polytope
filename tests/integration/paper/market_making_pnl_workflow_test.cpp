#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::filesystem::path temp_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("market_making_pnl_workflow_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
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

std::string command(
    const std::filesystem::path& approved_quotes,
    const std::filesystem::path& market_events,
    const std::filesystem::path& output,
    const std::string& fill_mode
) {
    return std::string{VERIFY_MARKET_MAKING_PNL_WORKFLOW_BIN} +
           " --approved-quotes " + shell_quote(approved_quotes) +
           " --market-events " + shell_quote(market_events) +
           " --snapshots " +
           shell_quote(source_path(
               "tests/fixtures/market_making/mark_snapshots.jsonl"
           )) +
           " --starting-cash 100000000000 --fill-mode " + fill_mode +
           " --check-determinism > " + shell_quote(output) + " 2>&1";
}

void MakerRoundTrip_ComputesExpectedRealizedPnl() {
    const auto root = temp_root("roundtrip");
    const auto output = root / "roundtrip.out";
    const int rc = std::system(command(
        source_path("tests/fixtures/market_making/approved_quotes_roundtrip.jsonl"),
        source_path("tests/fixtures/market_making/maker_trade_through_events.jsonl"),
        output,
        "conservative"
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "maker_fills: 2");
    expect_contains(text, "duplicate_reports_ignored: 1");
    expect_contains(text, "ending_cash: 100004000000");
    expect_contains(text, "realized_pnl: 4000000");
    expect_contains(text, "open_position_qty: 0");
    expect_contains(text, "unrealized_pnl_mid: 0");
    expect_contains(text, "determinism_passed: true");
}

void MarketMakingPnlWorkflow_Deterministic() {
    const auto root = temp_root("deterministic");
    const auto output_a = root / "roundtrip_a.out";
    const auto output_b = root / "roundtrip_b.out";
    const auto approved =
        source_path("tests/fixtures/market_making/approved_quotes_roundtrip.jsonl");
    const auto events =
        source_path("tests/fixtures/market_making/maker_trade_through_events.jsonl");

    expect_true(
        std::system(command(approved, events, output_a, "conservative").c_str()) == 0,
        "workflow a exit"
    );
    expect_true(
        std::system(command(approved, events, output_b, "conservative").c_str()) == 0,
        "workflow b exit"
    );

    const auto text_a = read_file(output_a);
    const auto text_b = read_file(output_b);
    expect_true(
        extract_value(text_a, "maker_ledger_hash:") ==
            extract_value(text_b, "maker_ledger_hash:"),
        "stable ledger hash"
    );
    expect_true(
        extract_value(text_a, "maker_pnl_hash:") ==
            extract_value(text_b, "maker_pnl_hash:"),
        "stable pnl hash"
    );
}

void MarketMakingPnlWorkflow_NoFillProducesZeroPnl() {
    const auto root = temp_root("nofill");
    const auto output = root / "nofill.out";
    const int rc = std::system(command(
        source_path("tests/fixtures/market_making/approved_quotes_nofill.jsonl"),
        source_path("tests/fixtures/market_making/maker_no_trade_events.jsonl"),
        output,
        "nofill"
    ).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "maker_fills: 0");
    expect_contains(text, "ending_cash: 100000000000");
    expect_contains(text, "realized_pnl: 0");
    expect_contains(text, "unrealized_pnl_mid: 0");
    expect_contains(text, "determinism_passed: true");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            fail("expected one test case");
        }
        const std::string test_case{argv[1]};
        if (test_case == "MakerRoundTrip_ComputesExpectedRealizedPnl") {
            MakerRoundTrip_ComputesExpectedRealizedPnl();
        } else if (
            test_case == "MarketMakingPnlWorkflow_Deterministic"
        ) {
            MarketMakingPnlWorkflow_Deterministic();
        } else if (
            test_case == "MarketMakingPnlWorkflow_NoFillProducesZeroPnl"
        ) {
            MarketMakingPnlWorkflow_NoFillProducesZeroPnl();
        } else {
            fail("unknown test case: " + test_case);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}

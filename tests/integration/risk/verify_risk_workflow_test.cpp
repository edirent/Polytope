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
        ("verify_risk_workflow_" + name);
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

std::string command(
    const std::filesystem::path& output,
    const std::filesystem::path& intents,
    const std::filesystem::path& config
) {
    return std::string{VERIFY_RISK_WORKFLOW_BIN} +
           " --intent-fixture " + shell_quote(intents) +
           " --snapshot-fixture " +
           shell_quote(source_path("tests/fixtures/risk/market_state_snapshots_risk.json")) +
           " --risk-config " + shell_quote(config) +
           " --check-determinism > " + shell_quote(output) + " 2>&1";
}

std::string positive_command(const std::filesystem::path& output) {
    return command(
        output,
        source_path("tests/fixtures/risk/opportunity_intents_positive.jsonl"),
        source_path("tests/fixtures/risk/risk_config_small.json")
    );
}

std::string rejections_command(
    const std::filesystem::path& output,
    const std::filesystem::path& config
) {
    return command(
        output,
        source_path("tests/fixtures/risk/opportunity_intents_rejections.jsonl"),
        config
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

void expect_contains(
    const std::string& text,
    const std::string& needle
) {
    expect_true(text.find(needle) != std::string::npos, needle);
}

void VerifyRiskWorkflow_PositiveApproves() {
    const auto root = temp_root("positive");
    const auto output = root / "risk.out";

    const int rc = std::system(positive_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "intents_loaded: 1");
    expect_contains(text, "intents_evaluated: 1");
    expect_contains(text, "approved: 1");
    expect_contains(text, "reservations_created: 1");
    expect_contains(text, "risk.evaluate.count: 1");
    expect_contains(text, "risk.approve.count: 1");
    expect_contains(text, "risk.reservation.created: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyRiskWorkflow_RejectionsCoverRequiredReasons() {
    const auto root = temp_root("rejections");
    const auto output = root / "risk.out";

    const int rc = std::system(
        rejections_command(
            output,
            source_path("tests/fixtures/risk/risk_config_small.json")
        ).c_str()
    );
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "rejected_expired: 1");
    expect_contains(text, "rejected_duplicate: 1");
    expect_contains(text, "rejected_stale_book: 1");
    expect_contains(text, "rejected_insufficient_depth: 1");
    expect_contains(text, "rejected_cost_drift: 1");
    expect_contains(text, "rejected_partial_fill_risk: 1");
    expect_contains(text, "risk.reject.expired: 1");
    expect_contains(text, "risk.reject.duplicate: 1");
    expect_contains(text, "risk.reject.stale_book: 1");
    expect_contains(text, "risk.reject.insufficient_depth: 1");
    expect_contains(text, "risk.reject.cost_drift: 1");
    expect_contains(text, "risk.reject.partial_fill: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyRiskWorkflow_KillSwitchRejects() {
    const auto root = temp_root("kill_switch");
    const auto output = root / "risk.out";

    const int rc = std::system(
        command(
            output,
            source_path("tests/fixtures/risk/opportunity_intents_positive.jsonl"),
            source_path("tests/fixtures/risk/risk_config_kill_switch.json")
        ).c_str()
    );
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "approved: 0");
    expect_contains(text, "rejected_kill_switch: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyRiskWorkflow_TightExposureRejects() {
    const auto root = temp_root("tight_exposure");
    const auto output = root / "risk.out";

    const int rc = std::system(
        rejections_command(
            output,
            source_path("tests/fixtures/risk/risk_config_tight_exposure.json")
        ).c_str()
    );
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_contains(text, "rejected_exposure_limit: 1");
    expect_contains(text, "determinism_passed: true");
}

void VerifyRiskWorkflow_Deterministic() {
    const auto root = temp_root("deterministic");
    const auto output_a = root / "risk_a.out";
    const auto output_b = root / "risk_b.out";

    const int rc_a = std::system(positive_command(output_a).c_str());
    const int rc_b = std::system(positive_command(output_b).c_str());
    expect_true(rc_a == 0, "workflow a exit");
    expect_true(rc_b == 0, "workflow b exit");

    const auto text_a = read_file(output_a);
    const auto text_b = read_file(output_b);
    expect_true(
        extract_value(text_a, "risk_output_hash:") ==
            extract_value(text_b, "risk_output_hash:"),
        "stable risk output hash"
    );
    expect_contains(text_a, "determinism_passed: true");
    expect_contains(text_b, "determinism_passed: true");
}

void VerifyRiskWorkflow_EmitsSummary() {
    const auto root = temp_root("summary");
    const auto output = root / "risk.out";

    const int rc = std::system(positive_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    for (const std::string section : {
             "risk_workflow:",
             "decisions:",
             "repricing:",
             "risk_vwap_mode:",
             "reservation:",
             "metrics:",
             "hashes:"
         }) {
        expect_contains(text, section);
    }
    for (const std::string field : {
             "rejected_invalid_intent:",
             "rejected_expired:",
             "rejected_duplicate:",
             "rejected_kill_switch:",
             "rejected_stale_book:",
             "rejected_insufficient_depth:",
             "rejected_cost_drift:",
             "rejected_low_edge:",
             "rejected_exposure_limit:",
             "rejected_inventory_limit:",
             "rejected_partial_fill_risk:",
             "rejected_max_loss:",
             "rejected_rate_limited:",
             "vwap_reused_signal_cost:",
             "vwap_recomputed:",
             "cost_drift_min:",
             "cost_drift_max:",
             "reservations_created:",
             "reservations_released:",
             "reservations_expired:",
             "risk.evaluate.count:",
             "risk.approve.count:",
             "risk.reject.count:",
             "risk.reject.invalid_intent:",
             "risk.reject.expired:",
             "risk.reject.duplicate:",
             "risk.reject.kill_switch:",
             "risk.reject.stale_book:",
             "risk.reject.insufficient_depth:",
             "risk.reject.cost_drift:",
             "risk.reject.low_edge:",
             "risk.reject.exposure:",
             "risk.reject.inventory:",
             "risk.reject.partial_fill:",
             "risk.reject.max_loss:",
             "risk.reject.rate_limited:",
             "risk.reservation.created:",
             "risk.reservation.expired:",
             "risk.reservation.released:",
             "risk.vwap.reused_signal_cost:",
             "risk.vwap.recomputed:",
             "risk.latency.evaluate_ns:",
             "risk_output_hash:",
             "determinism_passed:"
         }) {
        expect_contains(text, field);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: verify_risk_workflow_tests <test_name>\n";
        return 2;
    }

    const std::string test = argv[1];

    try {
        if (test == "VerifyRiskWorkflow_PositiveApproves") {
            VerifyRiskWorkflow_PositiveApproves();
        } else if (test == "VerifyRiskWorkflow_RejectionsCoverRequiredReasons") {
            VerifyRiskWorkflow_RejectionsCoverRequiredReasons();
        } else if (test == "VerifyRiskWorkflow_KillSwitchRejects") {
            VerifyRiskWorkflow_KillSwitchRejects();
        } else if (test == "VerifyRiskWorkflow_TightExposureRejects") {
            VerifyRiskWorkflow_TightExposureRejects();
        } else if (test == "VerifyRiskWorkflow_Deterministic") {
            VerifyRiskWorkflow_Deterministic();
        } else if (test == "VerifyRiskWorkflow_EmitsSummary") {
            VerifyRiskWorkflow_EmitsSummary();
        } else {
            std::cerr << "unknown test: " << test << "\n";
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }

    return 0;
}

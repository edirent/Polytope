#include <boost/json.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace json = boost::json;

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
        fail("failed to open output: " + path.string());
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

json::value parse_json(
    const std::string& text,
    const std::string& label
) {
    boost::json::error_code error;
    auto parsed = json::parse(text, error);
    if (error) {
        fail("failed to parse " + label + ": " + error.message());
    }
    return parsed;
}

bool json_number_gt_zero(const json::value& value) {
    if (value.is_uint64()) {
        return value.as_uint64() > 0;
    }
    if (value.is_int64()) {
        return value.as_int64() > 0;
    }
    if (value.is_double()) {
        return value.as_double() > 0.0;
    }
    return false;
}

std::string base_command(
    const std::filesystem::path& output,
    const std::filesystem::path& state_snapshot,
    const std::filesystem::path& oracle_artifact,
    const std::string& extra_args = {}
) {
    return std::string{VERIFY_SIGNAL_WORKFLOW_BIN} +
           " --state-snapshot " +
           shell_quote(state_snapshot) +
           " --oracle-artifact " +
           shell_quote(oracle_artifact) +
           " --check-determinism " +
           extra_args +
           " > " + shell_quote(output) + " 2>&1";
}

std::string small_fixture_command(
    const std::filesystem::path& output,
    const std::string& extra_args = {}
) {
    return base_command(
        output,
        source_path("tests/fixtures/signal/market_state_snapshots_small.json"),
        source_path("tests/fixtures/oracle/artifact_small"),
        extra_args
    );
}

std::string positive_fixture_command(
    const std::filesystem::path& output,
    const std::string& extra_args = {}
) {
    return base_command(
        output,
        source_path("tests/fixtures/signal/market_state_snapshots_positive.json"),
        source_path("tests/fixtures/oracle/artifact_positive"),
        extra_args
    );
}

std::filesystem::path temp_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("verify_signal_workflow_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
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

void VerifySignalWorkflow_SmallFixtureRuns() {
    const auto root = temp_root("small_fixture");
    const auto output = root / "workflow.out";

    const int rc = std::system(small_fixture_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(
        text.find("candidate_bundles_loaded: 1") != std::string::npos,
        "bundles loaded"
    );
    expect_true(
        text.find("candidate_bundles_scanned: 1") != std::string::npos,
        "bundles scanned"
    );
    expect_true(
        text.find("intents_published: 1") != std::string::npos,
        "intents published"
    );
}

void VerifySignalWorkflow_Deterministic() {
    const auto root = temp_root("deterministic");
    const auto output_a = root / "workflow_a.out";
    const auto output_b = root / "workflow_b.out";

    const int rc_a = std::system(small_fixture_command(output_a).c_str());
    const int rc_b = std::system(small_fixture_command(output_b).c_str());
    expect_true(rc_a == 0, "workflow a exit");
    expect_true(rc_b == 0, "workflow b exit");

    const auto text_a = read_file(output_a);
    const auto text_b = read_file(output_b);
    expect_true(
        text_a.find("determinism_passed: true") != std::string::npos,
        "determinism a"
    );
    expect_true(
        text_b.find("determinism_passed: true") != std::string::npos,
        "determinism b"
    );
    expect_true(
        extract_value(text_a, "signal_output_hash:") ==
            extract_value(text_b, "signal_output_hash:"),
        "stable output hash"
    );
}

void VerifySignalWorkflow_EmitsSummary() {
    const auto root = temp_root("summary");
    const auto output = root / "workflow.out";

    const int rc = std::system(small_fixture_command(output).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    for (const std::string section : {
             "signal_workflow:",
             "quality_gate:",
             "snapshot:",
             "settlement:",
             "vwap:",
             "edge:",
             "intents:",
             "intent_lifecycle:",
             "metrics:",
             "hashes:"
         }) {
        expect_true(text.find(section) != std::string::npos, section);
    }
    for (const std::string field : {
             "consistency_checked:",
             "rejected_snapshot_skew:",
             "rejected_stale_snapshot:",
             "bundle_qty_min:",
             "bundle_qty_max:",
             "unit_edge_min:",
             "unit_edge_max:",
             "total_edge_min:",
             "total_edge_max:",
             "edge_bps_min:",
             "edge_bps_max:",
             "intents_with_expiry:",
             "intents_with_idempotency_key:",
             "duplicate_rejected:",
             "rate_limited:",
             "signal.scan.count:",
             "signal.bundle.scanned:",
             "signal.bundle.rejected:",
             "signal.bundle.passed:",
             "signal.reject.settled:",
             "signal.reject.missing_snapshot:",
             "signal.reject.stale_lob:",
             "signal.reject.snapshot_skew:",
             "signal.reject.insufficient_depth:",
             "signal.reject.edge_below_threshold:",
             "signal.reject.duplicate:",
             "signal.reject.rate_limited:",
             "signal.intent.published:",
             "signal.scan.latency_ns:"
         }) {
        expect_true(text.find(field) != std::string::npos, field);
    }
}

void VerifySignalWorkflow_DoesNotRequireNetwork() {
    const auto root = temp_root("no_network");
    const auto output = root / "workflow.out";
    const auto intents = root / "signal_intents.jsonl";
    const std::string extra =
        " --emit-rejections true --out " + shell_quote(intents);

    const int rc = std::system(small_fixture_command(output, extra).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(
        text.find("determinism_passed: true") != std::string::npos,
        "determinism"
    );
    expect_true(std::filesystem::exists(intents), "intents file exists");
    expect_true(!read_file(intents).empty(), "intents file non-empty");
}

void VerifySignalWorkflow_PositiveFixtureEmitsPaperOpportunity() {
    const auto root = temp_root("positive");
    const auto output = root / "workflow.out";
    const auto intents = root / "signal_intents.jsonl";
    const std::string extra =
        " --emit-rejections true --out " + shell_quote(intents);

    const int rc = std::system(positive_fixture_command(output, extra).c_str());
    expect_true(rc == 0, "workflow exit");

    const auto text = read_file(output);
    expect_true(
        text.find("candidate_bundles_loaded: 1") != std::string::npos,
        "bundles loaded"
    );
    expect_true(
        text.find("candidate_bundles_scanned: 1") != std::string::npos,
        "bundles scanned"
    );
    expect_true(
        text.find("paper_opportunities: 1") != std::string::npos,
        "paper opportunity"
    );
    expect_true(
        text.find("bundle_qty_min: 20") != std::string::npos,
        "bundle qty min"
    );
    expect_true(
        text.find("bundle_qty_max: 20") != std::string::npos,
        "bundle qty max"
    );
    expect_true(
        text.find("total_edge_max: 3800000") != std::string::npos,
        "total edge max"
    );
    expect_true(
        text.find("intents_with_expiry: 1") != std::string::npos,
        "expiry count"
    );
    expect_true(
        text.find("intents_with_idempotency_key: 1") != std::string::npos,
        "idempotency count"
    );
    expect_true(
        text.find("rejected_insufficient_depth: 0") != std::string::npos,
        "insufficient depth"
    );
    expect_true(
        text.find("intents_published: 1") != std::string::npos,
        "intents published"
    );
    expect_true(
        text.find("signal.bundle.passed: 1") != std::string::npos,
        "metrics bundle passed"
    );
    expect_true(
        text.find("signal.intent.published: 1") != std::string::npos,
        "metrics intent published"
    );
    expect_true(
        text.find("determinism_passed: true") != std::string::npos,
        "determinism"
    );

    const auto intent_jsonl = read_file(intents);
    const auto actual_intent = parse_json(intent_jsonl, "positive intent jsonl");
    const auto expected = parse_json(
        read_file(source_path("tests/fixtures/signal/expected_intents_positive.json")),
        "expected positive intents"
    );
    const auto& expected_intents =
        expected.as_object().at("intents").as_array();
    expect_true(expected_intents.size() == 1, "expected one intent");
    expect_true(actual_intent == expected_intents[0], "expected intent match");

    const auto& actual_object = actual_intent.as_object();
    expect_true(
        json_number_gt_zero(actual_object.at("snapshot_version")),
        "snapshot version present"
    );
    expect_true(
        json_number_gt_zero(actual_object.at("oracle_artifact_hash")),
        "artifact hash present"
    );
    expect_true(
        json_number_gt_zero(actual_object.at("bundle_hash")),
        "bundle hash present"
    );
    expect_true(
        json_number_gt_zero(actual_object.at("bundle_qty")),
        "bundle qty present"
    );
    expect_true(
        json_number_gt_zero(actual_object.at("expires_at_ns")),
        "expires at present"
    );
    expect_true(
        !actual_object.at("idempotency_key").as_string().empty(),
        "idempotency key present"
    );
    expect_true(
        intent_jsonl.find("\"status\":\"PaperOpportunity\"") !=
            std::string::npos,
        "paper status"
    );
    expect_true(
        intent_jsonl.find("\"enough_depth\":true") != std::string::npos,
        "enough depth"
    );
    expect_true(
        intent_jsonl.find("\"bundle_qty\":20") != std::string::npos,
        "bundle qty"
    );
    expect_true(
        intent_jsonl.find("\"estimated_edge_tick\":3800000") !=
            std::string::npos,
        "positive edge"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "VerifySignalWorkflow_SmallFixtureRuns",
            &VerifySignalWorkflow_SmallFixtureRuns
        },
        {
            "VerifySignalWorkflow_Deterministic",
            &VerifySignalWorkflow_Deterministic
        },
        {
            "VerifySignalWorkflow_EmitsSummary",
            &VerifySignalWorkflow_EmitsSummary
        },
        {
            "VerifySignalWorkflow_DoesNotRequireNetwork",
            &VerifySignalWorkflow_DoesNotRequireNetwork
        },
        {
            "VerifySignalWorkflow_PositiveFixtureEmitsPaperOpportunity",
            &VerifySignalWorkflow_PositiveFixtureEmitsPaperOpportunity
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

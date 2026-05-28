#include "oracle/artifact/ArtifactChecksum.h"
#include "oracle/artifact/ArtifactExporter.h"
#include "oracle/artifact/ArtifactLoader.h"
#include "oracle/bundles/BundleHash.h"
#include "oracle/bundles/CandidateBundleGenerator.h"
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/enumerate/StateEnumerator.h"
#include "oracle/ingestion/MarketDescriptionLoader.h"
#include "oracle/ingestion/MarketUniverseBuilder.h"
#include "oracle/payoff/PayoffMatrixBuilder.h"
#include "oracle/rules/RuleValidator.h"
#include "oracle/rules/Rulebook.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::oracle {
namespace {

namespace json = boost::json;

struct Options {
    std::string market_snapshot_path;
    std::string rulebook_path;
    std::string candidate_bundles_path;
    std::filesystem::path out_path;
    bool check_determinism = false;
};

struct WorkflowSummary {
    std::uint32_t markets_loaded = 0;
    std::uint32_t assets_loaded = 0;
    std::uint32_t missing_fields = 0;

    std::uint32_t approved_rules = 0;
    std::uint32_t unapproved_rules = 0;
    std::uint32_t validation_errors = 0;

    std::uint32_t variables = 0;
    std::uint32_t constraints = 0;
    std::uint32_t contradictions = 0;

    std::uint64_t feasible_states = 0;
    std::string enumeration_mode = "bruteforce_u32";

    std::uint32_t payoff_rows = 0;
    std::uint32_t payoff_columns = 0;
    std::uint32_t invalid_entries = 0;

    std::uint32_t candidate_bundles = 0;
    std::uint32_t rejected_bundles = 0;

    bool manifest_ok = false;
    bool checksums_ok = false;
    bool determinism_passed = false;

    bool llm_enabled = false;
    std::string llm_provider = "none";
    bool llm_outputs_used = false;

    std::uint64_t workflow_hash = 0;
    std::vector<std::string> errors;

    [[nodiscard]] bool acceptance_ok() const noexcept {
        return markets_loaded > 0 && assets_loaded > 0 && approved_rules > 0 &&
               unapproved_rules == 0 && validation_errors == 0 &&
               variables > 0 && constraints > 0 && contradictions == 0 &&
               feasible_states > 0 && payoff_rows > 0 && manifest_ok &&
               checksums_ok && determinism_passed && !llm_outputs_used &&
               errors.empty();
    }
};

[[nodiscard]] std::optional<Options> parse_args(
    int argc,
    char** argv,
    std::vector<std::string>* errors
) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                errors->push_back(std::string{"missing value for "} + name);
                return std::nullopt;
            }
            return std::string{argv[++i]};
        };

        if (arg == "--market-snapshot") {
            if (auto value = require_value("--market-snapshot")) {
                options.market_snapshot_path = *value;
            }
        } else if (arg == "--rulebook") {
            if (auto value = require_value("--rulebook")) {
                options.rulebook_path = *value;
            }
        } else if (arg == "--candidate-bundles") {
            if (auto value = require_value("--candidate-bundles")) {
                options.candidate_bundles_path = *value;
            }
        } else if (arg == "--out") {
            if (auto value = require_value("--out")) {
                options.out_path = *value;
            }
        } else if (arg == "--check-determinism") {
            options.check_determinism = true;
        } else {
            errors->push_back("unknown argument: " + arg);
        }
    }

    if (options.market_snapshot_path.empty()) {
        errors->push_back("missing --market-snapshot");
    }
    if (options.rulebook_path.empty()) {
        errors->push_back("missing --rulebook");
    }
    if (options.candidate_bundles_path.empty()) {
        errors->push_back("missing --candidate-bundles");
    }
    if (options.out_path.empty()) {
        errors->push_back("missing --out");
    }

    if (!errors->empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] std::string read_text_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

void append_u8(std::vector<std::byte>* out, std::uint8_t value) {
    out->push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i32(std::vector<std::byte>* out, std::int32_t value) {
    append_u32(out, static_cast<std::uint32_t>(value));
}

void append_u64(std::vector<std::byte>* out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i64(std::vector<std::byte>* out, std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

void append_string(std::vector<std::byte>* out, const std::string& value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) {
        append_u8(out, byte);
    }
}

[[nodiscard]] std::vector<std::byte> serialize_variables(
    const std::vector<BooleanVariable>& variables
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(variables.size()));
    for (const auto& variable : variables) {
        append_u32(&out, variable.var_id);
        append_string(&out, variable.variable_key);
        append_string(&out, variable.market_id);
        append_string(&out, variable.outcome_id);
        append_string(&out, variable.asset_id);
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> serialize_constraints(
    const std::vector<LinearBooleanConstraint>& constraints
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(constraints.size()));
    for (const auto& constraint : constraints) {
        append_u8(&out, static_cast<std::uint8_t>(constraint.op));
        append_i32(&out, constraint.rhs);
        append_u32(&out, static_cast<std::uint32_t>(constraint.var_ids.size()));
        for (std::size_t i = 0; i < constraint.var_ids.size(); ++i) {
            append_u32(&out, constraint.var_ids[i]);
            append_i32(&out, constraint.coeffs[i]);
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> serialize_feasible_states(
    const std::vector<FeasibleState>& states
) {
    std::vector<std::byte> out;
    append_u64(&out, static_cast<std::uint64_t>(states.size()));
    for (const auto& state : states) {
        append_u64(&out, state.state_id);
        append_u32(&out, static_cast<std::uint32_t>(state.bitset_words.size()));
        for (const auto word : state.bitset_words) {
            append_u64(&out, word);
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> serialize_payoff_matrix(
    const PayoffMatrix& matrix
) {
    std::vector<std::byte> out;
    append_u32(&out, matrix.row_count);
    append_u32(&out, matrix.column_count);
    append_u64(&out, matrix.payoff_hash);
    append_u32(&out, static_cast<std::uint32_t>(matrix.asset_ids.size()));
    for (const auto& asset_id : matrix.asset_ids) {
        append_string(&out, asset_id);
    }
    append_u32(&out, static_cast<std::uint32_t>(matrix.state_ids.size()));
    for (const auto state_id : matrix.state_ids) {
        append_u64(&out, state_id);
    }
    append_u64(&out, static_cast<std::uint64_t>(matrix.entries.size()));
    for (const auto& entry : matrix.entries) {
        append_u64(&out, entry.state_id);
        append_u32(&out, entry.asset_index);
        append_i64(&out, entry.payout_tick);
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> serialize_candidate_bundles(
    const std::vector<CandidateBundle>& bundles
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(bundles.size()));
    for (const auto& bundle : bundles) {
        append_u64(&out, bundle.bundle_id);
        append_u64(&out, bundle.required_true_mask);
        append_u64(&out, bundle.required_false_mask);
        append_u64(&out, bundle.invalid_mask);
        append_i64(&out, bundle.guaranteed_payout_tick);
        append_u32(&out, bundle.leg_count);
        append_i64(&out, bundle.min_edge_tick);
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            const auto& leg = bundle.legs[i];
            append_string(&out, leg.market_id);
            append_string(&out, leg.asset_id);
            append_u8(&out, static_cast<std::uint8_t>(leg.side));
            append_i64(&out, leg.quantity_lots);
            append_i64(&out, leg.max_price_tick);
        }
    }
    return out;
}

[[nodiscard]] std::string market_universe_json(
    const std::vector<RawMarketRecord>& records
) {
    json::array markets;
    for (const auto& record : records) {
        json::object object;
        object["market_id"] = record.market_id;
        object["event_id"] = record.event_id;
        object["title"] = record.title;
        object["description"] = record.description;

        json::array outcomes;
        for (const auto& outcome : record.outcomes) {
            outcomes.push_back(json::value(outcome));
        }
        object["outcomes"] = std::move(outcomes);

        json::array asset_ids;
        for (const auto& asset_id : record.asset_ids) {
            asset_ids.push_back(json::value(asset_id));
        }
        object["asset_ids"] = std::move(asset_ids);

        object["resolution_source"] = record.resolution_source;
        object["end_time"] = record.end_time;
        object["fetched_at_ns"] = record.fetched_at_ns;
        object["source"] = record.source;
        markets.push_back(std::move(object));
    }

    json::object root;
    root["markets"] = std::move(markets);
    return json::serialize(root) + "\n";
}

[[nodiscard]] std::vector<BooleanVariable> variables_from_universe(
    const MarketUniverse& universe
) {
    std::vector<BooleanVariable> variables;
    for (const auto& market : universe.markets) {
        for (std::size_t i = 0;
             i < market.outcomes.size() && i < market.asset_ids.size();
             ++i) {
            variables.push_back(BooleanVariable{
                .var_id = static_cast<std::uint32_t>(variables.size()),
                .variable_key = market.market_id + ":" + market.outcomes[i],
                .market_id = market.market_id,
                .outcome_id = market.outcomes[i],
                .asset_id = market.asset_ids[i]
            });
        }
    }
    return variables;
}

[[nodiscard]] std::unordered_set<std::string> known_variable_keys(
    const std::vector<BooleanVariable>& variables
) {
    std::unordered_set<std::string> keys;
    for (const auto& variable : variables) {
        keys.insert(variable.variable_key);
    }
    return keys;
}

[[nodiscard]] std::unordered_set<std::string> known_market_ids(
    const MarketUniverse& universe
) {
    std::unordered_set<std::string> out;
    for (const auto& market : universe.markets) {
        out.insert(market.market_id);
    }
    return out;
}

[[nodiscard]] std::unordered_set<std::string> known_asset_ids(
    const MarketUniverse& universe
) {
    std::unordered_set<std::string> out;
    for (const auto& market : universe.markets) {
        for (const auto& asset_id : market.asset_ids) {
            out.insert(asset_id);
        }
    }
    return out;
}

[[nodiscard]] std::uint32_t invalid_payoff_entries(const PayoffMatrix& matrix) {
    std::uint32_t count = 0;
    for (const auto& entry : matrix.entries) {
        if (entry.payout_tick < 0) {
            ++count;
        }
    }
    return count;
}

void hash_string_into(std::uint64_t* hash, const std::string& value) {
    const auto part = checksum_hex(fnv1a64(value));
    for (const unsigned char byte : part) {
        *hash ^= byte;
        *hash *= 1099511628211ULL;
    }
}

[[nodiscard]] std::uint64_t workflow_hash_for(
    const WorkflowSummary& summary,
    const ArtifactExportResult& artifact
) {
    std::uint64_t hash = 14695981039346656037ULL;
    auto hash_u64 = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };

    hash_u64(summary.markets_loaded);
    hash_u64(summary.assets_loaded);
    hash_u64(summary.approved_rules);
    hash_u64(summary.variables);
    hash_u64(summary.constraints);
    hash_u64(summary.feasible_states);
    hash_u64(summary.payoff_rows);
    hash_u64(summary.payoff_columns);
    hash_u64(summary.candidate_bundles);
    hash_u64(artifact.artifact_hash);
    for (const auto& [name, checksum] : artifact.checksums) {
        hash_string_into(&hash, name);
        hash_string_into(&hash, checksum);
    }
    return hash;
}

[[nodiscard]] ArtifactManifest build_manifest(
    const MarketUniverse& universe,
    const Rulebook& rulebook,
    const CompiledConstraintSet& compiled,
    const std::vector<FeasibleState>& feasible_states,
    const PayoffMatrix& payoff_matrix,
    std::uint64_t bundle_hash,
    const OracleArtifactContents& contents
) {
    ArtifactManifest manifest = universe.manifest;
    manifest.artifact_version = 1;
    manifest.created_at_ns = 1;
    manifest.market_count = static_cast<std::uint32_t>(universe.markets.size());
    manifest.asset_count = universe.manifest.asset_count;
    manifest.variable_count = static_cast<std::uint32_t>(compiled.variables.size());
    manifest.rule_count = static_cast<std::uint32_t>(rulebook.rules().size());
    manifest.constraint_count =
        static_cast<std::uint32_t>(compiled.constraints.size());
    manifest.feasible_state_count =
        static_cast<std::uint64_t>(feasible_states.size());
    manifest.bundle_count = 0;
    manifest.llm_enabled = false;
    manifest.llm_outputs_used = false;
    manifest.llm_outputs_require_manual_review = false;
    manifest.llm_provider = "none";

    manifest.input_snapshot_hash =
        checksum_hex(fnv1a64(contents.market_universe_json));
    manifest.rulebook_hash = checksum_hex(fnv1a64(contents.rulebook_json));
    manifest.constraint_hash = checksum_hex(compiled.constraint_hash);
    manifest.feasible_states_hash = checksum_hex(fnv1a64(contents.feasible_states_bin));
    manifest.payoff_hash = checksum_hex(payoff_matrix.payoff_hash);
    manifest.bundle_hash = checksum_hex(bundle_hash);
    return manifest;
}

[[nodiscard]] std::filesystem::path artifact_root(const std::filesystem::path& out) {
    const auto parent = out.parent_path();
    if (parent.empty()) {
        return ".";
    }
    return parent;
}

[[nodiscard]] std::string artifact_id(const std::filesystem::path& out) {
    const auto name = out.filename().string();
    return name.empty() ? "oracle_artifact" : name;
}

[[nodiscard]] WorkflowSummary run_once(
    const Options& options,
    const std::filesystem::path& out_path
) {
    WorkflowSummary summary;

    MarketDescriptionLoader loader;
    const auto loaded_markets = loader.load_jsonl(options.market_snapshot_path);
    summary.missing_fields = static_cast<std::uint32_t>(
        loaded_markets.warnings.size()
    );
    if (!loaded_markets.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            loaded_markets.errors.begin(),
            loaded_markets.errors.end()
        );
        return summary;
    }

    MarketUniverseBuilder universe_builder;
    const auto universe_result =
        universe_builder.build(loaded_markets.records);
    if (!universe_result.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            universe_result.errors.begin(),
            universe_result.errors.end()
        );
        return summary;
    }

    const auto variables = variables_from_universe(universe_result.universe);
    summary.markets_loaded = static_cast<std::uint32_t>(
        universe_result.universe.markets.size()
    );
    summary.assets_loaded = universe_result.universe.manifest.asset_count;

    const auto loaded_rules = Rulebook::load_json(options.rulebook_path);
    summary.validation_errors =
        static_cast<std::uint32_t>(loaded_rules.errors.size());
    if (!loaded_rules.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            loaded_rules.errors.begin(),
            loaded_rules.errors.end()
        );
        return summary;
    }

    Rulebook rulebook;
    for (const auto& rule : loaded_rules.rules) {
        rulebook.add_rule(rule);
    }
    summary.approved_rules =
        static_cast<std::uint32_t>(rulebook.approved_rules().size());

    RuleValidator validator;
    const auto rule_validation =
        validator.validate_rulebook(rulebook, known_variable_keys(variables));
    summary.unapproved_rules =
        static_cast<std::uint32_t>(rule_validation.unapproved_rules.size());
    summary.validation_errors +=
        static_cast<std::uint32_t>(rule_validation.errors.size());
    if (!rule_validation.compiler_ready()) {
        summary.errors.insert(
            summary.errors.end(),
            rule_validation.errors.begin(),
            rule_validation.errors.end()
        );
        for (const auto& rule_id : rule_validation.unapproved_rules) {
            summary.errors.push_back("unapproved rule: " + rule_id);
        }
        return summary;
    }

    ConstraintCompiler compiler;
    const auto compiled_result = compiler.compile(rulebook, variables);
    summary.variables =
        static_cast<std::uint32_t>(compiled_result.compiled.variables.size());
    summary.constraints =
        static_cast<std::uint32_t>(compiled_result.compiled.constraints.size());
    if (!compiled_result.ok()) {
        summary.validation_errors +=
            static_cast<std::uint32_t>(compiled_result.errors.size());
        summary.errors.insert(
            summary.errors.end(),
            compiled_result.errors.begin(),
            compiled_result.errors.end()
        );
        return summary;
    }

    StateEnumerator enumerator;
    const auto enumeration = enumerator.enumerate(compiled_result.compiled);
    if (!enumeration.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            enumeration.errors.begin(),
            enumeration.errors.end()
        );
        return summary;
    }
    summary.feasible_states =
        static_cast<std::uint64_t>(enumeration.feasible_states.size());
    summary.contradictions = summary.feasible_states == 0 ? 1U : 0U;

    PayoffMatrixBuilder payoff_builder;
    const auto payoff = payoff_builder.build(
        compiled_result.compiled.variables,
        enumeration.feasible_states
    );
    if (!payoff.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            payoff.errors.begin(),
            payoff.errors.end()
        );
        return summary;
    }
    summary.payoff_rows = payoff.matrix.row_count;
    summary.payoff_columns = payoff.matrix.column_count;
    summary.invalid_entries = invalid_payoff_entries(payoff.matrix);

    CandidateBundleGenerator bundle_generator;
    const auto bundles = bundle_generator.load_fixture(
        options.candidate_bundles_path,
        known_market_ids(universe_result.universe),
        known_asset_ids(universe_result.universe)
    );
    summary.candidate_bundles =
        static_cast<std::uint32_t>(bundles.bundles.size());
    summary.rejected_bundles = static_cast<std::uint32_t>(bundles.errors.size());
    if (!bundles.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            bundles.errors.begin(),
            bundles.errors.end()
        );
        return summary;
    }

    OracleArtifactContents contents;
    contents.market_universe_json = market_universe_json(
        universe_result.universe.markets
    );
    contents.rulebook_json = read_text_file(options.rulebook_path);
    contents.variables_bin = serialize_variables(compiled_result.compiled.variables);
    contents.constraints_bin = serialize_constraints(
        compiled_result.compiled.constraints
    );
    contents.feasible_states_bin =
        serialize_feasible_states(enumeration.feasible_states);
    contents.payoff_matrix_bin = serialize_payoff_matrix(payoff.matrix);
    contents.candidate_bundles_bin = serialize_candidate_bundles(bundles.bundles);
    contents.market_dependency_graph_bin = {};
    contents.settlement_bitmask_bin = {};
    contents.manifest = build_manifest(
        universe_result.universe,
        rulebook,
        compiled_result.compiled,
        enumeration.feasible_states,
        payoff.matrix,
        bundles.bundle_hash,
        contents
    );
    contents.manifest.bundle_count =
        static_cast<std::uint64_t>(bundles.bundles.size());

    ArtifactExporter exporter;
    const auto exported = exporter.export_artifact(
        artifact_root(out_path),
        artifact_id(out_path),
        contents
    );
    if (!exported.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            exported.errors.begin(),
            exported.errors.end()
        );
        return summary;
    }

    ArtifactLoader artifact_loader;
    const auto loaded_artifact = artifact_loader.load(exported.artifact_dir);
    summary.checksums_ok = loaded_artifact.checksums_ok;
    summary.manifest_ok =
        loaded_artifact.ok() &&
        loaded_artifact.contents.manifest.artifact_version == 1 &&
        loaded_artifact.contents.manifest.market_count == summary.markets_loaded &&
        loaded_artifact.contents.manifest.variable_count == summary.variables &&
        loaded_artifact.contents.manifest.constraint_count == summary.constraints &&
        loaded_artifact.contents.manifest.feasible_state_count ==
            summary.feasible_states;

    if (!loaded_artifact.ok()) {
        summary.errors.insert(
            summary.errors.end(),
            loaded_artifact.errors.begin(),
            loaded_artifact.errors.end()
        );
    }

    summary.llm_enabled = contents.manifest.llm_enabled;
    summary.llm_provider = contents.manifest.llm_provider;
    summary.llm_outputs_used = contents.manifest.llm_outputs_used;
    summary.workflow_hash = workflow_hash_for(summary, exported);
    return summary;
}

void print_summary(const WorkflowSummary& summary) {
    std::cout << "ingestion:\n";
    std::cout << "  markets_loaded: " << summary.markets_loaded << '\n';
    std::cout << "  assets_loaded: " << summary.assets_loaded << '\n';
    std::cout << "  missing_fields: " << summary.missing_fields << "\n\n";

    std::cout << "rules:\n";
    std::cout << "  approved_rules: " << summary.approved_rules << '\n';
    std::cout << "  unapproved_rules: " << summary.unapproved_rules << '\n';
    std::cout << "  validation_errors: " << summary.validation_errors << "\n\n";

    std::cout << "constraints:\n";
    std::cout << "  variables: " << summary.variables << '\n';
    std::cout << "  constraints: " << summary.constraints << '\n';
    std::cout << "  contradictions: " << summary.contradictions << "\n\n";

    std::cout << "states:\n";
    std::cout << "  feasible_states: " << summary.feasible_states << '\n';
    std::cout << "  enumeration_mode: " << summary.enumeration_mode << "\n\n";

    std::cout << "payoff:\n";
    std::cout << "  rows: " << summary.payoff_rows << '\n';
    std::cout << "  columns: " << summary.payoff_columns << '\n';
    std::cout << "  invalid_entries: " << summary.invalid_entries << "\n\n";

    std::cout << "bundles:\n";
    std::cout << "  candidate_bundles: " << summary.candidate_bundles << '\n';
    std::cout << "  rejected_bundles: " << summary.rejected_bundles << "\n\n";

    std::cout << "artifacts:\n";
    std::cout << "  manifest_ok: " << (summary.manifest_ok ? "true" : "false") << '\n';
    std::cout << "  checksums_ok: " << (summary.checksums_ok ? "true" : "false") << '\n';
    std::cout << "  determinism_passed: "
              << (summary.determinism_passed ? "true" : "false") << "\n\n";

    std::cout << "llm:\n";
    std::cout << "  enabled: " << (summary.llm_enabled ? "true" : "false") << '\n';
    std::cout << "  provider: " << summary.llm_provider << '\n';
    std::cout << "  outputs_used: "
              << (summary.llm_outputs_used ? "true" : "false") << '\n';
}

}  // namespace
}  // namespace trading_engine::oracle

int main(int argc, char** argv) {
    using namespace trading_engine::oracle;

    std::vector<std::string> errors;
    const auto options = parse_args(argc, argv, &errors);
    if (!options) {
        for (const auto& error : errors) {
            std::cerr << error << '\n';
        }
        return 2;
    }

    auto summary = run_once(*options, options->out_path);
    if (options->check_determinism && summary.errors.empty()) {
        const auto second_out =
            std::filesystem::path{options->out_path.string() + "_determinism"};
        const auto second = run_once(*options, second_out);
        summary.determinism_passed =
            second.errors.empty() &&
            second.workflow_hash == summary.workflow_hash &&
            second.markets_loaded == summary.markets_loaded &&
            second.assets_loaded == summary.assets_loaded &&
            second.approved_rules == summary.approved_rules &&
            second.variables == summary.variables &&
            second.constraints == summary.constraints &&
            second.feasible_states == summary.feasible_states &&
            second.payoff_rows == summary.payoff_rows &&
            second.payoff_columns == summary.payoff_columns &&
            second.candidate_bundles == summary.candidate_bundles &&
            second.manifest_ok == summary.manifest_ok &&
            second.checksums_ok == summary.checksums_ok;
        if (!second.errors.empty()) {
            summary.errors.insert(
                summary.errors.end(),
                second.errors.begin(),
                second.errors.end()
            );
        }
    } else if (!options->check_determinism) {
        summary.determinism_passed = true;
    }

    print_summary(summary);
    if (!summary.errors.empty()) {
        std::cerr << "\nerrors:\n";
        for (const auto& error : summary.errors) {
            std::cerr << "  " << error << '\n';
        }
    }

    return summary.acceptance_ok() ? 0 : 1;
}

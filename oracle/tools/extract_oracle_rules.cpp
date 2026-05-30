#include "oracle/ingestion/MarketDescriptionLoader.h"
#include "oracle/llm/OpenRouterRuleExtractor.h"
#include "oracle/rules/ManualRuleEditor.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::oracle {
namespace {

struct Options {
    std::string market_snapshot_path;
    std::string drafts_in_path;
    std::string drafts_out_path;
    std::string approved_rulebook_out_path;
    std::string approved_by = "manual";
    std::string event_id_filter;
    std::string openrouter_model;
    std::uint32_t max_tokens = 0;
    std::uint64_t approved_at_ns = 1;
    bool use_llm = false;
    bool approve_drafts = false;
};

[[nodiscard]] std::optional<std::string> require_value(
    int argc,
    char** argv,
    int* index,
    const std::string& name,
    std::vector<std::string>* errors
) {
    if (*index + 1 >= argc) {
        errors->push_back("missing value for " + name);
        return std::nullopt;
    }
    ++(*index);
    return std::string{argv[*index]};
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(
    const std::string& value
) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::uint32_t> parse_u32(
    const std::string& value
) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Options> parse_args(
    int argc,
    char** argv,
    std::vector<std::string>* errors
) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--market-snapshot") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.market_snapshot_path = *value;
            }
        } else if (arg == "--drafts-in") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.drafts_in_path = *value;
            }
        } else if (arg == "--drafts-out") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.drafts_out_path = *value;
            }
        } else if (arg == "--approved-rulebook-out") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.approved_rulebook_out_path = *value;
            }
        } else if (arg == "--approved-by") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.approved_by = *value;
            }
        } else if (arg == "--event-id") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.event_id_filter = *value;
            }
        } else if (arg == "--model") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                options.openrouter_model = *value;
            }
        } else if (arg == "--max-tokens") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                if (auto parsed = parse_u32(*value)) {
                    options.max_tokens = *parsed;
                } else {
                    errors->push_back("invalid --max-tokens");
                }
            }
        } else if (arg == "--approved-at-ns") {
            if (auto value = require_value(argc, argv, &i, arg, errors)) {
                if (auto parsed = parse_u64(*value)) {
                    options.approved_at_ns = *parsed;
                } else {
                    errors->push_back("invalid --approved-at-ns");
                }
            }
        } else if (arg == "--use-llm") {
            options.use_llm = true;
        } else if (arg == "--approve-drafts") {
            options.approve_drafts = true;
        } else {
            errors->push_back("unknown argument: " + arg);
        }
    }

    if (options.use_llm && options.market_snapshot_path.empty()) {
        errors->push_back("--use-llm requires --market-snapshot");
    }
    if (options.use_llm && options.drafts_out_path.empty() &&
        !options.approve_drafts) {
        errors->push_back("--use-llm requires --drafts-out unless approving");
    }
    if (!options.use_llm && options.drafts_in_path.empty()) {
        errors->push_back("missing --drafts-in or --use-llm");
    }
    if (options.approve_drafts && options.approved_rulebook_out_path.empty()) {
        errors->push_back(
            "--approve-drafts requires --approved-rulebook-out"
        );
    }
    if (options.approve_drafts && options.approved_by.empty()) {
        errors->push_back("--approve-drafts requires non-empty --approved-by");
    }

    if (!errors->empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] LLMRuleExtractionRequest make_request(
    const std::string& market_snapshot_path,
    const std::string& event_id_filter
) {
    LLMRuleExtractionRequest request;
    request.instruction =
        "Extract only conservative Boolean combinatorial rules that are "
        "directly supported by the current compiler. Do not generate trivial "
        "single-market YES/NO constraints. Focus on cross-market rules within "
        "the same event_id: MutuallyExclusive with exhaustive=true for "
        "complete winner/bracket sets, MutuallyExclusive with exhaustive=false "
        "for exclusive-only sets, and Implies only when the market text "
        "directly states a dependency.";
    request.requested_at_ns = 1;

    MarketDescriptionLoader loader;
    const auto loaded = loader.load_jsonl(market_snapshot_path);
    for (const auto& record : loaded.records) {
        if (!event_id_filter.empty() && record.event_id != event_id_filter) {
            continue;
        }
        request.markets.push_back(LLMMarketContext{
            .market_id = record.market_id,
            .event_id = record.event_id,
            .title = record.title,
            .description = record.description,
            .outcomes = record.outcomes,
            .asset_ids = record.asset_ids,
            .resolution_source = record.resolution_source
        });
    }
    return request;
}

[[nodiscard]] std::string effective_openrouter_model(
    const Options& options
) {
    if (!options.openrouter_model.empty()) {
        return options.openrouter_model;
    }

    const char* env_model = std::getenv(OpenRouterRuleExtractor::kModelEnvVar);
    if (env_model && *env_model != '\0') {
        return env_model;
    }
    return OpenRouterRuleExtractor::kDefaultModel;
}

[[nodiscard]] std::uint32_t effective_openrouter_max_tokens(
    const Options& options
) {
    if (options.max_tokens != 0) {
        return options.max_tokens;
    }

    const char* env_value = std::getenv(
        OpenRouterRuleExtractor::kMaxTokensEnvVar
    );
    if (env_value && *env_value != '\0') {
        if (auto parsed = parse_u32(env_value)) {
            return *parsed;
        }
    }
    return OpenRouterRuleExtractor::kDefaultMaxTokens;
}

[[nodiscard]] std::vector<RuleDraft> load_or_extract_drafts(
    const Options& options,
    std::vector<std::string>* errors
) {
    if (options.use_llm) {
        OpenRouterRuleExtractor extractor(
            true,
            OpenRouterRuleExtractor::kDefaultApiKeyEnvVar,
            effective_openrouter_model(options),
            OpenRouterRuleExtractor::kChatCompletionsEndpoint,
            effective_openrouter_max_tokens(options)
        );
        const auto request = make_request(
            options.market_snapshot_path,
            options.event_id_filter
        );
        if (request.markets.empty()) {
            errors->push_back("no markets matched --market-snapshot/filter");
            return {};
        }

        const auto result = extractor.extract(request);
        if (result.status != LLMExtractionStatus::Ok) {
            errors->push_back(result.diagnostic);
            return {};
        }
        return result.drafts;
    }

    ManualRuleEditor editor;
    const auto loaded = editor.load_rule_drafts(options.drafts_in_path);
    if (!loaded.ok()) {
        errors->insert(errors->end(), loaded.errors.begin(), loaded.errors.end());
        return {};
    }
    return loaded.drafts;
}

void create_parent_directory(const std::string& path) {
    const auto parent = std::filesystem::path{path}.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

int run(const Options& options) {
    std::vector<std::string> errors;
    auto drafts = load_or_extract_drafts(options, &errors);
    if (!errors.empty()) {
        for (const auto& error : errors) {
            std::cerr << error << '\n';
        }
        return 1;
    }

    ManualRuleEditor editor;
    if (!options.drafts_out_path.empty()) {
        create_parent_directory(options.drafts_out_path);
        if (!editor.write_rule_drafts(
                drafts,
                options.drafts_out_path,
                &errors
            )) {
            for (const auto& error : errors) {
                std::cerr << error << '\n';
            }
            return 1;
        }
    }

    std::size_t approved_count = 0;
    if (options.approve_drafts) {
        create_parent_directory(options.approved_rulebook_out_path);
        const auto rulebook = editor.approve_drafts(
            drafts,
            options.approved_by,
            options.approved_at_ns
        );
        if (!editor.write_approved_rulebook(
                rulebook,
                options.approved_rulebook_out_path,
                &errors
            )) {
            for (const auto& error : errors) {
                std::cerr << error << '\n';
            }
            return 1;
        }
        approved_count = rulebook.approved_rules().size();
    }

    std::cout << "oracle_rule_extraction:\n";
    std::cout << "  llm_used: " << (options.use_llm ? "true" : "false")
              << '\n';
    std::cout << "  model: " << effective_openrouter_model(options) << '\n';
    std::cout << "  max_tokens: " << effective_openrouter_max_tokens(options)
              << '\n';
    if (!options.event_id_filter.empty()) {
        std::cout << "  event_id: " << options.event_id_filter << '\n';
    }
    std::cout << "  drafts: " << drafts.size() << '\n';
    std::cout << "  drafts_out: " << options.drafts_out_path << '\n';
    std::cout << "  approved_rules: " << approved_count << '\n';
    std::cout << "  approved_rulebook_out: "
              << options.approved_rulebook_out_path << '\n';
    return 0;
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
    return run(*options);
}

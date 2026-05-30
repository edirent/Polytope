#include "oracle/llm/OpenRouterRuleExtractor.h"
#include "oracle/llm/StubRuleExtractor.h"
#include "oracle/rules/ValidatedRule.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

using trading_engine::oracle::LLMExtractionStatus;
using trading_engine::oracle::LLMRuleExtractionRequest;
using trading_engine::oracle::OpenRouterRuleExtractor;
using trading_engine::oracle::RuleDraft;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::StubRuleExtractor;
using trading_engine::oracle::ValidatedRule;
using trading_engine::oracle::build_openrouter_rule_extraction_prompt;
using trading_engine::oracle::parse_openrouter_chat_response;
using trading_engine::oracle::parse_rule_drafts_json;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_equal(
    LLMExtractionStatus actual,
    LLMExtractionStatus expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

LLMRuleExtractionRequest request() {
    LLMRuleExtractionRequest out;
    out.instruction = "extract draft rules only";
    out.markets.push_back({
        .market_id = "m1",
        .event_id = "e1",
        .title = "Winner?",
        .description = "Fixture market",
        .outcomes = {"YES", "NO"},
        .asset_ids = {"asset_yes", "asset_no"},
        .resolution_source = "fixture"
    });
    return out;
}

void StubRuleExtractor_NoNetwork() {
    StubRuleExtractor extractor;
    const auto result = extractor.extract(request());

    expect_equal(
        result.status,
        LLMExtractionStatus::Disabled,
        "status"
    );
    expect_true(result.drafts.empty(), "drafts empty");
}

void StubRuleExtractor_ReturnsDisabled() {
    StubRuleExtractor extractor;
    const auto result = extractor.extract({});

    expect_equal(
        result.status,
        LLMExtractionStatus::Disabled,
        "status"
    );
}

void StubRuleExtractor_DoesNotProduceValidatedRules() {
    StubRuleExtractor extractor;
    const auto result = extractor.extract(request());

    using Drafts = decltype(result.drafts);
    static_assert(std::is_same_v<Drafts::value_type, RuleDraft>);
    static_assert(!std::is_same_v<Drafts::value_type, ValidatedRule>);
    expect_true(result.drafts.empty(), "drafts empty");
}

void OpenRouterRuleExtractor_DisabledByDefault() {
    OpenRouterRuleExtractor extractor;
    const auto result = extractor.extract(request());

    expect_equal(
        result.status,
        LLMExtractionStatus::Disabled,
        "status"
    );
    expect_true(result.drafts.empty(), "drafts empty");
}

void OpenRouterRuleExtractor_MissingApiKeyWhenEnabled() {
    OpenRouterRuleExtractor extractor(
        true,
        "__POLYTOPE_MISSING_OPENROUTER_API_KEY__"
    );
    const auto result = extractor.extract(request());

    expect_equal(
        result.status,
        LLMExtractionStatus::MissingApiKey,
        "status"
    );
    expect_true(result.drafts.empty(), "drafts empty");
}

void OpenRouterRuleExtractor_UsesLlama33FreeModel() {
    OpenRouterRuleExtractor extractor(false);

    expect_true(
        extractor.api_key_env_var() == "OPENROUTER_API_KEY",
        "api key env var"
    );
    expect_true(
        std::string{OpenRouterRuleExtractor::kModelEnvVar} ==
            "OPENROUTER_MODEL",
        "model env var"
    );
    expect_true(
        std::string{OpenRouterRuleExtractor::kMaxTokensEnvVar} ==
            "OPENROUTER_MAX_TOKENS",
        "max tokens env var"
    );
    expect_true(
        extractor.model() == "meta-llama/llama-3.3-70b-instruct:free",
        "model"
    );
    expect_true(
        extractor.max_tokens() == OpenRouterRuleExtractor::kDefaultMaxTokens,
        "max tokens"
    );
    expect_true(
        extractor.endpoint() ==
            "https://openrouter.ai/api/v1/chat/completions",
        "endpoint"
    );
}

void OpenRouterRuleExtractor_ModelCanBeOverridden() {
    OpenRouterRuleExtractor extractor(
        false,
        OpenRouterRuleExtractor::kDefaultApiKeyEnvVar,
        "openrouter/test-model"
    );

    expect_true(extractor.model() == "openrouter/test-model", "model");
}

void OpenRouterRuleExtractor_MaxTokensCanBeOverridden() {
    OpenRouterRuleExtractor extractor(
        false,
        OpenRouterRuleExtractor::kDefaultApiKeyEnvVar,
        OpenRouterRuleExtractor::kDefaultModel,
        OpenRouterRuleExtractor::kChatCompletionsEndpoint,
        128
    );

    expect_true(extractor.max_tokens() == 128, "max tokens");
}

void OpenRouterRuleExtractor_PromptRequiresCombinatorialRules() {
    auto input = request();
    input.markets.push_back({
        .market_id = "m2",
        .event_id = "e1",
        .title = "Other winner?",
        .description = "Same event fixture market",
        .outcomes = {"YES", "NO"},
        .asset_ids = {"asset2_yes", "asset2_no"},
        .resolution_source = "fixture"
    });

    const auto prompt = build_openrouter_rule_extraction_prompt(input);
    expect_true(
        prompt.find("DO NOT generate trivial intra-market constraints") !=
            std::string::npos,
        "no trivial directive"
    );
    expect_true(
        prompt.find("ONLY COMBINATORIAL CONSTRAINTS") != std::string::npos,
        "combinatorial directive"
    );
    expect_true(
        prompt.find("event_id: e1") != std::string::npos,
        "event group"
    );
    expect_true(
        prompt.find("variable_id: m1:YES") != std::string::npos,
        "variable id"
    );
    expect_true(
        prompt.find("asset_id: asset_yes") != std::string::npos,
        "asset audit context"
    );
}

void OpenRouterRuleExtractor_ParsesDraftJson() {
    const auto result = parse_rule_drafts_json(R"json(
        {
          "drafts": [
            {
              "rule_id": "draft_exactly_one_m1",
              "type": "ExactlyOne",
              "variable_ids": ["m1:YES", "m1:NO"],
              "source_text": "YES and NO are the market outcomes",
              "rationale": "binary market outcomes are exhaustive",
              "requires_manual_review": false
            }
          ]
        }
    )json");

    expect_equal(result.status, LLMExtractionStatus::Ok, "status");
    expect_true(result.drafts.size() == 1, "draft count");
    expect_true(result.drafts.front().requires_manual_review, "manual review");
    expect_true(
        result.drafts.front().type == RuleType::ExactlyOne,
        "rule type"
    );
}

void OpenRouterRuleExtractor_ParsesChatCompletionResponse() {
    const auto result = parse_openrouter_chat_response(R"json(
        {
          "choices": [
            {
              "message": {
                "content": "```json\n{\"drafts\":[{\"rule_id\":\"draft_1\",\"type\":\"ExactlyOne\",\"variable_ids\":[\"m1:YES\",\"m1:NO\"],\"source_text\":\"fixture\",\"rationale\":\"binary\",\"requires_manual_review\":true}]}\n```"
              }
            }
          ]
        }
    )json");

    expect_equal(result.status, LLMExtractionStatus::Ok, "status");
    expect_true(result.drafts.size() == 1, "draft count");
    expect_true(result.drafts.front().rule_id == "draft_1", "rule id");
}

void OpenRouterRuleExtractor_ParsesChatCompletionContentArray() {
    const auto result = parse_openrouter_chat_response(R"json(
        {
          "choices": [
            {
              "message": {
                "content": [
                  {
                    "type": "text",
                    "text": "{\"drafts\":[{\"rule_id\":\"draft_array\",\"type\":\"ExactlyOne\",\"variable_ids\":[\"m1:YES\",\"m1:NO\"],\"source_text\":\"fixture\",\"rationale\":\"binary\",\"requires_manual_review\":true}]}"
                  }
                ]
              }
            }
          ]
        }
    )json");

    expect_equal(result.status, LLMExtractionStatus::Ok, "status");
    expect_true(result.drafts.size() == 1, "draft count");
    expect_true(result.drafts.front().rule_id == "draft_array", "rule id");
}

void OpenRouterRuleExtractor_ExtractsJsonFromProse() {
    const auto result = parse_rule_drafts_json(R"json(
        Here is the JSON:
        {"drafts":[{"rule_id":"draft_prose","type":"ExactlyOne","variable_ids":["m1:YES","m1:NO"],"source_text":"fixture","rationale":"binary","requires_manual_review":true}]}
    )json");

    expect_equal(result.status, LLMExtractionStatus::Ok, "status");
    expect_true(result.drafts.size() == 1, "draft count");
    expect_true(result.drafts.front().rule_id == "draft_prose", "rule id");
}

void OpenRouterRuleExtractor_InvalidJsonDiagnosticIncludesContent() {
    const auto result = parse_rule_drafts_json("not json at all");

    expect_equal(result.status, LLMExtractionStatus::InvalidResponse, "status");
    expect_true(
        result.diagnostic.find("not json at all") != std::string::npos,
        "diagnostic"
    );
}

void OpenRouterRuleExtractor_EmptyContentDiagnosticIncludesChoice() {
    const auto result = parse_openrouter_chat_response(R"json(
        {
          "choices": [
            {
              "finish_reason": "length",
              "message": {
                "content": ""
              }
            }
          ]
        }
    )json");

    expect_equal(result.status, LLMExtractionStatus::InvalidResponse, "status");
    expect_true(
        result.diagnostic.find("finish_reason") != std::string::npos,
        "diagnostic"
    );
}

void OpenRouterRuleExtractorManual() {
    OpenRouterRuleExtractor extractor;
    const auto result = extractor.extract(request());

    expect_true(
        result.status == LLMExtractionStatus::Disabled ||
        result.status == LLMExtractionStatus::MissingApiKey ||
        result.status == LLMExtractionStatus::ProviderError,
        "manual status"
    );
    expect_true(result.drafts.empty(), "no placeholder drafts");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StubRuleExtractor_NoNetwork", &StubRuleExtractor_NoNetwork},
        {
            "StubRuleExtractor_ReturnsDisabled",
            &StubRuleExtractor_ReturnsDisabled
        },
        {
            "StubRuleExtractor_DoesNotProduceValidatedRules",
            &StubRuleExtractor_DoesNotProduceValidatedRules
        },
        {
            "OpenRouterRuleExtractor_DisabledByDefault",
            &OpenRouterRuleExtractor_DisabledByDefault
        },
        {
            "OpenRouterRuleExtractor_MissingApiKeyWhenEnabled",
            &OpenRouterRuleExtractor_MissingApiKeyWhenEnabled
        },
        {
            "OpenRouterRuleExtractor_UsesLlama33FreeModel",
            &OpenRouterRuleExtractor_UsesLlama33FreeModel
        },
        {
            "OpenRouterRuleExtractor_ModelCanBeOverridden",
            &OpenRouterRuleExtractor_ModelCanBeOverridden
        },
        {
            "OpenRouterRuleExtractor_MaxTokensCanBeOverridden",
            &OpenRouterRuleExtractor_MaxTokensCanBeOverridden
        },
        {
            "OpenRouterRuleExtractor_PromptRequiresCombinatorialRules",
            &OpenRouterRuleExtractor_PromptRequiresCombinatorialRules
        },
        {
            "OpenRouterRuleExtractor_ParsesDraftJson",
            &OpenRouterRuleExtractor_ParsesDraftJson
        },
        {
            "OpenRouterRuleExtractor_ParsesChatCompletionResponse",
            &OpenRouterRuleExtractor_ParsesChatCompletionResponse
        },
        {
            "OpenRouterRuleExtractor_ParsesChatCompletionContentArray",
            &OpenRouterRuleExtractor_ParsesChatCompletionContentArray
        },
        {
            "OpenRouterRuleExtractor_ExtractsJsonFromProse",
            &OpenRouterRuleExtractor_ExtractsJsonFromProse
        },
        {
            "OpenRouterRuleExtractor_InvalidJsonDiagnosticIncludesContent",
            &OpenRouterRuleExtractor_InvalidJsonDiagnosticIncludesContent
        },
        {
            "OpenRouterRuleExtractor_EmptyContentDiagnosticIncludesChoice",
            &OpenRouterRuleExtractor_EmptyContentDiagnosticIncludesChoice
        },
        {"OpenRouterRuleExtractorManual", &OpenRouterRuleExtractorManual}
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
        if (name == "OpenRouterRuleExtractorManual") {
            continue;
        }
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}

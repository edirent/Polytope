#include "oracle/llm/ClaudeRuleExtractor.h"
#include "oracle/llm/StubRuleExtractor.h"
#include "oracle/rules/ValidatedRule.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

using trading_engine::oracle::ClaudeRuleExtractor;
using trading_engine::oracle::LLMExtractionStatus;
using trading_engine::oracle::LLMRuleExtractionRequest;
using trading_engine::oracle::RuleDraft;
using trading_engine::oracle::StubRuleExtractor;
using trading_engine::oracle::ValidatedRule;

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

void ClaudeRuleExtractor_DisabledByDefault() {
    ClaudeRuleExtractor extractor;
    const auto result = extractor.extract(request());

    expect_equal(
        result.status,
        LLMExtractionStatus::Disabled,
        "status"
    );
    expect_true(result.drafts.empty(), "drafts empty");
}

void ClaudeRuleExtractor_MissingApiKeyWhenEnabled() {
    ClaudeRuleExtractor extractor(
        true,
        "__POLYTOPE_MISSING_ANTHROPIC_API_KEY__"
    );
    const auto result = extractor.extract(request());

    expect_equal(
        result.status,
        LLMExtractionStatus::MissingApiKey,
        "status"
    );
    expect_true(result.drafts.empty(), "drafts empty");
}

void ClaudeRuleExtractorManual() {
    ClaudeRuleExtractor extractor;
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
            "ClaudeRuleExtractor_DisabledByDefault",
            &ClaudeRuleExtractor_DisabledByDefault
        },
        {
            "ClaudeRuleExtractor_MissingApiKeyWhenEnabled",
            &ClaudeRuleExtractor_MissingApiKeyWhenEnabled
        },
        {"ClaudeRuleExtractorManual", &ClaudeRuleExtractorManual}
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
        if (name == "ClaudeRuleExtractorManual") {
            continue;
        }
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}

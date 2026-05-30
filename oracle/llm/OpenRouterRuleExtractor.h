#pragma once

#include "oracle/llm/ILLMRuleExtractor.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace trading_engine::oracle {

class OpenRouterRuleExtractor final : public ILLMRuleExtractor {
public:
    static constexpr const char* kDefaultApiKeyEnvVar = "OPENROUTER_API_KEY";
    static constexpr const char* kModelEnvVar = "OPENROUTER_MODEL";
    static constexpr const char* kMaxTokensEnvVar = "OPENROUTER_MAX_TOKENS";
    static constexpr const char* kDefaultModel =
        "meta-llama/llama-3.3-70b-instruct:free";
    static constexpr std::uint32_t kDefaultMaxTokens = 256;
    static constexpr const char* kChatCompletionsEndpoint =
        "https://openrouter.ai/api/v1/chat/completions";

    OpenRouterRuleExtractor();
    explicit OpenRouterRuleExtractor(
        bool enabled,
        std::string api_key_env_var = kDefaultApiKeyEnvVar,
        std::string model = kDefaultModel,
        std::string endpoint = kChatCompletionsEndpoint,
        std::uint32_t max_tokens = kDefaultMaxTokens
    );

    [[nodiscard]] LLMRuleExtractionResult extract(
        const LLMRuleExtractionRequest& request
    ) override;

    [[nodiscard]] const std::string& model() const noexcept;
    [[nodiscard]] const std::string& endpoint() const noexcept;
    [[nodiscard]] const std::string& api_key_env_var() const noexcept;
    [[nodiscard]] std::uint32_t max_tokens() const noexcept;

private:
    bool enabled_ = false;
    std::string api_key_env_var_ = kDefaultApiKeyEnvVar;
    std::string model_ = kDefaultModel;
    std::string endpoint_ = kChatCompletionsEndpoint;
    std::uint32_t max_tokens_ = kDefaultMaxTokens;
};

[[nodiscard]] std::string build_openrouter_rule_extraction_prompt(
    const LLMRuleExtractionRequest& request
);

[[nodiscard]] LLMRuleExtractionResult parse_rule_drafts_json(
    std::string_view content
);

[[nodiscard]] LLMRuleExtractionResult parse_openrouter_chat_response(
    std::string_view response_body
);

}  // namespace trading_engine::oracle

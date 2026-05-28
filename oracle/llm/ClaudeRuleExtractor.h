#pragma once

#include "oracle/llm/ILLMRuleExtractor.h"

#include <string>

namespace trading_engine::oracle {

class ClaudeRuleExtractor final : public ILLMRuleExtractor {
public:
    ClaudeRuleExtractor();
    explicit ClaudeRuleExtractor(
        bool enabled,
        std::string api_key_env_var = "ANTHROPIC_API_KEY"
    );

    [[nodiscard]] LLMRuleExtractionResult extract(
        const LLMRuleExtractionRequest& request
    ) override;

private:
    bool enabled_ = false;
    std::string api_key_env_var_ = "ANTHROPIC_API_KEY";
};

}  // namespace trading_engine::oracle

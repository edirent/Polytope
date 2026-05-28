#pragma once

#include "oracle/llm/ILLMRuleExtractor.h"

namespace trading_engine::oracle {

class StubRuleExtractor final : public ILLMRuleExtractor {
public:
    [[nodiscard]] LLMRuleExtractionResult extract(
        const LLMRuleExtractionRequest& request
    ) override;
};

}  // namespace trading_engine::oracle

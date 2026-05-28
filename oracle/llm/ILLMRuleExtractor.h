#pragma once

#include "oracle/llm/LLMRuleExtractionRequest.h"
#include "oracle/llm/LLMRuleExtractionResult.h"

namespace trading_engine::oracle {

class ILLMRuleExtractor {
public:
    virtual LLMRuleExtractionResult extract(
        const LLMRuleExtractionRequest& request
    ) = 0;

    virtual ~ILLMRuleExtractor() = default;
};

}  // namespace trading_engine::oracle

#include "oracle/llm/StubRuleExtractor.h"

namespace trading_engine::oracle {

LLMRuleExtractionResult StubRuleExtractor::extract(
    const LLMRuleExtractionRequest&
) {
    LLMRuleExtractionResult result;
    result.status = LLMExtractionStatus::Disabled;
    result.diagnostic = "stub rule extractor is disabled";
    return result;
}

}  // namespace trading_engine::oracle

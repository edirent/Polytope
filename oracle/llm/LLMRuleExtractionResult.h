#pragma once

#include "oracle/rules/RuleDraft.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

enum class LLMExtractionStatus : std::uint8_t {
    Ok,
    Disabled,
    MissingApiKey,
    ProviderError,
    InvalidResponse
};

struct LLMRuleExtractionResult {
    LLMExtractionStatus status = LLMExtractionStatus::Disabled;
    std::vector<RuleDraft> drafts;
    std::string diagnostic;
};

}  // namespace trading_engine::oracle

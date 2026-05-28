#include "oracle/llm/ClaudeRuleExtractor.h"

#include <cstdlib>
#include <utility>

namespace trading_engine::oracle {

namespace {

constexpr bool default_llm_enabled() noexcept {
#if ORACLE_ENABLE_LLM
    return true;
#else
    return false;
#endif
}

}  // namespace

ClaudeRuleExtractor::ClaudeRuleExtractor()
    : ClaudeRuleExtractor(default_llm_enabled()) {}

ClaudeRuleExtractor::ClaudeRuleExtractor(
    bool enabled,
    std::string api_key_env_var
)
    : enabled_(enabled),
      api_key_env_var_(std::move(api_key_env_var)) {}

LLMRuleExtractionResult ClaudeRuleExtractor::extract(
    const LLMRuleExtractionRequest&
) {
    LLMRuleExtractionResult result;

    if (!enabled_) {
        result.status = LLMExtractionStatus::Disabled;
        result.diagnostic = "Claude rule extractor disabled";
        return result;
    }

    const char* api_key = std::getenv(api_key_env_var_.c_str());
    if (!api_key || *api_key == '\0') {
        result.status = LLMExtractionStatus::MissingApiKey;
        result.diagnostic = "missing API key environment variable: " +
                            api_key_env_var_;
        return result;
    }

    result.status = LLMExtractionStatus::ProviderError;
    result.diagnostic =
        "Claude rule extractor provider call is not implemented";
    return result;
}

}  // namespace trading_engine::oracle

#pragma once

#include <cstdint>
#include <string>

#ifndef ORACLE_ENABLE_LLM
#define ORACLE_ENABLE_LLM 0
#endif

namespace trading_engine::oracle {

using MarketId = std::string;
using AssetId = std::string;
using VariableId = std::string;
using RuleId = std::string;
using BundleId = std::string;

inline constexpr std::uint32_t kArtifactVersion = 1;
inline constexpr bool kLlmBuildEnabled = ORACLE_ENABLE_LLM != 0;

}  // namespace trading_engine::oracle

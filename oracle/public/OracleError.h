#pragma once

#include <cstdint>

namespace trading_engine::oracle {

enum class OracleErrorCode : std::uint16_t {
    None,
    InvalidInput,
    MissingField,
    UnknownMarket,
    UnknownAsset,
    UnknownVariable,
    UnapprovedRule,
    ContradictoryRules,
    TooManyVariables,
    ArtifactWriteFailed,
    ArtifactReadFailed,
    ChecksumMismatch,
    LlmDisabled,
    MissingApiKey,
    InternalError
};

}  // namespace trading_engine::oracle

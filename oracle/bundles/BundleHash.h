#pragma once

#include "oracle/bundles/CandidateBundle.h"

#include <cstdint>
#include <vector>

namespace trading_engine::oracle {

[[nodiscard]] std::uint64_t hash_candidate_bundle(
    const CandidateBundle& bundle
) noexcept;

[[nodiscard]] std::uint64_t hash_candidate_bundles(
    const std::vector<CandidateBundle>& bundles
) noexcept;

}  // namespace trading_engine::oracle

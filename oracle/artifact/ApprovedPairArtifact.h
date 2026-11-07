#pragma once
#include "engine/state/cross_venue/Types.h"

namespace trading_engine::cross_venue {
// Frozen pair rules/payoff artifact. A hash detects mutation; it is not a
// substitute for independent human review of all settlement contingencies.
std::string pair_hash(const ApprovedPair& pair);
void validate_pair(const ApprovedPair& pair);
}  // namespace trading_engine::cross_venue

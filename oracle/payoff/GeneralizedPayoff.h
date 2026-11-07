#pragma once
#include "oracle/compiler/ConstraintCompiler.h"
#include "oracle/payoff/PayoffMatrixBuilder.h"
#include "engine/state/cross_venue/Types.h"
#include <optional>
namespace trading_engine::oracle {
struct ContractPayoff {
    cross_venue::VenueInstrumentKey instrument;
    std::uint32_t variable_id=0;
    bool yes=true;
    std::string rules_hash;
    std::int64_t minimum_micro=0,step_micro=0;
};
struct ExceptionalPayoff {
    std::string branch,source_hash;
    std::vector<std::int64_t> payout_per_unit;
    bool evidence_verified=false,proven_impossible=false;
    std::string case_id; // Multiple explicit alternatives per exceptional branch.
};
struct PayoffRow {
    std::string id;
    std::uint64_t boolean_mask=0;
    std::vector<std::int64_t> payout_per_unit;
    bool uncertainty_envelope=false;
};
struct PortfolioProof {
    std::vector<PayoffRow> normal_states,all_states;
    std::vector<std::string> missing_states;
    std::uint64_t oracle_matrix_hash=0;
    bool complete=false;
};
struct FloorResult {std::int64_t payout=0;std::size_t witness=0;};
// Existing Oracle constraints/enumerator/matrix supply the normal state space.
// Unverified exception branches remain explicit conservative envelopes.
PortfolioProof enumerate_portfolio(const CompiledConstraintSet&,const std::vector<ContractPayoff>&,
    const std::vector<ExceptionalPayoff>& exceptions={});
FloorResult portfolio_floor(const std::vector<PayoffRow>& states,const std::vector<std::int64_t>& quantity_micro);
const std::vector<std::string>& required_exception_branches();
}

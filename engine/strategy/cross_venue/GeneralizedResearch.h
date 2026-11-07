#pragma once
#include "engine/strategy/cross_venue/LiveResearch.h"
#include "oracle/payoff/GeneralizedPayoff.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
namespace trading_engine::cross_venue::research {
struct PortfolioLeg {Market market;bool yes=true;std::uint32_t variable=0;};
struct Relationship {
    std::string id,type,category,scope;
    std::vector<PortfolioLeg> legs;
    oracle::CompiledConstraintSet proposed,justified;
    oracle::PortfolioProof hypothesis,evidence;
    Object artifact;
    std::string hash,status;
    bool approved=false;
};
struct GeneralizedLimits {
    std::uint64_t max_capital_lock_seconds=14*86400,max_legs=4,max_groups=512,max_per_type=96,max_evaluations=10000,max_runtime_ms=250,max_points=6;
    Money capital=200000*unit,max_unhedged_cost=5000*unit;
    std::array<Money,2> wallets{100000*unit,100000*unit};
    std::int64_t max_quantity_micro=1000*unit;
    Money forward_per_share=2000,conversion_per_share=3000;
};
GeneralizedLimits generalized_limits(const Config&);
Object generalized_discover(const Config&,const std::filesystem::path& metadata,const std::filesystem::path& out);
std::vector<Relationship> read_relationships(const std::filesystem::path&);
Object generalized_replay(const Config&,const std::filesystem::path& relationships,
    const std::filesystem::path& capture,const std::filesystem::path& out);
struct PortfolioQuote {
    bool valid=false,strict_eligible=false,abstain=true;
    std::string reason,optimality,evidence_status;
    std::vector<std::int64_t> quantities,gross_witness_quantities,fee_witness_quantities;
    std::vector<Money> costs,fees,budgets;
    std::vector<std::int64_t> visible_depth;
    std::vector<std::vector<std::int64_t>> quantity_grid;
    std::vector<std::string> liquidity_keys;
    Money max_gross_margin=0,max_fee_margin=0;
    Money nominal_floor=0,conservative_floor=0,gross=0,fee_net=0,net=0,relaxation_upper_bound=0;
    std::array<Money,2> wallet_required{};
    std::uint64_t evaluated=0;
    std::size_t witness=0;
};
PortfolioQuote optimize_portfolio(const Relationship&,const std::vector<VersionedBook>&,
    const std::vector<Market>&,std::uint64_t wall,const GeneralizedLimits&,const std::vector<std::int64_t>& forced_quantities={});
struct QuantityVerification {Object summary;Array witnesses;std::uint64_t elapsed_ns=0;};
QuantityVerification verify_quantity_breakpoints(const Relationship&,const std::vector<VersionedBook>&,const std::vector<Market>&,std::uint64_t,const GeneralizedLimits&,const PortfolioQuote&);
Money passive_bid_at_or_below(const Market&,Money ceiling);
bool approved_generalized_review(const Relationship&,const Config&);
void reserve_generalized(const Relationship&,const PortfolioQuote&,SharedVenueCapital&,
    std::set<std::string>& used_liquidity,const std::string& liquidity_id,std::uint64_t now);
}

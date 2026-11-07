#include "oracle/payoff/GeneralizedPayoff.h"
#include "oracle/enumerate/StateEnumerator.h"
#include "engine/common/math/FixedPointMath.h"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
namespace trading_engine::oracle {
namespace {void ensure(bool b,const char* reason){if(!b)throw std::invalid_argument(reason);}}
const std::vector<std::string>& required_exception_branches(){
    static const std::vector<std::string> names{"TIE","VOID","CANCELLATION","POSTPONEMENT","EARLY_TERMINATION","DISPUTE","REPLACEMENT","FRACTIONAL_REFUND","SOURCE_DIVERGENCE","UNDEFINED"};return names;
}
PortfolioProof enumerate_portfolio(const CompiledConstraintSet& compiled,const std::vector<ContractPayoff>& legs,const std::vector<ExceptionalPayoff>& exceptions){
    ensure(legs.size()>=2 && legs.size()<=4 && !compiled.variables.empty() && compiled.variables.size()<=4,"bounded payoff model requires 2-4 legs and 1-4 event variables");
    for(std::size_t i=0;i<compiled.variables.size();++i)ensure(compiled.variables[i].var_id==i,"noncanonical variable index");
    auto enumeration=StateEnumerator{}.enumerate(compiled);ensure(enumeration.ok() && !enumeration.feasible_states.empty(),"empty or invalid feasible-state model; vacuous guarantee prohibited");
    auto matrix=PayoffMatrixBuilder{}.build(compiled.variables,enumeration.feasible_states);ensure(matrix.ok(),"Oracle payoff matrix invalid");
    PortfolioProof proof;proof.oracle_matrix_hash=matrix.matrix.payoff_hash;
    std::map<std::string,unsigned> columns;for(unsigned i=0;i<matrix.matrix.asset_ids.size();++i)columns[matrix.matrix.asset_ids[i]]=i;
    for(std::size_t row=0;row<enumeration.feasible_states.size();++row){const auto& state=enumeration.feasible_states[row];PayoffRow p{"BINARY_"+std::to_string(state.state_id),state.state_id,{}};
        for(const auto& leg:legs){ensure(leg.variable_id<compiled.variables.size(),"unmapped portfolio variable");const auto col=columns.at(compiled.variables[leg.variable_id].asset_id);const auto value=matrix.matrix.entries.at(row*matrix.matrix.column_count+col).payout_tick;p.payout_per_unit.push_back(leg.yes?value:cross_venue::unit-value);}proof.normal_states.push_back(p);proof.all_states.push_back(p);
    }
    std::set<std::string> seen,cases;
    for(const auto& e:exceptions){ensure(cases.insert(e.branch+":"+e.case_id).second,"duplicate exceptional payoff case");seen.insert(e.branch);const auto case_name=e.branch+(e.case_id.empty()?"":"_"+e.case_id);ensure(std::find(required_exception_branches().begin(),required_exception_branches().end(),e.branch)!=required_exception_branches().end(),"unknown exception branch");
        const bool verified=e.evidence_verified && e.source_hash.size()==64 && std::all_of(e.source_hash.begin(),e.source_hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
        if(!verified){proof.missing_states.push_back(e.branch);proof.all_states.push_back({case_name+"_UNVERIFIED",0,std::vector<std::int64_t>(legs.size(),0),true});continue;}
        if(e.proven_impossible)continue;
        ensure(e.payout_per_unit.size()==legs.size(),"incomplete exception payout vector");for(auto p:e.payout_per_unit)ensure(p>=0 && p<=cross_venue::unit,"unsupported cash payout interval");proof.all_states.push_back({case_name,0,e.payout_per_unit,false});
    }
    for(const auto& name:required_exception_branches())if(!seen.contains(name)){proof.missing_states.push_back(name);proof.all_states.push_back({name+"_UNKNOWN_LOWER_BOUND",0,std::vector<std::int64_t>(legs.size(),0),true});}
    proof.complete=proof.missing_states.empty();return proof;
}
FloorResult portfolio_floor(const std::vector<PayoffRow>& states,const std::vector<std::int64_t>& q){
    ensure(!states.empty() && q.size()>=2 && q.size()<=4,"missing payoff states or legs");for(auto n:q)ensure(n>=0 && n<=cross_venue::max_quantity*cross_venue::unit,"invalid portfolio quantity");
    FloorResult result{std::numeric_limits<std::int64_t>::max(),0};
    for(std::size_t i=0;i<states.size();++i){ensure(states[i].payout_per_unit.size()==q.size(),"payoff dimension mismatch");std::int64_t total=0;
        // Separate contracts/wallets need not net fractional cash dust. Round
        // each nonnegative leg payout DOWN before checked summation.
        for(std::size_t j=0;j<q.size();++j){auto payout=states[i].payout_per_unit[j];ensure(payout>=0 && payout<=cross_venue::unit,"invalid payout");const auto part=static_cast<__int128>(q[j])*payout/cross_venue::unit;ensure(part<=std::numeric_limits<std::int64_t>::max(),"payoff overflow");std::int64_t next=0;ensure(common::math::checked_add_i64(total,static_cast<std::int64_t>(part),&next),"payoff overflow");total=next;}
        if(total<result.payout)result={total,i};
    }return result;
}
}

#include "engine/strategy/cross_venue/GeneralizedResearch.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <set>

namespace trading_engine::cross_venue::research {
namespace {
Array nums(const std::vector<std::int64_t>& v){Array a;for(auto n:v)a.push_back(n);return a;}
std::int64_t depth(const VersionedBook& b){std::int64_t n=0;for(unsigned i=0;i<b.depth.ask_count;++i)n=add(n,b.ask_qty_micro[i]);return n;}
Money total(const std::vector<Money>& v){return std::accumulate(v.begin(),v.end(),Money{});}
// Model-cost lower bound before exchange/account rounding, denominator unit^3.
// All rounding/accumulator reserves are nonnegative over a fresh order. This
// relaxation ignores minimums, increments, wallet limits and execution risk.
__int128 continuous_cost(const VersionedBook& b,std::int64_t q,Money rate,Money budget){
    __int128 n=static_cast<__int128>(q)*budget*unit*unit;
    for(unsigned i=0;i<b.depth.ask_count && q;++i){const auto take=std::min(q,b.ask_qty_micro[i]);const auto p=b.depth.asks[i].price_tick;
        n+=static_cast<__int128>(take)*p*unit*unit;
        n+=static_cast<__int128>(take)*rate*p*(unit-p);q-=take;
    }
    require(q==0,"continuous lower-bound depth shortfall");return n;
}
}
QuantityVerification verify_quantity_breakpoints(const Relationship& r,const std::vector<VersionedBook>& books,const std::vector<Market>& markets,std::uint64_t wall,const GeneralizedLimits& limits,const PortfolioQuote& baseline){
    QuantityVerification result;const auto start=std::chrono::steady_clock::now();const std::uint64_t cap=10000;
    std::set<std::vector<std::int64_t>> tested;std::vector<PortfolioQuote> quotes;PortfolioQuote best=baseline;
    Money gross=baseline.max_gross_margin,fee=baseline.max_fee_margin;std::map<unsigned,Money> active_best;bool timed_out=false,exhaustive=false;
    auto evaluate=[&](const std::vector<std::int64_t>& q){
        if(tested.contains(q))return;if(tested.size()>=cap)return;
        if(std::chrono::steady_clock::now()-start>std::chrono::milliseconds(1000)){timed_out=true;return;}
        tested.insert(q);auto w=optimize_portfolio(r,books,markets,wall,limits,q);if(!w.valid)return;
        const auto active=std::count_if(q.begin(),q.end(),[](auto n){return n>0;});
        if(!active_best.contains(active) || w.net>active_best[active])active_best[active]=w.net;
        gross=std::max(gross,w.gross);fee=std::max(fee,w.fee_net);
        if(w.net>best.net || (w.net==best.net && total(w.costs)<total(best.costs)))best=w;
        quotes.push_back(std::move(w));
    };
    evaluate(baseline.quantities);evaluate(baseline.gross_witness_quantities);evaluate(baseline.fee_witness_quantities);
    bool min_model=books.size()==2,has10=false,has01=false;
    for(const auto& s:r.hypothesis.normal_states){if(s.payout_per_unit.size()!=2){min_model=false;break;}const auto a=s.payout_per_unit[0],b=s.payout_per_unit[1];min_model &= (a==0 || a==unit) && (b==0 || b==unit) && (a || b);has10 |= a==unit && b==0;has01 |= a==0 && b==unit;}
    min_model &= has10 && has01;
    Array breakpoints_json,seeds_json,grids_json;std::string search_kind;std::optional<Money> coupled_upper,coupled_fee_upper,coupled_gross_upper;
    if(min_model){
        search_kind="TWO_LEG_DEPTH_MINIMUM_INCREMENT_FEE_AND_CAPITAL_NEIGHBORS";
        const auto maximum=std::min({depth(books[0]),depth(books[1]),limits.max_quantity_micro});
        std::set<std::int64_t> breaks{0,maximum},seeds{maximum};
        for(unsigned i=0;i<2;++i){std::int64_t n=0;const auto step=markets[i].qty_step_micro>0?markets[i].qty_step_micro:1;const auto first=markets[i].min_qty_micro>0?markets[i].min_qty_micro:unit;
            for(unsigned j=0;j<books[i].depth.ask_count;++j){n=add(n,books[i].ask_qty_micro[j]);if(n<=maximum)breaks.insert(n);}
            for(auto q:baseline.quantity_grid[i])if(q>0 && q<=maximum)seeds.insert(q);
            // Exact small-size increments where known; missing step is only a
            // conditional microshare probe, never a certified exchange quantity.
            for(std::int64_t j=0;j<256 && first<=maximum-j*step;++j)seeds.insert(first+j*step);
        }
        for(auto n:breaks){breakpoints_json.push_back(n);if(n>0)seeds.insert(n);for(unsigned i=0;i<2;++i){const auto step=markets[i].qty_step_micro>0?markets[i].qty_step_micro:1;if(n>step)seeds.insert(n-step);if(n<=maximum-step)seeds.insert(n+step);}}
        auto vector_for=[&](std::int64_t n){std::vector<std::int64_t> q;for(const auto& m:markets){const auto step=m.qty_step_micro>0?m.qty_step_micro:1;const auto minimum=m.min_qty_micro>0?m.min_qty_micro:unit;q.push_back(((std::max(n,minimum)+step-1)/step)*step);}return q;};
        // Feasible capital boundary, using the existing exact VWAP/fee/wallet
        // evaluator. Infeasible vectors never become reported feasible trades.
        std::int64_t lo=0,hi=maximum;while(lo<hi){const auto mid=lo+(hi-lo+1)/2;const auto q=vector_for(mid);const auto w=optimize_portfolio(r,books,markets,wall,limits,q);if(w.valid)lo=mid;else hi=mid-1;}if(lo)seeds.insert(lo);
        // Fee-reserve/rounding neighbors around each meaningful seed. Bounded
        // bisection locates the next continuous-fee 1-cent (K) / 1e-5 (P) step;
        // all exact reported reserves are still recomputed by the old fee code.
        const auto original=seeds;
        for(const auto n:original)for(unsigned i=0;i<2;++i){const auto s=fee_schedule(markets[i]);const auto quantum=s.type=="quadratic_cent_ceiling"?10000:10;const auto d=static_cast<__int128>(unit)*unit*unit;
            auto fee_lower=[&](std::int64_t q){return (continuous_cost(books[i],q,s.rate,0)-continuous_cost(books[i],q,0,0))/d;};
            const auto target=(fee_lower(n)/quantum+1)*quantum;if(fee_lower(maximum)<target)continue;
            std::int64_t a=n,z=maximum;while(a<z){const auto mid=a+(z-a)/2;if(fee_lower(mid)>=target)z=mid;else a=mid+1;}seeds.insert(a);if(a>0)seeds.insert(a-1);
        }
        // A min(q1,q2) floor with nondecreasing acquisition cash is bounded by
        // equal-size continuous costs at every merged depth breakpoint. The
        // relaxed objective is affine between these breakpoints, so endpoint
        // maxima give a sound bound over the entire bounded quantity box.
        const auto denominator=static_cast<__int128>(unit)*unit*unit;__int128 upper=0,fee_upper=0,gross_upper=0;
        for(const auto n:breaks){__int128 cost=0,fee_cost=0,gross_cost=0;for(unsigned i=0;i<2;++i){const auto schedule=fee_schedule(markets[i]);require(schedule.valid && schedule.rate>=0,"fee lower-bound schedule invalid");cost+=continuous_cost(books[i],n,schedule.rate,limits.forward_per_share+(r.scope=="CROSS_VENUE"?limits.conversion_per_share:0));fee_cost+=continuous_cost(books[i],n,schedule.rate,0);gross_cost+=continuous_cost(books[i],n,0,0);}upper=std::max(upper,static_cast<__int128>(n)*denominator-cost);fee_upper=std::max(fee_upper,static_cast<__int128>(n)*denominator-fee_cost);gross_upper=std::max(gross_upper,static_cast<__int128>(n)*denominator-gross_cost);}
        require(upper/denominator<std::numeric_limits<Money>::max(),"quantity upper bound overflow");coupled_upper=static_cast<Money>((upper+denominator-1)/denominator);coupled_fee_upper=static_cast<Money>((fee_upper+denominator-1)/denominator);coupled_gross_upper=static_cast<Money>((gross_upper+denominator-1)/denominator);
        for(const auto n:seeds){if(n<=0 || n>maximum)continue;seeds_json.push_back(n);evaluate(vector_for(n));if(timed_out)break;}
    }else{
        search_kind="SMALL_MULTI_LEG_DISCRETE_DOMAIN_OR_EXISTING_GRID";
        std::vector<std::vector<std::int64_t>> grids=baseline.quantity_grid;bool exact=true;std::uint64_t product=1;
        for(unsigned i=0;i<markets.size();++i){const auto& m=markets[i];const auto maximum=std::min(depth(books[i]),limits.max_quantity_micro);const auto step=m.qty_step_micro;
            if(m.min_qty_micro<=0 || step<=0 || maximum/step>24){exact=false;break;}
            std::vector<std::int64_t> g{0};for(auto n=((m.min_qty_micro+step-1)/step)*step;n<=maximum;n+=step)g.push_back(n);product*=g.size();if(product>cap){exact=false;break;}grids[i]=g;
        }
        if(!exact)grids=baseline.quantity_grid;
        for(const auto& g:grids)grids_json.push_back(nums(g));std::vector<std::int64_t> q(grids.size());
        std::function<void(unsigned)> enumerate=[&](unsigned i){if(timed_out || tested.size()>=cap)return;if(i==q.size()){evaluate(q);return;}for(auto n:grids[i]){q[i]=n;enumerate(i+1);}};enumerate(0);exhaustive=exact && !timed_out && tested.size()<cap;
    }
    result.elapsed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    result.summary={{"search_kind",search_kind},{"normal_floor_is_minimum_of_two_quantities",min_model},{"all_merged_visible_depth_breakpoints_micro",breakpoints_json},{"scalar_quantity_seeds_micro",seeds_json},{"discrete_grid_micro",grids_json},{"tested_quantity_vectors",tested.size()},{"valid_quantity_vectors",quotes.size()},{"quantity_vectors_micro",Array{}},{"exhaustive_valid_discrete_domain",exhaustive},{"timed_out",timed_out},{"runtime_limit_ms",1000},{"maximum_evaluations",cap},{"baseline_best_nonzero_net",decimal(baseline.net)},{"baseline_max_gross",decimal(baseline.max_gross_margin)},{"baseline_max_fee",decimal(baseline.max_fee_margin)},{"best_nonzero_quantities_micro",nums(best.quantities)},{"best_nonzero_net",decimal(best.net)},{"best_zero_or_nonzero_net",decimal(std::max<Money>(0,best.net))},{"max_gross_margin",decimal(gross)},{"max_fee_margin",decimal(fee)},{"new_gross_positive",baseline.max_gross_margin<=0 && gross>0},{"new_fee_positive",baseline.max_fee_margin<=0 && fee>0},{"new_budget_positive",baseline.net<=0 && best.net>0},{"coupled_continuous_upper_bound",coupled_upper?json::value(decimal(*coupled_upper)):json::value(nullptr)},{"coupled_continuous_fee_upper_bound",coupled_fee_upper?json::value(decimal(*coupled_fee_upper)):json::value(nullptr)},{"coupled_continuous_gross_upper_bound",coupled_gross_upper?json::value(decimal(*coupled_gross_upper)):json::value(nullptr)},{"bounded_global_optimality_gap",coupled_upper?json::value(decimal(*coupled_upper-std::max<Money>(0,best.net))):(exhaustive?json::value("0.000000"):json::value(nullptr))},{"bound_scope","Ordinary hypothesis only; retained visible depth and max_quantity box; no contractual or execution guarantee"},{"quantity_constraints_complete",std::all_of(markets.begin(),markets.end(),[](const auto& m){return m.min_qty_micro>0 && m.qty_step_micro>0;})},{"best_two_active_net",active_best.contains(2)?json::value(decimal(active_best[2])):json::value(nullptr)},{"best_three_active_net",active_best.contains(3)?json::value(decimal(active_best[3])):json::value(nullptr)},{"best_four_active_net",active_best.contains(4)?json::value(decimal(active_best[4])):json::value(nullptr)},{"strict_pnl",nullptr}};
    auto& vectors=result.summary.at("quantity_vectors_micro").as_array();for(const auto& q:tested)vectors.push_back(nums(q));
    for(const auto& q:quotes){if(q.gross<=0 && q.fee_net<=0 && q.net!=best.net)continue;result.witnesses.push_back(Object{{"quantities_micro",nums(q.quantities)},{"costs",Array{}},{"fees",Array{}},{"budgets",Array{}},{"nominal_floor",decimal(q.nominal_floor)},{"gross",decimal(q.gross)},{"fee_adjusted",decimal(q.fee_net)},{"budget_adjusted",decimal(q.net)},{"wallet_kalshi",decimal(q.wallet_required[0])},{"wallet_polymarket",decimal(q.wallet_required[1])},{"strict_pnl",nullptr}});auto& o=result.witnesses.back().as_object();for(auto n:q.costs)o.at("costs").as_array().emplace_back(decimal(n));for(auto n:q.fees)o.at("fees").as_array().emplace_back(decimal(n));for(auto n:q.budgets)o.at("budgets").as_array().emplace_back(decimal(n));}
    return result;
}
}

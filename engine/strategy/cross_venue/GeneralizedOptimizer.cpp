#include "engine/strategy/cross_venue/GeneralizedResearch.h"
#include "engine/common/math/FractionalVwap.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <numeric>
#include <iomanip>
#include <sstream>
namespace trading_engine::cross_venue::research {
namespace {
std::int64_t visible(const VersionedBook& b){std::int64_t n=0;for(unsigned i=0;i<b.depth.ask_count;++i)n=add(n,b.ask_qty_micro[i]);return n;}
struct Point {std::int64_t q=0;Money cost=0,fee=0,budget=0;};
std::vector<std::int64_t> grid(const Market& m,const VersionedBook& b,const GeneralizedLimits& limits){
    const auto step=m.qty_step_micro>0?m.qty_step_micro:1;const auto minimum=m.min_qty_micro>0?m.min_qty_micro:unit;
    const auto first=((minimum+step-1)/step)*step,maximum=std::min(visible(b),limits.max_quantity_micro)/step*step;
    std::set<std::int64_t> options{0};if(first>maximum)return {0};options.insert(first);for(auto mult:{2,10,100})if(first<=maximum/mult)options.insert(first*mult);options.insert(maximum);
    std::int64_t accumulated=0;for(unsigned i=0;i<b.depth.ask_count;++i){accumulated=add(accumulated,b.ask_qty_micro[i]);const auto n=std::min(accumulated,maximum)/step*step;if(n>=first)options.insert(n);}
    std::vector<std::int64_t> values{options.begin(),options.end()};if(values.size()>limits.max_points){std::vector<std::int64_t> bounded{0,first};for(std::size_t i=2;i<limits.max_points;++i){const auto index=1+(values.size()-2)*(i-1)/(limits.max_points-2);bounded.push_back(values[index]);}bounded.back()=maximum;std::sort(bounded.begin(),bounded.end());bounded.erase(std::unique(bounded.begin(),bounded.end()),bounded.end());return bounded;}return values;
}
Money ceil_product(std::int64_t q,Money rate){return static_cast<Money>((static_cast<__int128>(q)*rate+unit-1)/unit);}
}
Money passive_bid_at_or_below(const Market& market,Money ceiling){
    if(ceiling<0)return 0;Money best=0;
    if(const auto* ranges=market.raw.if_contains("price_ranges");ranges && ranges->is_array() && !ranges->as_array().empty()){
        auto amount=[](const json::value& value){if(value.is_string())return money(text_value(value));if(value.is_int64())return multiply(value.as_int64(),unit);if(value.is_uint64()){require(value.as_uint64()<=static_cast<std::uint64_t>(std::numeric_limits<Money>::max()/unit),"price grid overflow");return multiply(static_cast<Money>(value.as_uint64()),unit);}std::ostringstream s;s<<std::fixed<<std::setprecision(6)<<value.as_double();return money(s.str());};
        for(const auto& value:ranges->as_array()){const auto& range=value.as_object();const auto lo=amount(range.at("start")),hi=amount(range.at("end")),step=amount(range.at("step"));require(step>0 && lo>=0 && hi<=unit && hi>=lo,"invalid piecewise price grid");if(ceiling<lo)continue;const auto price=lo+(std::min(ceiling,hi)-lo)/step*step;best=std::max(best,price);}return best;
    }
    return market.tick>0?std::min<Money>(ceiling,unit)/market.tick*market.tick:0;
}
PortfolioQuote optimize_portfolio(const Relationship& r,const std::vector<VersionedBook>& books,const std::vector<Market>& markets,std::uint64_t wall,const GeneralizedLimits& limits,const std::vector<std::int64_t>& forced){
    PortfolioQuote best;best.reason="NO_FEASIBLE_DISCRETE_PORTFOLIO";require(r.legs.size()>=2 && r.legs.size()<=4 && books.size()==r.legs.size() && markets.size()==books.size(),"portfolio/book dimensions");
    require(forced.empty() || forced.size()==books.size(),"fixed quantity dimension mismatch");
    if(r.legs.size()>limits.max_legs){best.reason="PORTFOLIO_LEG_LIMIT";return best;}
    std::vector<std::vector<Point>> points;bool quantity_verified=true,assets_verified=true,asset_mismatch=false;std::string asset;std::set<std::string> unique;
    for(std::size_t i=0;i<books.size();++i){const auto& b=books[i];const auto& m=markets[i];const auto key=r.legs[i].yes?m.yes:m.no;
        if(!(b.key==key) || !(key==(r.legs[i].yes?r.legs[i].market.yes:r.legs[i].market.no)) || m.rules_hash!=r.legs[i].market.rules_hash){best.reason="RULE_OR_INSTRUMENT_CHANGED";return best;}
        if(!b.depth.usable_for_depth || b.depth.recovering || b.evidence.seq_gap || b.evidence.stale || b.depth.ask_count==0){best.reason="INVALID_OR_MISSING_DEPTH";return best;}
        require(unique.insert(key.identity()).second,"duplicate instrument would reuse its depth");quantity_verified &= m.min_qty_micro>0 && m.qty_step_micro>0 && m.tick>0;
        assets_verified &= !key.currency.empty() && !key.currency.starts_with("UNKNOWN");if(asset.empty())asset=key.currency;else {asset_mismatch |= !asset.empty() && !key.currency.empty() && !asset.starts_with("UNKNOWN") && !key.currency.starts_with("UNKNOWN") && asset!=key.currency;assets_verified &= asset==key.currency;}
        std::vector<Point> qpoints;try{const auto fee=fee_schedule(m);const auto choices=forced.empty()?grid(m,b,limits):std::vector<std::int64_t>{forced[i]};best.quantity_grid.push_back(choices);for(const auto q:choices){require(q>=0 && q<=limits.max_quantity_micro,"fixed quantity outside bounded search");if(q && m.min_qty_micro>0)require(q>=m.min_qty_micro,"BELOW_MINIMUM_QUANTITY");if(q && m.qty_step_micro>0)require(q%m.qty_step_micro==0,"INVALID_QUANTITY_STEP");if(!q){qpoints.push_back({});continue;}const auto v=common::math::buy_vwap_micro(b,q);if(!v.ok)continue;
            require(m.tick>0,"UNKNOWN_PRICE_TICK");for(unsigned level=0;level<b.depth.ask_count && b.depth.asks[level].price_tick<=v.worst;++level)require(passive_bid_at_or_below(m,b.depth.asks[level].price_tick)==b.depth.asks[level].price_tick,"PRICE_TICK_MISMATCH");
            const auto f=estimate_fee_micro(fee,b,q,wall);const auto budget=ceil_product(q,limits.forward_per_share)+(r.scope=="CROSS_VENUE"?ceil_product(q,limits.conversion_per_share):0);
            qpoints.push_back({q,v.cost,f,budget});
        }}catch(const std::exception& e){best.reason=e.what();return best;}
        if(qpoints.empty() || (forced.empty() && qpoints.size()<2)){best.reason="NO_SUPPORTED_QUANTITY_WITH_VISIBLE_DEPTH";return best;}points.push_back(std::move(qpoints));best.visible_depth.push_back(visible(b));
    }
    const auto start=std::chrono::steady_clock::now();std::vector<std::int64_t> q(books.size());std::vector<Money> costs(q.size()),fees(q.size()),budgets(q.size());std::array<Money,2> wallets{};bool bounded=false,timed_out=false;std::uint64_t evaluated=0;bool stage_initialized=false;Money stage_gross=0,stage_fee=0;std::vector<std::int64_t> gross_witness,fee_witness;
    std::function<void(std::size_t,unsigned,Money,Money,Money)> search=[&](std::size_t i,unsigned active,Money cost,Money fee,Money budget){
        if(evaluated>=limits.max_evaluations){bounded=true;return;}if((evaluated&255)==0 && std::chrono::steady_clock::now()-start>std::chrono::milliseconds(limits.max_runtime_ms)){bounded=true;timed_out=true;return;}
        if(i<points.size()){const auto wallet=books[i].key.venue==VenueId::Kalshi?0:1;for(const auto& p:points[i]){
            const auto amount=add(add(p.cost,p.fee),p.budget);if(amount>limits.wallets[wallet]-wallets[wallet])continue;const auto total=add(add(add(cost,p.cost),add(fee,p.fee)),add(budget,p.budget));if(total>limits.capital || add(add(cost,p.cost),add(fee,p.fee))>limits.max_unhedged_cost)continue;
            q[i]=p.q;costs[i]=p.cost;fees[i]=p.fee;budgets[i]=p.budget;wallets[wallet]+=amount;search(i+1,active+(p.q>0),add(cost,p.cost),add(fee,p.fee),add(budget,p.budget));wallets[wallet]-=amount;if(bounded)return;
        }return;}
        ++evaluated;if(active<2)return;const auto floor=oracle::portfolio_floor(r.hypothesis.normal_states,q),safe=oracle::portfolio_floor(r.evidence.all_states,q);const auto gross=add(floor.payout,-cost),afterfee=add(gross,-fee),net=add(afterfee,-budget);
        if(!stage_initialized || gross>stage_gross){stage_gross=gross;gross_witness=q;}if(!stage_initialized || afterfee>stage_fee){stage_fee=afterfee;fee_witness=q;}stage_initialized=true;
        if(!best.valid || net>best.net || (net==best.net && cost<std::accumulate(best.costs.begin(),best.costs.end(),Money{}))){best.valid=true;best.quantities=q;best.costs=costs;best.fees=fees;best.budgets=budgets;best.nominal_floor=floor.payout;best.conservative_floor=safe.payout;best.gross=gross;best.fee_net=afterfee;best.net=net;best.wallet_required=wallets;best.witness=floor.witness;}
    };search(0,0,0,0,0);best.max_gross_margin=stage_gross;best.max_fee_margin=stage_fee;best.gross_witness_quantities=gross_witness;best.fee_witness_quantities=fee_witness;best.evaluated=evaluated;best.optimality=timed_out?"TIMEOUT_FEASIBLE_INCUMBENT_ONLY":(bounded?"EVALUATION_LIMIT_FEASIBLE_INCUMBENT_ONLY":"EXACT_REPORTED_DISCRETE_GRID_OPTIMUM_GLOBAL_GAP_REMAINS");
    // Sound weak-duality upper bound over the bounded quantity box. Ignore
    // wallet coupling, fees and depth slippage, making this deliberately loose.
    Money upper=std::numeric_limits<Money>::max();for(const auto& state:r.hypothesis.normal_states){Money row=0;for(std::size_t i=0;i<books.size();++i){const auto maxq=std::min(visible(books[i]),limits.max_quantity_micro);const auto gain=state.payout_per_unit[i]-books[i].depth.asks[0].price_tick;const auto part=gain>0?static_cast<Money>((static_cast<__int128>(gain)*maxq+unit-1)/unit):0;row=add(row,part);}upper=std::min(upper,row);}best.relaxation_upper_bound=upper;
    if(best.valid){for(std::size_t i=0;i<books.size();++i)if(best.quantities[i]>0)best.liquidity_keys.push_back(books[i].key.identity());best.abstain=best.net<=0;best.reason="UNAPPROVED_CONDITIONAL_PORTFOLIO";const auto release_bound=get_uint(r.artifact,"capital_release_bound_seconds");best.strict_eligible=release_bound>0 && release_bound<=limits.max_capital_lock_seconds && valid_sha256(get_text(r.artifact,"capital_release_evidence_hash")) && r.approved && r.evidence.complete && quantity_verified && assets_verified && get_bool(r.artifact,"assets_verified") && get_bool(r.artifact,"quantities_verified") && best.conservative_floor>std::accumulate(best.costs.begin(),best.costs.end(),Money{})+std::accumulate(best.fees.begin(),best.fees.end(),Money{})+std::accumulate(best.budgets.begin(),best.budgets.end(),Money{});}
    if(best.valid)best.evidence_status=r.status=="COUNTEREXAMPLE_FOUND"?"COUNTEREXAMPLE_FOUND":(asset_mismatch?"INCOMPATIBLE_ASSETS":(!quantity_verified?"UNSUPPORTED_QUANTITY_CONSTRAINTS":(!r.evidence.complete?"INCOMPLETE_SETTLEMENT_COVERAGE":(!assets_verified?"INSUFFICIENT_EVIDENCE":(r.approved?"PROVEN_AND_APPROVED":"MATHEMATICALLY_VALID_PENDING_RULE_REVIEW")))));
    return best;
}
void reserve_generalized(const Relationship& r,const PortfolioQuote& q,SharedVenueCapital& capital,std::set<std::string>& used,const std::string& liquidity,std::uint64_t now){
    require(r.approved && r.evidence.complete && q.valid && q.strict_eligible && q.net>0,"APPROVED_COMPLETE_CONTRACTUAL_PROOF_REQUIRED");require(!liquidity.empty() && !used.contains(liquidity),"DUPLICATE_LIQUIDITY_OR_INTENT");
    for(const auto& key:q.liquidity_keys)require(!used.contains(key),"OVERLAPPING_INSTRUMENT_LIQUIDITY_HELD_UNTIL_SETTLEMENT");
    for(unsigned i=0;i<2;++i)require(q.wallet_required[i]>=0 && q.wallet_required[i]<=capital.cash[i]-capital.reservations[i].snapshot().total_reserved_cost_tick,"VENUE_WALLET_CONFLICT");
    std::array<risk::RiskReservation,2> reservations;bool reserved=false;
    try{for(unsigned i=0;i<2;++i)if(q.wallet_required[i]){auto& reservation=reservations[i];reservation.reservation_id=reservation.intent_id=compact_hash(r.hash+liquidity+std::to_string(i));reservation.bundle_id=compact_hash(r.id);reservation.idempotency_hash=reservation.intent_id;reservation.reserved_cost_tick=q.wallet_required[i];reservation.reserved_exposure_tick=q.wallet_required[i];reservation.created_ts_ns=now;reservation.expires_at_ns=std::numeric_limits<std::uint64_t>::max();capital.reservations[i].reserve(reservation);reservation.ledger_reserved=true;reserved=true;}}
    catch(...){if(reserved)for(unsigned i=0;i<2;++i)if(reservations[i].ledger_reserved)capital.reservations[i].release(reservations[i]);throw;}
    used.insert(liquidity);for(const auto& key:q.liquidity_keys)used.insert(key); // No release or capital recycling without actual modeled settlement evidence.
}
}

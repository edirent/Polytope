#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/state/cross_venue/JointBookAsOf.h"
#include "engine/common/math/FractionalVwap.h"
#include <algorithm>
#include <fstream>
#include <numeric>
#include "oracle/compiler/BooleanConstraintBuilder.h"

namespace trading_engine::cross_venue::research {
namespace {
using common::math::buy_vwap_micro;
constexpr std::uint64_t ms=1000000;
const char* label="UNAPPROVED_PRICE_DIAGNOSTIC";
struct Frame {std::uint64_t at=0,wall=0;std::array<std::shared_ptr<const VersionedBook>,4> b;std::array<std::uint64_t,4> liveness{};std::array<bool,4> ws{},tradable{};};
struct Candidate {std::string id;Market k,p;std::vector<Frame> frames;std::array<std::shared_ptr<const VersionedBook>,4> latest;};
struct Policy {std::string name;JointBookPolicy p;};
VersionedBook view(const Frame& f,unsigned i){auto b=*f.b[i];b.evidence.last_liveness_ns=f.liveness[i];return b;}
JointBookAsOf join(const Frame& f,unsigned direction,std::uint64_t t,const JointBookPolicy& p) {
    const unsigned k=direction?1:0,poly=direction?2:3;
    return joint_book_asof({view(f,k),view(f,poly)},t,p,{f.ws[k],f.ws[poly]},{f.tradable[k],f.tradable[poly]});
}
void table(const std::filesystem::path& path,const Array& rows) {
    std::set<std::string> names;for(const auto& r:rows)for(const auto& x:r.as_object())names.insert(std::string(x.key()));
    if(names.empty()){write_table(path,{"status","reason","strict_pnl"},Array{Object{{"status","UNAVAILABLE"},{"reason","NO_ELIGIBLE_POSITIVE_OBSERVATIONS_FOR_THIS_OUTPUT"},{"strict_pnl",nullptr}}});return;}
    write_table(path,{names.begin(),names.end()},rows);
}
std::int64_t depth(const VersionedBook& b){std::int64_t q=0;for(unsigned i=0;i<b.depth.ask_count;++i)q=add(q,b.ask_qty_micro[i]);return q;}
Object evidence(const Frame& f,unsigned d,std::uint64_t t) {
    const auto& a=(*f.b[d?1:0]).evidence;const auto& b=(*f.b[d?2:3]).evidence;
    return {{"decision_mono_ns",t},{"kalshi_recv_ns",a.recv_monotonic_ns},{"poly_recv_ns",b.recv_monotonic_ns},
        {"kalshi_authority_ns",a.last_authoritative_snapshot_ns},{"poly_authority_ns",b.last_authoritative_snapshot_ns},
        {"kalshi_last_change_ns",a.last_book_change_ns},{"poly_last_change_ns",b.last_book_change_ns},
        {"kalshi_liveness_ns",f.liveness[d?1:0]},{"poly_liveness_ns",f.liveness[d?2:3]},{"kalshi_sid",a.stream_sid},{"kalshi_seq",a.sequence},{"kalshi_exchange_clock",a.exchange_clock},{"kalshi_snapshot_version",a.snapshot_version},{"poly_snapshot_version",b.snapshot_version},{"kalshi_epoch",a.connection_epoch},{"poly_epoch",b.connection_epoch},
        {"kalshi_raw_hash",a.raw_hash},{"poly_raw_hash",b.raw_hash},{"kalshi_raw_offset",a.raw_log_offset},{"poly_raw_offset",b.raw_log_offset},
        {"kalshi_source_ts_ns",a.source_ts_ns},{"poly_source_ts_ns",b.source_ts_ns},{"kalshi_recv_wall_ns",a.recv_wall_ns},{"poly_recv_wall_ns",b.recv_wall_ns}};
}
struct Cost {bool valid=false;Money k=0,p=0,kfee=0,pfee=0,gross=0,net=0,budget=0;std::int64_t supported=0;std::string reason;};
Cost cost(const Candidate& c,const Frame& f,unsigned d,std::int64_t q) {
    Cost x;const auto& k=(*f.b[d?1:0]);const auto& p=(*f.b[d?2:3]);x.supported=std::min(depth(k),depth(p));
    const auto kv=buy_vwap_micro(k,q),pv=buy_vwap_micro(p,q);
    if(!kv.ok || !pv.ok){x.reason="INSUFFICIENT_VISIBLE_DEPTH";return x;}
    try{x.k=kv.cost;x.p=pv.cost;x.kfee=estimate_fee_micro(fee_schedule(c.k),k,q,f.wall);x.pfee=estimate_fee_micro(fee_schedule(c.p),p,q,f.wall);
        x.gross=q-x.k-x.p;
        // Separate forward deterioration (.002/share) and conditional conversion (.003/share).
        x.budget=static_cast<Money>((static_cast<__int128>(q)*5000+unit-1)/unit);
        x.net=x.gross-x.kfee-x.pfee-x.budget;x.valid=true;
    }catch(const std::exception& e){x.reason=e.what();}return x;
}
std::string state_id(const Frame& f,unsigned d){return std::to_string((*f.b[d?1:0]).evidence.content_hash)+":"+std::to_string((*f.b[d?2:3]).evidence.content_hash);}
Money snap_bid(const Market& m,Money ceiling) {
    Money tick=m.tick;
    if(const auto ranges=m.raw.if_contains("price_ranges");ranges && ranges->is_array())for(const auto& v:ranges->as_array()){
        const auto& r=v.as_object();const auto lo=money(text_value(r.at("start"))),hi=money(text_value(r.at("end")));
        if(ceiling>=lo && ceiling<=hi)tick=money(text_value(r.at("step")));
    }
    return tick>0 && ceiling>0?ceiling/tick*tick:0;
}
}
Object economic_discovery(const std::filesystem::path& directory,const std::filesystem::path& out) {
    require(!std::filesystem::exists(out)||std::filesystem::is_empty(out),"economic output must be new/empty");std::filesystem::create_directories(out);
    const auto manifest=read_object(directory/"capture_manifest.json");auto config=load_config(directory/"capture_config.json");
    require(sha256(json::serialize(config.original))==get_text(manifest,"config_sha256"),"captured config changed");
    require(read_object(directory/"approved_pairs.json").at("pairs").as_array().empty(),"Diagnostic discovery cannot report strict results for approved inputs; use existing strict replay with As-Of config");
    const auto records=read_capture_records(directory,manifest);std::vector<Market> markets;
    const auto selected=json::parse(read_bytes(directory/"selected_markets.json")).as_array();
    for(const auto& v:selected){const auto& o=v.as_object();markets.push_back(normalize_market(get_text(o,"venue"),o.at("raw").as_object()));}
    // Only configured related candidates; never Cartesian-product unrelated selected markets.
    std::vector<Candidate> candidates;Array unavailable;
    for(const auto& v:config.original.at("diagnostic_watch_pairs").as_array()){
        const auto& o=v.as_object();const auto kid=get_text(o,"kalshi_market"),pid=get_text(o,"polymarket_market");
        auto k=std::find_if(markets.begin(),markets.end(),[&](const auto& m){return m.venue=="kalshi" && m.id==kid;});
        auto p=std::find_if(markets.begin(),markets.end(),[&](const auto& m){return m.venue=="polymarket_international" && m.id==pid;});
        if(k==markets.end() || p==markets.end()){unavailable.push_back(Object{{"candidate",get_text(o,"label")},{"status","UNAVAILABLE"},{"reason","NO_DUAL_VENUE_CAPTURE"}});continue;}
        candidates.push_back({get_text(o,"label"),*k,*p,{}});
    }
    const auto begin=get_uint(manifest,"start_monotonic_ns"),end=get_uint(manifest,"end_monotonic_ns");require(end>begin,"invalid window");
    ResearchState state(config,markets,{});std::vector<double> kclock,pclock,processing,callback_delay;std::uint64_t negative_k=0,negative_p=0;
    for(const auto& v:records){const auto& r=v.as_object();state.process(r);const auto at=get_uint(r,"availability_monotonic_ns",get_uint(r,"recv_monotonic_ns"));
        for(auto& c:candidates){Frame f;f.at=at;f.wall=get_uint(r,"recv_wall_ns");unsigned i=0;
            for(const auto& key:{c.k.yes,c.k.no,c.p.yes,c.p.no}){auto b=state.books().read(key);f.liveness[i]=b.evidence.last_liveness_ns;
                const auto& previous=c.latest[i];
                if(!previous || b.evidence.snapshot_version!=previous->evidence.snapshot_version || b.evidence.last_authoritative_snapshot_ns!=previous->evidence.last_authoritative_snapshot_ns || b.depth.usable_for_depth!=previous->depth.usable_for_depth || b.depth.recovering!=previous->depth.recovering || b.evidence.halted!=previous->evidence.halted || b.evidence.seq_gap!=previous->evidence.seq_gap || b.evidence.stale!=previous->evidence.stale)c.latest[i]=std::make_shared<VersionedBook>(std::move(b));
                f.b[i]=c.latest[i];f.ws[i]=state.websocket_book(key);f.tradable[i]=state.market(key).tradable;++i;}
            c.frames.push_back(std::move(f));
        }
        callback_delay.push_back(static_cast<double>(get_uint(r,"recv_monotonic_ns")-get_uint(r,"callback_recv_monotonic_ns",get_uint(r,"recv_monotonic_ns")))/ms);
        processing.push_back(static_cast<double>(at-get_uint(r,"recv_monotonic_ns"))/ms);
    }
    // Book-source ages are sampled only when their raw evidence changes, never interpreted as network latency.
    std::set<std::pair<unsigned,std::string>> seen_clock;
    for(const auto& c:candidates){std::array<std::string,4> last;for(const auto& f:c.frames)for(unsigned i=0;i<4;++i){const auto& e=(*f.b[i]).evidence;if(e.source_ts_ns && e.recv_wall_ns && e.raw_hash!=last[i] && seen_clock.emplace(i<2?0u:1u,e.raw_hash).second){
        const double age=(static_cast<double>(e.recv_wall_ns)-static_cast<double>(e.source_ts_ns))/ms;
        (i<2?kclock:pclock).push_back(age);if(age<0)(i<2?negative_k:negative_p)++;last[i]=e.raw_hash;}}}
    std::vector<Policy> policies{{"legacy_update_100ms",{30000*ms,45000*ms,15000*ms,true,false}},
        {"balanced_asof",{30000*ms,45000*ms,15000*ms,false,false}},
        {"conservative_asof",{15000*ms,10000*ms,5000*ms,false,false}},
        {"permissive_RESEARCH_ONLY",{30000*ms,90000*ms,60000*ms,false,true}}};
    Array coverage,health,prices,episodes,fees,latency,maker,capital,diagnostics,latency_details,maker_sensitivity;
    std::uint64_t observations=0,positive=0,gross_positive=0;Money strongest=std::numeric_limits<Money>::min();
    for(const auto& c:candidates)for(const auto& policy:policies)for(unsigned d=0;d<2;++d){
        std::map<std::string,std::uint64_t> times;std::uint64_t transport=0,reconstructed=0,authority=0,tradable=0,eligible=0;
        std::uint64_t positives_policy=0,priced_policy=0;std::array<std::uint64_t,5> known{},survived{},unknown{},gross_known{},gross_survived{},gross_unknown{};std::uint64_t gross_observations=0;const std::array<unsigned,5> lags{0,100,250,500,1000};
        std::int64_t base=std::max({c.k.min_qty_micro,c.p.min_qty_micro,std::int64_t(unit)});
        const bool quantities_verified=c.k.qty_step_micro>0 && c.p.qty_step_micro>0;
        std::int64_t step=quantities_verified?std::lcm(c.k.qty_step_micro,c.p.qty_step_micro):unit;
        base=((base+step-1)/step)*step;const std::vector<std::int64_t> quantities{base,base*2,base*5,base*10};
        std::map<std::int64_t,std::pair<std::uint64_t,std::uint64_t>> open;std::map<std::int64_t,Money> peak;
        std::string last_content;bool last_eligible=false;
        auto close_episode=[&](std::int64_t q,std::uint64_t t,unsigned kind){const auto key=q*3+kind;if(open.contains(key)){const auto [start,last]=open.at(key);episodes.push_back(Object{{"mode",label},{"candidate",c.id},{"policy",policy.name},{"direction",d},{"quantity",decimal(q)},
            {"basis",std::array<std::string,3>{"GROSS_PARITY","FEE_ADJUSTED","BUDGET_ADJUSTED"}[kind]},{"start_mono_ns",start},{"end_mono_ns",t},{"duration_sec",static_cast<double>(t-start)/1e9},{"positive_observations",last},{"peak_basis_discrepancy",decimal(peak[key])},{"censored_at_capture_end",t==end},{"strict_pnl",nullptr}});open.erase(key);peak.erase(key);}};
        for(std::size_t fi=0;fi<c.frames.size();++fi){const auto& f=c.frames[fi];const auto stop=fi+1<c.frames.size()?c.frames[fi+1].at:end;if(f.at>=end || stop<f.at)continue;
            // Split intervals exactly at authority/liveness expiration, including the inclusive boundary.
            std::vector<std::uint64_t> cuts{std::max(begin,f.at),std::min(stop,end)};
            for(unsigned bi:{d?1u:0u,d?2u:3u}){const auto& e=(*f.b[bi]).evidence;for(auto deadline:{f.liveness[bi]+policy.p.liveness_ns+1,e.last_authoritative_snapshot_ns+policy.p.authority_ns+1,e.last_book_change_ns+policy.p.change_ns+1})if(deadline>cuts[0] && deadline<cuts[1])cuts.push_back(deadline);}
            std::sort(cuts.begin(),cuts.end());
            for(std::size_t ci=0;ci+1<cuts.size();++ci){const auto t=cuts[ci],dt=cuts[ci+1]-t;if(!dt)continue;const auto j=join(f,d,t,policy.p);
                transport+=j.transport_healthy?dt:0;reconstructed+=j.reconstruction_valid?dt:0;authority+=j.source_authority_valid && j.available && j.reconstruction_valid?dt:0;tradable+=j.both_markets_tradable?dt:0;eligible+=j.diagnostic_eligible?dt:0;times[j.reason]+=dt;
                auto h=evidence(f,d,t);h["end_mono_ns"]=cuts[ci+1];h["candidate"]=c.id;h["policy"]=policy.name;h["direction"]=d;h["eligible"]=j.diagnostic_eligible;h["reason"]=j.reason;h["transport_healthy"]=j.transport_healthy;h["reconstruction_valid"]=j.reconstruction_valid;h["authority_valid"]=j.source_authority_valid;h["book_freshness_acceptable"]=j.book_freshness_acceptable;h["markets_tradable"]=j.both_markets_tradable;h["executable_asks_available"]=j.executable_asks_available;h["update_skew_ms"]=static_cast<double>(j.update_skew_ns)/ms;health.push_back(h);
                if(!j.diagnostic_eligible){for(auto q:quantities)for(unsigned kind=0;kind<3;++kind)close_episode(q,t,kind);last_eligible=false;continue;}
                const auto content=state_id(f,d);const bool observation=!last_eligible || content!=last_content;last_content=content;last_eligible=true;
                if(!observation)continue;
                for(auto q:quantities){const auto x=cost(c,f,d,q);auto row=evidence(f,d,t);row["mode"]=label;row["candidate"]=c.id;row["policy"]=policy.name;row["direction"]=d;row["quantity"]=decimal(q);row["quantity_constraints"]=quantities_verified?"VERIFIED_METADATA_STEPS":"UNVERIFIED_STEP_CONDITIONAL_PROBE";
                    row["quantity_minimum"]=decimal(base);row["visible_pair_depth"]=decimal(x.supported);row["status"]=x.valid?"PRICED":x.reason;row["settlement_caveat"]="NO_PAYOFF_PROOF;USD_VS_UNVERIFIED_COLLATERAL;VOID_AND_DEADLINE_UNRESOLVED";
                    if(!x.valid){fees.push_back(row);for(unsigned kind=0;kind<3;++kind)close_episode(q,t,kind);continue;}
                    ++priced_policy;row["kalshi_cost"]=decimal(x.k);row["poly_cost"]=decimal(x.p);row["kalshi_fee"]=decimal(x.kfee);row["poly_fee"]=decimal(x.pfee);row["gross_parity_discrepancy"]=decimal(x.gross);row["fee_adjusted_discrepancy"]=decimal(x.gross-x.kfee-x.pfee);row["forward_and_conversion_budget"]=decimal(x.budget);row["conditional_net_discrepancy"]=decimal(x.net);row["contractual_payout_floor"]=nullptr;row["strict_pnl"]=nullptr;
                    for(unsigned bi=0;bi<4;++bi){const auto book=view(f,bi);const auto quality=joint_book_asof({book,book},t,policy.p,{f.ws[bi],f.ws[bi]},{f.tradable[bi],f.tradable[bi]});auto vwap=buy_vwap_micro(book,q);const auto field=std::array<std::string,4>{"kalshi_yes_cost","kalshi_no_cost","poly_yes_cost","poly_no_cost"}[bi];row[field]=quality.diagnostic_eligible && vwap.ok?json::value(decimal(vwap.cost)):json::value(nullptr);row[field+"_status"]=quality.diagnostic_eligible?(vwap.ok?"QUOTED":"INSUFFICIENT_VISIBLE_DEPTH"):quality.reason;}
                    prices.push_back(row);fees.push_back(row);if(policy.name=="balanced_asof"){++observations;if(x.net>0)++positive;if(x.gross>0)++gross_positive;strongest=std::max(strongest,static_cast<Money>(static_cast<__int128>(x.net)*unit/q));}
                    if(x.net>0)++positives_policy;if(x.gross>0)++gross_observations;
                    for(unsigned kind=0;kind<3;++kind){const auto edge=std::array<Money,3>{x.gross,x.gross-x.kfee-x.pfee,x.net}[kind];const auto key=q*3+kind;if(edge>0){if(!open.contains(key))open[key]={t,0};++open[key].second;peak[key]=std::max(peak[key],edge);}else close_episode(q,t,kind);}
                    // Analyze all distinct observations; survival denominators contain positive observations only.
                    for(unsigned li=0;li<lags.size();++li){const auto exec=t+lags[li]*ms;auto it=std::upper_bound(c.frames.begin(),c.frames.end(),exec,[](auto t,const Frame& f){return t<f.at;});bool valid=exec<=end && it!=c.frames.begin();Cost later;const Frame* future=nullptr;
                        if(valid){--it;future=&*it;valid=join(*it,d,exec,policy.p).diagnostic_eligible;if(valid){later=cost(c,*it,d,q);valid=later.valid;}}
                        if(x.gross>0){if(valid){++gross_known[li];if(later.gross>0)++gross_survived[li];}else ++gross_unknown[li];}
                        if(x.net>0){if(valid){++known[li];if(later.net>0)++survived[li];}else ++unknown[li];}
                        Object lr{{"mode",label},{"candidate",c.id},{"policy",policy.name},{"direction",d},{"quantity",decimal(q)},{"observed_ns",t},{"execution_ns",exec},{"latency_ms",lags[li]},{"original_gross",decimal(x.gross)},{"original_net",decimal(x.net)},{"later_gross",valid?json::value(decimal(later.gross)):json::value(nullptr)},{"outcome",valid?"CONDITIONAL_COST_OBSERVATION":"UNKNOWN_UNAVAILABLE"},{"later_net",valid?json::value(decimal(later.net)):json::value(nullptr)},{"deterioration",valid?json::value(decimal(x.net-later.net)):json::value(nullptr)},{"remaining_depth",valid?json::value(decimal(later.supported)):json::value(nullptr)}};
                        if(valid){lr["later_k_cost"]=decimal(later.k+later.kfee);lr["later_p_cost"]=decimal(later.p+later.pfee);lr["execution_k_recv_ns"]=(*future->b[d?1:0]).evidence.recv_monotonic_ns;lr["execution_p_recv_ns"]=(*future->b[d?2:3]).evidence.recv_monotonic_ns;}latency_details.push_back(lr);
                    }
                    if(policy.name=="balanced_asof"){
                        capital.push_back(Object{{"mode",label},{"candidate",c.id},{"direction",d},{"observed_ns",t},{"quantity",decimal(q)},{"kalshi_wallet",decimal(config.allocations[0])},{"poly_wallet",decimal(config.allocations[1])},{"kalshi_required",decimal(x.k+x.kfee)},{"poly_required",decimal(x.p+x.pfee)},{"conditional_wallet_utilization",static_cast<double>(x.k+x.kfee+x.p+x.pfee)/config.capital},{"visible_pair_depth",decimal(x.supported)},{"capital_day_profit",nullptr},{"allocated_capital",nullptr},{"lockup","Until separate venue settlements; no receipts; no reuse modeled"},{"strict_pnl",nullptr}});
                        for(unsigned venue=0;venue<2;++venue){const auto& local=(*f.b[venue?(d?2:3):(d?1:0)]);const auto& m=venue?c.p:c.k;
                            const bool maker_known=venue?get_bool(c.p.fee,"takerOnly"):get_text(c.k.fee,"series_fee_type")=="quadratic";
                            const auto opposing=venue?x.k+x.kfee:x.p+x.pfee;
                            const Money ceiling=unit-static_cast<Money>((static_cast<__int128>(opposing)*unit+q-1)/q)-10000;
                            const auto ceiling_tick=snap_bid(m,ceiling);const auto bestask=local.depth.asks[0].price_tick,bestbid=local.depth.bids[0].price_tick;
                            const auto quote=snap_bid(m,std::min(ceiling_tick,bestask-std::max<Money>(1,m.tick)));
                            for(Money margin:{Money(0),Money(1000),Money(5000),Money(10000)}){
                                const auto cap=snap_bid(m,unit-static_cast<Money>((static_cast<__int128>(opposing)*unit+q-1)/q)-margin);
                                const auto bid=snap_bid(m,std::min(cap,bestask-std::max<Money>(1,m.tick)));
                                maker_sensitivity.push_back(Object{{"mode","MAKER_QUOTE_FEASIBILITY_ONLY"},{"candidate",c.id},{"direction",d},{"observed_ns",t},{"quantity",decimal(q)},{"maker_venue",m.venue},{"safety_plus_minprofit_per_share",decimal(margin)},{"conditional_quote_ceiling",maker_known?json::value(decimal(cap)):json::value(nullptr)},{"conditional_post_only_quote",maker_known?json::value(decimal(bid)):json::value(nullptr)},{"post_only_price_eligible",maker_known && bid>0 && bid<bestask},{"competitive_at_best_bid",maker_known && local.depth.bid_count && bid>0 && bid>=bestbid && bid<bestask},{"opposing_hedge_cost",decimal(opposing)},{"maker_fee_status",maker_known?"ZERO_FROM_MARKET_SCHEDULE":"UNKNOWN"},{"proven_payout_floor",nullptr},{"order_eligible",false},{"fills",nullptr},{"pnl",nullptr}});
                            }
                            maker.push_back(Object{{"mode","MAKER_QUOTE_FEASIBILITY_ONLY"},{"candidate",c.id},{"direction",d},{"observed_ns",t},{"maker_venue",m.venue},{"quantity",decimal(q)},
                                {"quantity_constraints",quantities_verified?"VERIFIED_METADATA_STEPS":"UNVERIFIED_STEP_CONDITIONAL_PROBE"},{"order_eligible",false},{"proven_payout_floor",nullptr},{"payout_basis","CONDITIONAL_1_PARITY_NOT_PROVEN"},{"maker_fee_status",maker_known?"ZERO_FROM_MARKET_SCHEDULE":"UNKNOWN"},{"conditional_post_only_quote",maker_known?json::value(decimal(quote)):json::value(nullptr)},{"conditional_quote_ceiling",maker_known?json::value(decimal(ceiling_tick)):json::value(nullptr)},
                                {"post_only_eligible",maker_known && quote>0 && quote<bestask},{"competitive_at_best_bid",maker_known && local.depth.bid_count && quote>=bestbid && quote<bestask},{"best_bid",local.depth.bid_count?json::value(decimal(bestbid)):json::value(nullptr)},{"best_ask",decimal(bestask)},{"hedge_depth",decimal(x.supported)},{"opposing_hedge_cost",decimal(opposing)},{"safety_plus_minprofit_per_share","0.010000"},{"worst_unhedged_cost",decimal(static_cast<Money>((static_cast<__int128>(std::max<Money>(quote,0))*q+unit-1)/unit))},{"fills",nullptr},{"queue_position",nullptr},{"rewards",nullptr},{"capacity",nullptr},{"inventory_risk","Unhedged loss up to quote cost; settlement equivalence unproved"}});
                        }
                    }
                }
            }
        }
        for(auto q:quantities)for(unsigned kind=0;kind<3;++kind)close_episode(q,end,kind);
        const auto covered=std::accumulate(times.begin(),times.end(),std::uint64_t{},[](auto n,const auto& p){return n+p.second;});if(covered<end-begin)times["MISSING_OR_FUTURE_EVIDENCE"]+=end-begin-covered;
        Object cr{{"candidate",c.id},{"policy",policy.name},{"direction",d},{"window_sec",static_cast<double>(end-begin)/1e9},{"liveness_limit_ms",policy.p.liveness_ns/ms},{"authority_limit_ms",policy.p.authority_ns/ms},{"change_limit_ms",policy.p.change_ns/ms},{"research_only",policy.p.research_only},
            {"connection_sec",static_cast<double>(transport)/1e9},{"reconstructed_sec",static_cast<double>(reconstructed)/1e9},{"validated_authority_sec",static_cast<double>(authority)/1e9},{"tradable_sec",static_cast<double>(tradable)/1e9},{"eligible_sec",static_cast<double>(eligible)/1e9},{"eligible_pct",100.0*eligible/(end-begin)},{"strict_eligible_sec",nullptr}};
        for(const auto& [reason,time]:times)cr["exclusive_"+reason+"_sec"]=static_cast<double>(time)/1e9;coverage.push_back(cr);
        for(unsigned li=0;li<lags.size();++li)latency.push_back(Object{{"mode",label},{"candidate",c.id},{"policy",policy.name},{"direction",d},{"latency_ms",lags[li]},{"gross_positive_distinct_observations",gross_observations},{"gross_known",gross_known[li]},{"gross_unknown",gross_unknown[li]},{"gross_positive_surviving",gross_survived[li]},{"gross_survival_known_only",gross_known[li]?json::value(static_cast<double>(gross_survived[li])/gross_known[li]):json::value(nullptr)},{"positive_distinct_observations",positives_policy},{"known",known[li]},{"unknown",unknown[li]},{"positive_surviving",survived[li]},{"survival_known_only",known[li]?json::value(static_cast<double>(survived[li])/known[li]):json::value(nullptr)},{"survival_lower_bound",positives_policy?json::value(static_cast<double>(survived[li])/positives_policy):json::value(nullptr)},{"simulated_fills",nullptr},{"status",positives_policy?"CONDITIONAL_NET_SURVIVAL":"NO_NET_POSITIVE_OBSERVATIONS"}});
        diagnostics.push_back(Object{{"mode",label},{"candidate",c.id},{"policy",policy.name},{"direction",d},{"positive_distinct_observations",positives_policy},{"priced_distinct_size_observations",priced_policy},{"eligible_book_sec",static_cast<double>(eligible)/1e9},{"diagnostic_status",priced_policy?"MEASURED_CONDITIONAL_PRICES":(eligible?"NO_SUPPORTED_QUANTITY_OR_FEE_EVIDENCE":"NO_ELIGIBLE_JOINT_BOOKS")},{"human_approval","PENDING"},{"strict_state","NO_APPROVED_STRICT_ARBITRAGE"},{"quantity_constraints",quantities_verified?"VERIFIED_METADATA_STEPS":"UNVERIFIED_STEP_CONDITIONAL_PROBE"},{"kalshi_rules_hash",c.k.rules_hash},{"poly_rules_hash",c.p.rules_hash},{"kalshi_currency",c.k.yes.currency},{"poly_currency",c.p.yes.currency}});
    }
    for(const auto& r:unavailable)diagnostics.push_back(r);
    // Measured quote-state persistence and future hedge-cost drift, never fills.
    std::map<std::string,std::vector<std::uint64_t>> invalid_starts;
    for(const auto& v:health){const auto& h=v.as_object();if(get_text(h,"policy")=="balanced_asof" && !get_bool(h,"eligible"))invalid_starts[get_text(h,"candidate")+":"+std::to_string(get_uint(h,"direction"))].push_back(get_uint(h,"decision_mono_ns"));}
    auto latency_key=[](const Object& row,std::string_view time){return get_text(row,"candidate")+":"+std::to_string(get_uint(row,"direction"))+":"+get_text(row,"quantity")+":"+std::to_string(get_uint(row,time));};
    std::map<std::string,Object> one_second;
    for(const auto& v:latency_details){const auto& l=v.as_object();if(get_text(l,"policy")=="balanced_asof" && get_uint(l,"latency_ms")==1000)one_second[latency_key(l,"observed_ns")]=l;}
    std::map<std::string,std::vector<std::size_t>> quote_groups;
    for(std::size_t i=0;i<maker.size();++i){const auto& q=maker[i].as_object();quote_groups[get_text(q,"candidate")+":"+std::to_string(get_uint(q,"direction"))+":"+get_text(q,"quantity")+":"+get_text(q,"maker_venue")].push_back(i);}
    Array quote_episodes;
    for(const auto& [key,indices]:quote_groups){std::uint64_t epstart=0,epstop=0;std::string epquote;Object ep;
        auto close=[&](){if(epstart){ep["start_mono_ns"]=epstart;ep["end_mono_ns"]=epstop;ep["duration_sec"]=static_cast<double>(epstop-epstart)/1e9;ep["fills"]=nullptr;quote_episodes.push_back(ep);epstart=0;}};
        for(std::size_t j=0;j<indices.size();++j){auto& q=maker[indices[j]].as_object();const auto t=get_uint(q,"observed_ns");auto until=j+1<indices.size()?get_uint(maker[indices[j+1]].as_object(),"observed_ns"):end;
            const auto health_key=get_text(q,"candidate")+":"+std::to_string(get_uint(q,"direction"));const auto& invalid=invalid_starts[health_key];const auto cut=std::lower_bound(invalid.begin(),invalid.end(),t);if(cut!=invalid.end())until=std::min(until,*cut);
            q["quote_state_until_ns"]=until;q["quote_state_duration_sec"]=static_cast<double>(until-t)/1e9;
            const auto l=one_second.find(latency_key(q,"observed_ns"));q["hedge_cost_change_1000ms"]=nullptr;q["hedge_1000ms_status"]="UNKNOWN_UNAVAILABLE";
            if(l!=one_second.end() && get_text(l->second,"outcome")=="CONDITIONAL_COST_OBSERVATION"){
                const auto field=get_text(q,"maker_venue")=="kalshi"?"later_p_cost":"later_k_cost";
                q["hedge_cost_change_1000ms"]=decimal(money(get_text(l->second,field))-money(get_text(q,"opposing_hedge_cost")));q["hedge_1000ms_status"]="MEASURED_FUTURE_BOOK_COST";
            }
            q["previous_ceiling_change"]=nullptr;
            if(j && q.at("conditional_quote_ceiling").is_string() && maker[indices[j-1]].as_object().at("conditional_quote_ceiling").is_string())q["previous_ceiling_change"]=decimal(money(get_text(q,"conditional_quote_ceiling"))-money(get_text(maker[indices[j-1]].as_object(),"conditional_quote_ceiling")));
            const auto quote=get_text(q,"conditional_post_only_quote");if(!get_bool(q,"post_only_eligible") || quote.empty()){close();continue;}
            if(!epstart || epstop!=t || epquote!=quote){close();ep=q;epstart=t;epquote=quote;}epstop=until;
        }close();
    }
    table(out/"maker_quote_episodes.csv",quote_episodes);table(out/"maker_safety_sensitivity.csv",maker_sensitivity);
    table(out/"asof_coverage_comparison.csv",coverage);table(out/"joint_book_health.csv",health);table(out/"price_dislocations.csv",prices);table(out/"opportunity_episodes.csv",episodes);table(out/"fee_depth_analysis.csv",fees);table(out/"latency_sensitivity.csv",latency);table(out/"latency_observations.csv",latency_details);table(out/"maker_quote_feasibility.csv",maker);table(out/"capital_feasibility.csv",capital);table(out/"candidate_pair_diagnostics.csv",diagnostics);
    trading_engine::oracle::BooleanConstraintBuilder oracle;
    auto implication=oracle.implies({"K_YES","P_YES"},{{"K_YES",0},{"P_YES",1}});
    require(implication.ok(),"Oracle draft implication compilation failed");
    write_json(out/"oracle_relationship_drafts.json",Object{{"status","UNVERIFIED_HYPOTHESIS_NOT_AN_APPROVAL"},{"relationship","K_YES implies P_YES, and reverse require independent source and deadline proof"},{"oracle_generated_coefficients",Array{implication.constraint.coeffs[0],implication.constraint.coeffs[1]}},{"operator","LessEqual"},{"rhs",implication.constraint.rhs},{"unrestricted_payoff_states",Array{Array{0,0},Array{0,1},Array{1,0},Array{1,1}}},{"excluded_states",Array{}},{"counterexample_to_complementary_floor","K_YES=0,P_YES=1 makes K_YES+P_NO pay zero; K_YES=1,P_YES=0 makes K_NO+P_YES pay zero"},{"injected_into_strategy",false}});
    auto distribution=[](std::vector<double> v)->Object{if(v.empty())return {{"n",0},{"p50_ms",nullptr},{"p95_ms",nullptr}};std::sort(v.begin(),v.end());return {{"n",v.size()},{"min_ms",v.front()},{"p50_ms",v[(v.size()-1)/2]},{"p95_ms",v[static_cast<std::size_t>((v.size()-1)*.95)]},{"max_ms",v.back()}};};
    write_json(out/"clock_replay_diagnostics.json",Object{{"kalshi_source_to_serialized_receive",distribution(kclock)},{"poly_source_to_serialized_receive",distribution(pclock)},{"kalshi_negative_samples",negative_k},{"poly_negative_samples",negative_p},{"serialized_receive_to_available",distribution(processing)},{"callback_to_serialized_receive",distribution(callback_delay)},{"processing_completion",manifest.contains("processing_timing_file")?json::value("VERIFIED_HASHED_SIDECAR"):json::value(nullptr)},{"one_way_network_latency",nullptr},{"clock_correction_applied",false},{"sequencing","LOCAL_MONOTONIC_AVAILABILITY"}});
    Object summary{{"schema","cross-venue-economic-v0.1.2"},{"source_run",directory.string()},{"source_manifest_sha256",sha256(read_bytes(directory/"capture_manifest.json"))},{"raw_records_verified",records.size()},{"capture_seconds",static_cast<double>(end-begin)/1e9},{"captured_related_candidates",candidates.size()},{"balanced_distinct_priced_observations",observations},{"balanced_gross_positive_observations",gross_positive},{"balanced_conditional_net_positive_observations",positive},{"strongest_conditional_net_per_share",observations?json::value(decimal(strongest)):json::value(nullptr)},{"approved_pairs",0},{"strict_state","NO_APPROVED_STRICT_ARBITRAGE"},{"strict_simulated_pnl",nullptr},{"verified_live_realized_pnl",nullptr},{"live_orders_sent",0},{"recommendation","INSUFFICIENT_EVIDENCE"},{"five_latencies_ms",Array{0,100,250,500,1000}},{"budgets_per_share",Object{{"forward_deterioration","0.002000"},{"conditional_conversion","0.003000"}}},{"quantity_probe_policy","Known minimum rounded to common verified step where available; otherwise explicitly unverified integer micro-share probes, never executable eligibility"}};
    write_json(out/"economic_summary.json",summary);write_json(out/"capture_manifest.json",manifest);return summary;
}
}

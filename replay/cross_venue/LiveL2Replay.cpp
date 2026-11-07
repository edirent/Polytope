#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/common/math/FractionalVwap.h"
#include "engine/execution/state/PairExecutionStateMachine.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include <algorithm>
#include <fstream>
#include <queue>
#include <sstream>

namespace trading_engine::cross_venue::research {
namespace {
const std::vector<std::string> opportunity_columns{"candidate_id","pair_id","lifecycle","evidence_level","observed_mono_ns","qty","gross_edge","fee_budget","future_slippage_budget","execution_risk_budget","net_edge","committed_capital","return_bps","liquidity_id","artifact_hash","fee_version","fee_source_yes","fee_source_no","fee_timestamp_yes","fee_timestamp_no","yes_raw_hash","no_raw_hash","yes_raw_offset","no_raw_offset"};
struct Action {std::uint64_t at=0,ordinal=0;AnalyzedOpportunity candidate;};
struct Later {bool operator()(const Action& a,const Action& b)const{return a.at!=b.at?a.at>b.at:a.ordinal>b.ordinal;}};
Money total_reserved(const SharedVenueCapital& c){return add(c.reservations[0].snapshot().total_reserved_cost_tick,c.reservations[1].snapshot().total_reserved_cost_tick);}
std::string liquidity_key(const VenueInstrumentKey& k,Money price){return k.identity()+field(std::to_string(price));}
std::uint64_t available(const Object& r){return get_uint(r,"availability_monotonic_ns",get_uint(r,"recv_monotonic_ns"));}
void update_policy(Config& c){c.policy.max_liveness_age_ns=c.heartbeat_ms*1'000'000;c.policy.max_authoritative_age_ns=c.authoritative_ms*1'000'000;}

} // namespace
Array read_capture_records(const std::filesystem::path& directory,const Object& manifest){
    Array records;std::set<std::string> expected_files;
    for(const auto& v:manifest.at("raw_inventory").as_array()){
        const auto& f=v.as_object();const auto name=get_text(f,"file");
        require(std::filesystem::path(name).filename()==name && name.find(".raw.")!=std::string::npos,"unsafe raw path");
        const auto path=directory/name;require(sha256(read_bytes(path))==get_text(f,"sha256"),"raw file hash mismatch");expected_files.insert(name);
        std::ifstream in(path,std::ios::binary);std::string line;std::uint64_t offset=0;
        while(std::getline(in,line)){
            auto row=json::parse(line).as_object();require(get_text(row,"raw_file")==name && get_uint(row,"raw_log_offset")==offset,"raw offset mismatch");
            require(sha256(get_text(row,"raw_payload"))==get_text(row,"raw_hash"),"raw payload hash mismatch");
            offset+=line.size()+1;records.push_back(std::move(row));
        }
    }
    for(const auto& e:std::filesystem::directory_iterator(directory))if(e.path().filename().string().find(".raw.")!=std::string::npos)
        require(expected_files.contains(e.path().filename().string()),"unmanifested raw segment");
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return get_uint(a.as_object(),"record_sequence")<get_uint(b.as_object(),"record_sequence");});
    std::uint64_t seq=0,last=0;
    for(const auto& r:records){require(get_uint(r.as_object(),"record_sequence")==++seq,"capture arrival gap/duplicate");
        require(available(r.as_object())>=last,"nonmonotonic availability clock");last=available(r.as_object());}
    if(manifest.contains("processing_timing_file")) {
        const auto name=get_text(manifest,"processing_timing_file");require(name=="processing_timings.jsonl","invalid timing path");
        const auto bytes=read_bytes(directory/name);require(sha256(bytes)==get_text(manifest,"processing_timing_sha256"),"processing timings checksum mismatch");
        std::istringstream in(bytes);std::string line;std::size_t index=0;last=0;
        while(std::getline(in,line)){require(index<records.size(),"extra processing timing");auto timing=json::parse(line).as_object();auto& record=records[index++].as_object();
            require(get_uint(timing,"record_sequence")==get_uint(record,"record_sequence") && get_text(timing,"raw_hash")==get_text(record,"raw_hash"),"timing/raw evidence mismatch");
            const auto completed=get_uint(timing,"processing_completed_monotonic_ns");require(completed>=available(record) && completed>=last,"invalid processing completion");last=completed;
            record["recorded_availability_monotonic_ns"]=available(record);record["availability_monotonic_ns"]=completed;
            record["processing_completed_monotonic_ns"]=completed;
        }require(index==records.size(),"missing processing timing");
    }
    return records;
}
namespace {
struct RunStats {
    std::uint64_t observations=0,survived=0,trades=0,duplicates=0,capital_rejected=0;
    Money costs=0,fees=0,gross_floor=0,net_floor=0,modeled_loss=0,max_residual=0,max_capital=0,drawdown=0,peak=0;
    std::map<std::string,std::uint64_t> rejected;
};
Object stats_row(const RunStats& s,unsigned latency,const std::string& model,Money capital,bool has_approved,bool executable_data){
    const bool measured=has_approved && executable_data;
    return {{"latency_ms",latency},{"execution_model",model},{"valid_opportunities",s.observations},{"survived",s.survived},
        {"survival_rate",s.observations?json::value(static_cast<double>(s.survived)/s.observations):json::value(nullptr)},
        {"simulated_trades",s.trades},{"eligible_capital",decimal(s.max_capital)},{"total_simulated_acquisition_cost",decimal(s.costs)},
        {"simulated_gross_pnl",measured?json::value(decimal(s.gross_floor)):json::value(nullptr)},
        {"fees",measured?json::value(decimal(s.fees)):json::value(nullptr)},{"modeled_execution_loss",measured?json::value(decimal(s.modeled_loss)):json::value(nullptr)},
        {"simulated_net_pnl",measured?json::value(decimal(s.net_floor)):json::value(nullptr)},{"max_residual_exposure",measured?json::value(decimal(s.max_residual)):json::value(nullptr)},
        {"max_capital_committed",decimal(s.max_capital)},{"max_capital_utilization",capital?static_cast<double>(s.max_capital)/capital:0.0},
        {"return_on_committed_capital",s.max_capital?json::value(static_cast<double>(s.net_floor)/s.max_capital):json::value(nullptr)},
        {"turnover",capital?static_cast<double>(s.costs)/capital:0.0},{"max_simulated_floor_drawdown",measured?json::value(decimal(s.drawdown)):json::value(nullptr)},
        {"duplicate_rejections",s.duplicates},{"capital_rejections",s.capital_rejected},{"verified_live_realized_pnl",nullptr}};
}
}

Object replay(const std::filesystem::path& directory,Money capital,const std::vector<unsigned>& latencies,const std::filesystem::path& requested){
    require(capital>0 && !latencies.empty(),"positive capital and latency scenarios required");
    const auto capture=read_object(directory/"capture_manifest.json");
    require(get_text(capture,"schema")=="cross-venue-capture-v0.1","wrong capture schema");
    auto c=load_config(directory/"capture_config.json");update_policy(c);
    require(capital<=c.capital,"requested capital exceeds configured portfolio ceiling");
    require(sha256(json::serialize(c.original))==get_text(capture,"config_sha256"),"captured config changed");
    const auto out=requested.empty()?directory/"analysis":requested;
    require(!std::filesystem::exists(out) || std::filesystem::is_empty(out),"analysis output must be new/empty");std::filesystem::create_directories(out);
    const auto selected=json::parse(read_bytes(directory/"selected_markets.json")).as_array();std::vector<Market> markets;
    for(const auto& v:selected){const auto& o=v.as_object();markets.push_back(normalize_market(get_text(o,"venue"),o.at("raw").as_object()));}
    const auto frozen=read_object(directory/"approved_pairs.json");std::vector<ApprovedPair> pairs;
    for(const auto& p:frozen.at("pairs").as_array())pairs.push_back(parse_pair(p.as_object(),false));
    auto records=read_capture_records(directory,capture);
    const auto end=get_uint(capture,"end_monotonic_ns");
    const bool synthetic=get_text(capture,"evidence_level")=="SYNTHETIC_REGRESSION";
    require(synthetic || get_text(capture,"evidence_level")=="REAL_PUBLIC_API_CAPTURE","unknown input evidence");
    ResearchState audit_state(c,markets,pairs);Array opportunity_rows;std::vector<AnalyzedOpportunity> quoted;
    std::set<std::string> unique_liquidity;std::uint64_t last=0,valid_sync_ns=0,sync_until=0,healthy_sync_observations=0;bool previous_sync=false;
    bool continuous_data=false;
    for(const auto& v:records){const auto& r=v.as_object();const auto time=available(r);
        if(previous_sync && time>=last && sync_until>last)valid_sync_ns+=std::min(time,sync_until)-last;
        audit_state.process(r);last=time;previous_sync=false;
        sync_until=time;
        for(const auto& pair:pairs)try{
            require(audit_state.continuous_pair(pair),"REST_NOT_CONTINUOUS_L2");
            const auto books=audit_state.books().read_pair(pair);validate_books(pair,books,c.policy,time);
            auto until=end;for(const auto& book:books)until=std::min({until,book.evidence.last_liveness_ns+c.heartbeat_ms*1'000'000,
                book.evidence.last_authoritative_snapshot_ns+c.authoritative_ms*1'000'000});
            sync_until=std::max(sync_until,until);previous_sync=true;
        }catch(...){}
        if(previous_sync){++healthy_sync_observations;continuous_data=true;}
        for(auto& candidate:audit_state.scan(time,get_uint(r,"recv_wall_ns"))){
            if(unique_liquidity.insert(candidate.liquidity_id).second){opportunity_rows.push_back(candidate.row);quoted.push_back(candidate);}
        }
    }
    const auto counts=audit_state.counters().at("counts").as_object();
    const auto k_valid=get_uint(counts,"kalshi:valid_book_observation"),p_valid=get_uint(counts,"polymarket_international:valid_book_observation");
    if(previous_sync && sync_until>last)valid_sync_ns+=std::min(end,sync_until)-last;
    Array trades,latency_rows,capital_rows,market_rows,rejection_rows,execution_events;
    const std::vector<std::string> models{"VISIBLE_DEPTH_HYPOTHETICAL","SECOND_LEG_60_PERCENT","SECOND_LEG_FAILS","SECOND_LEG_ADVERSE_4C"};
    for(const auto latency:latencies)for(const auto& model:models){
        RunStats stats;ResearchState state(c,markets,pairs);std::priority_queue<Action,std::vector<Action>,Later> pending;
        auto pool=std::make_shared<SharedVenueCapital>();const auto original_total=add(c.allocations[0],c.allocations[1]);
        require(original_total>0,"empty capital allocation");pool->cash[0]=static_cast<Money>(static_cast<__int128>(capital)*c.allocations[0]/original_total);pool->cash[1]=capital-pool->cash[0];
        std::set<std::string> detected,observed_states,executed_states,survived_states;std::map<std::string,std::int64_t> consumed;
        std::vector<std::unique_ptr<MultiVenueReservationBook>> held_reservations;
        std::map<std::string,Object> by_market;std::uint64_t ordinal=0;Money open_residual=0;
        auto owner=[&](const VenueInstrumentKey& k)->const Market&{return state.market(k);};
        auto execute=[&](Action action){
            const auto& a=action.candidate;const auto q=a.opportunity.intent.bundle_qty;
            Object row{{"observed_candidate_id",a.opportunity.candidate_id},{"pair_id",a.pair.pair_id},{"latency_ms",latency},{"execution_model",model},
                {"evidence_level",synthetic?"SYNTHETIC_REGRESSION":"SIMULATED_FROM_REAL_L2"},{"execution_mono_ns",action.at},
                {"quoted_edge",decimal(a.opportunity.intent.total_edge_tick)},{"lifecycle","REJECTED"},{"verified_live_realized_pnl",nullptr}};
            bool risk_committed=false;
            try{
                require(!executed_states.contains(a.liquidity_id),"DUPLICATE_EXECUTED_LIQUIDITY");
                require(a.continuous_l2,"REST_SNAPSHOT_ONLY_NOT_EXECUTION_BACKTEST");
                require(state.continuous_pair(a.pair),"CONTINUOUS_STREAM_NO_LONGER_VALID");
                require(action.at<=end,"MISSING_FUTURE_COVERAGE");
                const auto future_wall=a.observed_wall_ns+(action.at-a.observed_mono_ns);
                const auto original_books=state.books().read_pair(a.pair);validate_books(a.pair,original_books,c.policy,action.at);
                SynchronizedBookStore execution_store=state.books();
                for(const auto& book:original_books){BookUpdate u;u.key=book.key;u.evidence=book.evidence;u.fractional_quantities=true;
                    u.evidence.authoritative_snapshot=false;
                    for(std::uint16_t i=0;i<book.depth.bid_count;++i)u.bids.emplace_back(book.depth.bids[i].price_tick,book.bid_qty_micro[i]);
                    for(std::uint16_t i=0;i<book.depth.ask_count;++i){const auto p=book.depth.asks[i].price_tick;
                        u.asks.emplace_back(p,std::max<std::int64_t>(0,book.ask_qty_micro[i]-consumed[liquidity_key(book.key,p)]));}
                    require(execution_store.apply(u),"COUNTERFACTUAL_DEPTH_EXHAUSTED");
                }
                const auto books=execution_store.read_pair(a.pair);
                FeeQuote fee=a.fee;fee.valid_from_ns=0;fee.valid_until_ns=action.at+c.policy.max_age_ns+1;
                for(std::size_t i=0;i<2;++i)fee.fees[i]=estimate_fee(fee_schedule(owner(books[i].key)),books[i],q,future_wall);
                const auto& y_market=owner(books[0].key);const auto& n_market=owner(books[1].key);
                fee.version=sha256(field(fee_schedule(y_market).version)+field(fee_schedule(n_market).version)+field(std::to_string(y_market.tick))+
                    field(std::to_string(n_market.tick))+field(std::to_string(y_market.qty_step_micro))+field(std::to_string(n_market.qty_step_micro)));
                auto repriced=PairScanner::quote(a.pair,books,fee,c.policy,action.at);
                if(survived_states.insert(a.liquidity_id).second)++stats.survived;
                row["latency_adjusted_edge"]=decimal(repriced.intent.total_edge_tick);row["execution_candidate_id"]=repriced.candidate_id;
                std::array<std::int64_t,2> quantities{q,q};if(model=="SECOND_LEG_60_PERCENT")quantities[1]=q*3/5;if(model=="SECOND_LEG_FAILS")quantities[1]=0;
                std::array<Money,2> costs{},fees{},prices{};
                for(std::size_t i=0;i<2;++i)if(quantities[i]>0){
                    const auto cost=common::math::buy_vwap_exact(books[i],quantities[i]);require(cost.ok,"INCOMPLETE_FUTURE_DEPTH");
                    prices[i]=(cost.total_cost_tick+quantities[i]-1)/quantities[i];
                    if(i==1 && model=="SECOND_LEG_ADVERSE_4C")prices[i]=std::min<Money>(unit,add(prices[i],40000));
                    costs[i]=multiply(prices[i],quantities[i]);
                    auto executed_book=books[i];executed_book.fractional_quantities=true;executed_book.depth.ask_count=1;
                    executed_book.depth.asks[0].price_tick=prices[i];executed_book.ask_qty_micro[0]=multiply(quantities[i],unit);
                    fees[i]=estimate_fee(fee_schedule(owner(books[i].key)),executed_book,quantities[i],future_wall);
                }
                auto risk=std::make_unique<MultiVenueReservationBook>(a.pair,pool);
                for(std::size_t i=0;i<2;++i)require(add(costs[i],fees[i])<=risk->available_cash(i),"INSUFFICIENT_SHADOW_CAPITAL");
                const auto reservation=risk->reserve(repriced,a.pair,execution_store,fee,c.policy,action.at);
                risk_committed=true;
                std::array<Money,2> account_start{add(risk->available_cash(0),reservation.cash[0]),add(risk->available_cash(1),reservation.cash[1])};
                TwoAccountPaperLedger ledger(a.pair,account_start,future_wall+1);
                PairExecutionStateMachine execution(a.pair,q);execution.reserve(reservation);execution.plan();
                for(std::size_t i=0;i<2;++i){
                    const auto order=repriced.candidate_id+":"+model+":"+std::to_string(i);execution.submitted(i,order);execution.acknowledged(i);
                    if(quantities[i]>0){FillReceipt f;f.key=books[i].key;f.fill_id=order+":fill";f.order_id=order;f.pair_id=a.pair.pair_id;f.canonical_yes=i==0;
                        f.qty=quantities[i];f.price=prices[i];f.fee_paid=fees[i];f.exchange_time_ns=future_wall;f.received_time_ns=future_wall;
                        f.raw_log_offset=books[i].evidence.raw_log_offset;f.raw_hash=books[i].evidence.raw_hash;f.fee_version=fee.version;
                        execution.confirmed_fill(f);ledger.apply(f);
                    }
                    if(quantities[i]<q){execution.cancel_requested(i);execution.cancel_confirmed(i);}
                }
                risk->retain_confirmed_spend(execution);
                if(costs[0]+fees[0]>reservation.cash[0] || costs[1]+fees[1]>reservation.cash[1])execution.freeze("SIMULATED_BUDGET_BREACH");
                else if(execution.state()==PairState::FullyHedged)execution.settlement_pending();else execution.residual_stop();
                const auto audit=ledger.audit();
                for(const auto& stage:execution.trace())execution_events.push_back(Object{{"candidate_id",repriced.candidate_id},{"latency_ms",latency},{"model",model},{"state",stage},{"mode","SIMULATED"}});
                // Conservative counterfactual depletion never assumes unseen
                // replenishment. Same-price liquidity is consumed once per run.
                for(std::size_t i=0;i<2;++i){auto remaining=multiply(quantities[i],unit);
                    for(std::uint16_t level=0;level<books[i].depth.ask_count && remaining>0;++level){
                        auto take=std::min(remaining,books[i].ask_qty_micro[level]);consumed[liquidity_key(books[i].key,books[i].depth.asks[level].price_tick)]+=take;remaining-=take;}}
                const auto spend=add(audit.spend[0],audit.spend[1]);const auto gross=add(std::min(audit.payout_if_yes,audit.payout_if_no),-add(costs[0],costs[1]));
                const auto full_gross=add(multiply(q,unit),-add(repriced.costs[0],repriced.costs[1]));
                stats.costs=add(stats.costs,spend);stats.fees=add(stats.fees,audit.fees);stats.gross_floor=add(stats.gross_floor,full_gross);
                stats.modeled_loss=add(stats.modeled_loss,add(full_gross,-gross));stats.net_floor=add(stats.net_floor,audit.floor_pnl);
                open_residual=add(open_residual,multiply(std::abs(quantities[0]-quantities[1]),unit));
                stats.max_residual=std::max(stats.max_residual,open_residual);
                stats.max_capital=std::max(stats.max_capital,total_reserved(*pool));require(stats.max_capital<=capital,"portfolio ceiling violated");
                stats.peak=std::max(stats.peak,stats.net_floor);stats.drawdown=std::max(stats.drawdown,add(stats.peak,-stats.net_floor));
                ++stats.trades;row["lifecycle"]="SIMULATED";row["reason"]="VISIBLE_DEPTH_IS_NOT_A_CONFIRMED_FILL";
                row["qty_yes"]=quantities[0];row["qty_no"]=quantities[1];row["actual_simulated_cost"]=decimal(spend);row["fees"]=decimal(audit.fees);
                row["hypothetical_contractual_floor_pnl"]=decimal(audit.floor_pnl);row["partial_hedge_worst_case_pnl"]=decimal(audit.floor_pnl);
                row["simulated_gross_floor_pnl"]=decimal(gross);row["max_residual_exposure"]=decimal(multiply(std::abs(quantities[0]-quantities[1]),unit));
                row["capital_lock_lower_bound_sec"]=static_cast<double>(end-action.at)/1e9;row["settlement_evidence"]="NOT_OBSERVED_CAPITAL_REMAINS_LOCKED";
                row["estimated_mark_to_market_pnl"]=nullptr;
                if(books[0].depth.bid_count && books[1].depth.bid_count)
                    row["estimated_mark_to_market_pnl"]=decimal(add(add(multiply(quantities[0],books[0].depth.bids[0].price_tick),multiply(quantities[1],books[1].depth.bids[0].price_tick)),-spend));
                auto& market=by_market[a.pair.pair_id];market["pair_id"]=a.pair.pair_id;market["latency_ms"]=latency;market["execution_model"]=model;
                market["trades"]=get_uint(market,"trades")+1;market["simulated_floor_pnl"]=decimal(add(money(get_text(market,"simulated_floor_pnl","0")),audit.floor_pnl));
                capital_rows.push_back(Object{{"execution_mono_ns",action.at},{"latency_ms",latency},{"execution_model",model},
                    {"kalshi_cash_available",decimal(add(pool->cash[0],-pool->reservations[0].snapshot().total_reserved_cost_tick))},
                    {"polymarket_cash_available",decimal(add(pool->cash[1],-pool->reservations[1].snapshot().total_reserved_cost_tick))},
                    {"reserved_cash",decimal(total_reserved(*pool))},{"ceiling",decimal(capital)},{"settlement_release",false}});
                held_reservations.push_back(std::move(risk));
                executed_states.insert(a.liquidity_id);
            }catch(const std::exception& e){const std::string reason=e.what();
                if(risk_committed)throw std::runtime_error("SIMULATION_INVARIANT_FAILED_AFTER_RESERVATION: "+reason);
                row["reason"]=reason;++stats.rejected[reason];
                if(reason=="MISSING_FUTURE_COVERAGE" || reason=="nonpositive indicative edge" || reason=="AUTHORITATIVE_STATE_TOO_OLD")row["lifecycle"]="EXPIRED";
                if(reason=="DUPLICATE_EXECUTED_LIQUIDITY")++stats.duplicates;
                if(reason.find("capital")!=std::string::npos || reason.find("buying power")!=std::string::npos || reason=="INSUFFICIENT_SHADOW_CAPITAL")++stats.capital_rejected;}
            trades.push_back(std::move(row));
        };
        for(const auto& v:records){const auto& r=v.as_object();const auto time=available(r);
            while(!pending.empty() && pending.top().at<=time){auto a=pending.top();pending.pop();execute(std::move(a));}
            state.process(r);
            for(auto& candidate:state.scan(time,get_uint(r,"recv_wall_ns"))){
                const auto attempt=candidate.liquidity_id+":"+std::to_string(candidate.opportunity.intent.bundle_qty);
                if(!detected.insert(attempt).second){++stats.duplicates;continue;}
                if(observed_states.insert(candidate.liquidity_id).second)++stats.observations;
                pending.push(Action{time+static_cast<std::uint64_t>(latency)*1'000'000,++ordinal,std::move(candidate)});
            }
        }
        while(!pending.empty()){auto a=pending.top();pending.pop();execute(std::move(a));}
        for(const auto& [reason,n]:stats.rejected)rejection_rows.push_back(Object{{"latency_ms",latency},{"execution_model",model},{"reason",reason},{"count",n}});
        for(const auto& [id,row]:by_market)market_rows.push_back(row);
        latency_rows.push_back(stats_row(stats,latency,model,capital,!pairs.empty(),continuous_data));
    }
    const auto capture_stats=audit_state.counters();const auto gross=get_uint(capture_stats,"positive_gross_observations"),net=get_uint(capture_stats,"positive_net_observations");
    std::uint64_t simulated=0;bool positive=false;for(const auto& v:latency_rows){const auto& r=v.as_object();simulated+=get_uint(r,"simulated_trades");
        if(const auto* p=r.if_contains("simulated_net_pnl");p && p->is_string())positive|=money(text_value(*p))>0;}
    const std::string verdict=(synthetic || !k_valid || !p_valid)?"NO_VALID_DATA":pairs.empty()?"NO_APPROVED_PAIRS":!net?"NO_POSITIVE_NET_EDGE":
        !continuous_data?"POSITIVE_QUOTED_EDGE_ONLY":positive?"POSITIVE_SHADOW_OPPORTUNITY":"EXECUTION_EVIDENCE_REQUIRED";
    Object summary{{"schema","cross-venue-research-results-v0.1"},{"verification_status","PARTIALLY_VERIFIED"},{"verdict",verdict},
        {"evidence_level",synthetic?"SYNTHETIC_REGRESSION":"REAL_CAPTURE_REPLAY"},{"execution_evidence","SIMULATED_ONLY_NO_PRIVATE_RECEIPTS"},
        {"market_counts",capture.at("market_counts")},{"candidate_pairs",capture.at("candidate_pairs")},{"approved_pairs",capture.at("approved_pairs")},
        {"valid_kalshi_book_observations",k_valid},{"valid_polymarket_book_observations",p_valid},{"valid_synchronized_coverage_sec",static_cast<double>(valid_sync_ns)/1e9},
        {"healthy_synchronized_observations",healthy_sync_observations},
        {"capture_duration_sec",capture.at("actual_duration_sec")},{"raw_records",records.size()},{"positive_gross_observations",gross},
        {"positive_net_observations",net},{"unique_opportunity_liquidity_states",unique_liquidity.size()},{"simulated_trades_across_scenarios",simulated},
        {"capital_ceiling",decimal(capital)},{"latency_scenarios",latency_rows},{"rejections",capture_stats.at("rejections")},
        {"verified_live_realized_pnl",nullptr},{"live_orders_sent",0},{"opportunity_capacity_upper_bound",quoted.empty()?json::value(nullptr):json::value("VISIBLE_QUOTE_VOLUME_ONLY_SEE_OPPORTUNITIES")},
        {"capital_release_policy","NO_REUSE_BEFORE_OBSERVED_SETTLEMENT; NO_SETTLEMENT_CREDITS_IN_THIS_CAPTURE"}};
    Money upper_cost=0;std::int64_t upper_contracts=0;
    for(const auto& a:quoted){upper_cost=add(upper_cost,add(a.opportunity.costs[0],a.opportunity.costs[1]));upper_contracts=add(upper_contracts,multiply(a.opportunity.intent.bundle_qty,2));}
    summary["opportunity_capacity_upper_bound_contracts"]=pairs.empty()?json::value(nullptr):json::value(upper_contracts);
    summary["opportunity_capacity_upper_bound_acquisition_cost"]=pairs.empty()?json::value(nullptr):json::value(decimal(upper_cost));
    std::uint64_t unknown_fees=0,unknown_sizes=0,unknown_collateral=0;
    for(const auto& m:markets){unknown_fees+=!fee_schedule(m).valid;unknown_sizes+=m.qty_step_micro<=0 || m.min_qty_micro<=0;
        unknown_collateral+=m.yes.currency.starts_with("UNKNOWN");}
    summary["qualification_gates"]=Object{{"selected_unknown_fee_schedules",unknown_fees},{"selected_unverified_quantity_constraints",unknown_sizes},
        {"selected_unknown_collateral_assets",unknown_collateral},{"human_approved_pairs",capture.at("approved_pairs")}};
    write_json(out/"summary.json",summary);write_json(out/"execution_events.simulated.json",execution_events);write_json(out/"rejections.json",rejection_rows);
    write_table(out/"opportunities.csv",opportunity_columns,opportunity_rows);
    const std::vector<std::string> trade_columns{"observed_candidate_id","execution_candidate_id","pair_id","latency_ms","execution_model","evidence_level","execution_mono_ns","lifecycle","reason","quoted_edge","latency_adjusted_edge","qty_yes","qty_no","actual_simulated_cost","fees","simulated_gross_floor_pnl","hypothetical_contractual_floor_pnl","partial_hedge_worst_case_pnl","estimated_mark_to_market_pnl","max_residual_exposure","capital_lock_lower_bound_sec","settlement_evidence","verified_live_realized_pnl"};
    write_table(out/"shadow_trades.csv",trade_columns,trades);write_table(out/"pnl_by_trade.csv",trade_columns,trades);
    write_table(out/"pnl_by_market.csv",{"pair_id","latency_ms","execution_model","trades","simulated_floor_pnl"},market_rows);
    write_table(out/"latency_sensitivity.csv",{"latency_ms","execution_model","valid_opportunities","survived","survival_rate","eligible_capital","total_simulated_acquisition_cost","simulated_trades","simulated_gross_pnl","fees","modeled_execution_loss","simulated_net_pnl","max_residual_exposure","max_capital_committed","max_capital_utilization","return_on_committed_capital","turnover","max_simulated_floor_drawdown","duplicate_rejections","capital_rejections","verified_live_realized_pnl"},latency_rows);
    write_table(out/"capital_utilization.csv",{"execution_mono_ns","latency_ms","execution_model","kalshi_cash_available","polymarket_cash_available","reserved_cash","ceiling","settlement_release"},capital_rows);
    // Keep all required report surfaces together, without reclassifying the
    // immutable raw input directory as an analytical output.
    for(const auto& name:{"market_discovery.csv","candidate_pairs.csv","market_health.csv"})if(std::filesystem::exists(directory/name))std::filesystem::copy_file(directory/name,out/name);
    std::ofstream report(out/"report.md");report.exceptions(std::ios::badbit|std::ios::failbit);
    report<<"# Cross-Venue V0.1 research report\n\nStatus: **PARTIALLY_VERIFIED**. Verdict: **"<<verdict<<"**.\n\n"
        <<"Input evidence: "<<get_text(summary,"evidence_level")<<". No real orders. Verified live realized PnL: **null**.\n\n"
        <<"Capture: "<<get_text(capture,"start_utc")<<" to "<<get_text(capture,"end_utc")<<" ("<<get_text(capture,"actual_duration_sec")<<" seconds).\n\n"
        <<"Markets: "<<json::serialize(capture.at("market_counts"))<<"; proposed pairs: "<<get_text(capture,"candidate_pairs")<<"; human-approved pairs: "<<get_text(capture,"approved_pairs")<<".\n\n"
        <<"Valid Kalshi book observations: "<<k_valid<<"; valid Polymarket observations: "<<p_valid<<"; approved synchronized coverage: "<<static_cast<double>(valid_sync_ns)/1e9<<" seconds.\n\n"
        <<"Positive gross observations: "<<gross<<"; positive net observations: "<<net<<". Only approved, fully specified pairs are scanned; zero here is not evidence that no market-wide spread exists.\n\n"
        <<"|Latency ms|Model|Opportunities|Survived|Simulated trades|Net scenario floor PnL|Max capital|\n|---:|---|---:|---:|---:|---:|---:|\n";
    for(const auto& v:latency_rows){const auto& r=v.as_object();report<<'|'<<get_text(r,"latency_ms")<<'|'<<get_text(r,"execution_model")<<'|'<<get_text(r,"valid_opportunities")<<'|'<<get_text(r,"survived")<<'|'<<get_text(r,"simulated_trades")<<'|'<<get_text(r,"simulated_net_pnl","not measurable")<<'|'<<get_text(r,"max_capital_committed")<<"|\n";}
    report<<"\n## Interpretation and limitations\n\n"
        <<"REST observations are timestamped snapshot samples, not continuous L2 execution evidence. No simulated execution is permitted from REST-only candidates.\n\n"
        <<"All simulated fills are hypothetical visible-depth or explicitly named stress assumptions, never exchange-confirmed fills. Later execution rereads state at arrival time; equal-time future messages are unavailable until processed. Missing future coverage, gaps, fee uncertainty and unhealthy books reject execution.\n\n"
        <<"Capital is separated by venue. The same price-level liquidity is conservatively depleted across simulated trades and never assumes unseen replenishment. Positions and reservations stay locked because settlement receipts are absent; reported lock time is a lower bound through capture end. No annualized forecast is produced.\n\n"
        <<"Cash/USDC/other collateral equivalence is never inferred. Approval, fee and protocol uncertainty fail closed. Full-population discovery is not claimed when API pagination hit the configured sampling bound.\n\n"
        <<"Recorded source/health failures: `"<<json::serialize(summary.at("rejections"))<<"`. See metadata_requests.jsonl and connection_events.jsonl in the parent run directory for exact HTTP/auth/network failures.\n\n"
        <<"The current evidence does **not** establish economically executable or verified profitable cross-venue arbitrage. The next gates are independently reviewed rule/currency/fee/size artifacts, authorized two-venue L2 access, longer healthy captures and private execution/settlement evidence in a separately authorized version.\n";
    report<<"\nQualification gates: `"<<json::serialize(summary.at("qualification_gates"))<<"`. Missing quantity precision may be supplied only through explicitly reviewed source-pinned instrument constraints.\n";
    report.close();
    // Capture and analysis can run different revisions while fixing a parser.
    // Preserve both implementations rather than attributing a new analysis to
    // the older capture's source archive.
    const auto source_root=std::filesystem::path(POLYTOPE_SOURCE_DIR);Array analytical_sources;
    for(const auto& folder:{"engine/state/cross_venue","engine/strategy/cross_venue","engine/decode/parser/kalshi","replay/cross_venue"})
        for(const auto& e:std::filesystem::recursive_directory_iterator(source_root/folder))if(e.is_regular_file() && (e.path().extension()==".cpp" || e.path().extension()==".h")){
            const auto relative=std::filesystem::relative(e.path(),source_root),target=out/"analysis_source_snapshot"/relative;
            std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(e.path(),target);
            analytical_sources.push_back(Object{{"file",relative.string()},{"sha256",sha256(read_bytes(e.path()))}});
        }
    for(const auto& name:{"engine/decode/parser/polymarket/CrossVenueClobNormalizer.cpp","engine/decode/parser/polymarket/CrossVenueClobNormalizer.h", "engine/common/math/FractionalVwap.h",
        "engine/risk/ledger/MultiVenueReservationBook.cpp","engine/risk/ledger/MultiVenueReservationBook.h","engine/paper/ledger/TwoAccountPaperLedger.cpp","engine/paper/ledger/TwoAccountPaperLedger.h",
        "engine/execution/state/PairExecutionStateMachine.cpp","engine/execution/state/PairExecutionStateMachine.h","oracle/artifact/ApprovedPairArtifact.cpp"}){
        const auto source=source_root/name,target=out/"analysis_source_snapshot"/name;std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(source,target);
        analytical_sources.push_back(Object{{"file",name},{"sha256",sha256(read_bytes(source))}});
    }
    std::sort(analytical_sources.begin(),analytical_sources.end(),[](const auto& a,const auto& b){return get_text(a.as_object(),"file")<get_text(b.as_object(),"file");});
    write_json(out/"analysis_source_manifest.json",Object{{"git_revision",POLYTOPE_GIT_REVISION},{"files",analytical_sources}});
    Array inputs,outputs;
    for(const auto& name:{"capture_manifest.json","capture_config.json","selected_markets.json","market_metadata.json","approved_pairs.json","source_manifest.json","fee_models.frozen.json","fee_source_audit.json"})if(std::filesystem::exists(directory/name))inputs.push_back(Object{{"file",name},{"sha256",sha256(read_bytes(directory/name))}});
    for(const auto& e:std::filesystem::directory_iterator(out))if(e.is_regular_file())outputs.push_back(Object{{"file",e.path().filename().string()},{"sha256",sha256(read_bytes(e.path()))}});
    std::sort(outputs.begin(),outputs.end(),[](const auto& a,const auto& b){return get_text(a.as_object(),"file")<get_text(b.as_object(),"file");});
    write_json(out/"audit_manifest.json",Object{{"schema","cross-venue-audit-v0.1"},{"deterministic_inputs",inputs},{"deterministic_outputs",outputs},
        {"raw_inventory",capture.at("raw_inventory")},{"capital",decimal(capital)},{"software_revision",POLYTOPE_GIT_REVISION},{"verified_live_realized_pnl",nullptr}});
    return summary;
}
}  // namespace trading_engine::cross_venue::research

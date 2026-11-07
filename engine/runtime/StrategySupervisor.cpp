#include "engine/runtime/StrategySupervisor.h"
#include "engine/runtime/DurableJournal.h"
#include "engine/runtime/StrategyArtifacts.h"
#include "engine/state/cross_venue/Types.h"
#include <algorithm>
#include <thread>
#include <unistd.h>

namespace trading_engine::runtime {
namespace j=boost::json;namespace mm=strategy::market_making;namespace ex=execution;
namespace {
std::string text(const j::object& o,const char* k,std::string d={}) {auto p=o.if_contains(k);return p?std::string(p->as_string()):d;}
std::int64_t number(const j::object& o,const char* k,std::int64_t d=0) {auto p=o.if_contains(k);return p?p->to_number<std::int64_t>():d;}
bool boolean(const j::object& o,const char* k,bool d=false) {auto p=o.if_contains(k);return p?p->as_bool():d;}
j::object meta(const StrategyRequestMetadata& m) {return {{"strategy_id",m.strategy_id},{"strategy_instance_id",m.strategy_instance_id},
    {"configuration_epoch",m.configuration_epoch},{"model_version",m.model_version},{"market_state_version",m.market_state_version},
    {"request_id",m.request_id},{"idempotency_key",m.idempotency_key},{"source_timestamp",m.source_timestamp},{"availability_ns",m.availability_ns}};}
}
StrategySupervisor::StrategySupervisor(const j::object& config,risk::RiskEngine& risk,ex::ExecutionGateway& oms,paper::PaperTradingEngine& paper,
    Snapshot snapshot,Emit emit,LatencyMetrics& timing,bool shadow):risk_(risk),oms_(oms),paper_(paper),snapshot_(std::move(snapshot)),emit_(std::move(emit)),timing_(timing),shadow_(shadow) {
    test_faults_=boolean(config,"allow_test_faults");
    record_frames_=boolean(config,"record_strategy_frames");
    auto rounding=text(config,"maker_tick_policy","REJECT");if(rounding!="REJECT" && rounding!="PASSIVE_OUTWARD")throw std::runtime_error("unknown execution tick policy");round_ticks_=rounding=="PASSIVE_OUTWARD";
    limits_.total_cost_tick=number(config,"max_total_exposure_tick");limits_.event_cost_tick=number(config,"max_market_exposure_tick");
    limits_.asset_lots=number(config,"max_asset_inventory_lots");limits_.order_cost_tick=number(config,"max_order_cost_tick");
    limits_.safety_cash_tick=number(config,"safety_cash_tick");limits_.max_open_orders=number(config,"max_open_orders",128);
    limits_.venue_budget["polymarket_international"]=number(config,"max_venue_exposure_tick",limits_.total_cost_tick);
    limits_.account_budget["PAPER"]=number(config,"max_account_exposure_tick",limits_.total_cost_tick);
    if(limits_.safety_cash_tick<0 || limits_.max_open_orders==0 || limits_.max_open_orders>1000)throw std::runtime_error("invalid portfolio limits");
    for(const auto& value:config.at("instruments").as_array()) {
        const auto& r=value.as_object();Instrument i;i.asset=text(r,"asset_id");i.market=text(r,"market_id");i.venue=text(r,"venue");i.account=text(r,"account","PAPER");
        i.currency=text(r,"settlement_asset");i.asset_index=number(r,"asset_index");i.market_index=number(r,"market_index");i.tick=number(r,"tick_size_tick",1);i.step=number(r,"quantity_step_lots",1);i.min_qty=number(r,"min_qty_lots",1);
        if(i.asset.empty() || i.market.empty() || i.venue!="polymarket_international" || i.account!="PAPER" || i.currency!="USDC" || i.tick<=0 || i.tick>=1'000'000 || i.step<=0 || i.min_qty<=0)
            throw std::runtime_error("V021 requires explicit single-wallet Polymarket paper instruments");
        for(const auto& [asset,old]:instruments_)if(i.asset_index==old.asset_index)throw std::runtime_error("ambiguous instrument index");
        if(!instruments_.emplace(i.asset,i).second)throw std::runtime_error("duplicate instrument");paper_.bind_asset_index(i.asset,i.asset_index);
    }
    if(instruments_.empty() || instruments_.size()>16)throw std::runtime_error("bounded instrument universe required");
    const auto& sim=config.at("maker_execution").as_object();auto model=text(sim,"model");
    if(model=="optimistic")simulation_.model=ex::ManagedMakerModel::Optimistic;
    else if(model=="conservative")simulation_.model=ex::ManagedMakerModel::Conservative;
    else if(model=="queue_aware")simulation_.model=ex::ManagedMakerModel::QueueAware;
    else if(model=="adverse_selection")simulation_.model=ex::ManagedMakerModel::AdverseSelection;
    else throw std::runtime_error("unknown maker execution model");
    simulation_.submit_latency_ns=number(sim,"submit_latency_ns",50'000'000);simulation_.cancel_latency_ns=number(sim,"cancel_latency_ns",50'000'000);
    simulation_.ack_timeout_ns=number(sim,"ack_timeout_ns",1'000'000'000);simulation_.queue_min_rest_ns=number(sim,"queue_min_rest_ns",250'000'000);
    simulation_.fee_per_lot_tick=number(sim,"fee_per_lot_tick");
    if(simulation_.fee_per_lot_tick<0 || simulation_.fee_per_lot_tick>1000000 || simulation_.submit_latency_ns>60'000'000'000ULL || simulation_.cancel_latency_ns>60'000'000'000ULL || simulation_.ack_timeout_ns>120'000'000'000ULL)throw std::runtime_error("invalid paper execution bounds");
    quote_policy_.max_quote_qty_lots=limits_.asset_lots;quote_policy_.max_quote_notional_tick=limits_.order_cost_tick;
    quote_policy_.max_asset_inventory_lots=limits_.asset_lots;quote_policy_.max_book_age_ns=number(config,"max_book_age_ns");quote_policy_.max_quote_age_ns=quote_policy_.max_book_age_ns;
    quote_policy_.min_replace_interval_ns=0;quote_policy_.max_active_quotes_per_asset=limits_.max_open_orders;
    for(const auto& value:config.at("strategies").as_array()) {
        const auto& r=value.as_object();Instance inst;inst.identity.strategy_id=text(r,"strategy_id");inst.identity.strategy_instance_id=text(r,"strategy_instance_id");
        inst.identity.configuration_epoch=number(r,"configuration_epoch");inst.identity.model_version=sha256(j::serialize(r.at("model")));
        inst.priority=number(r,"priority");inst.budget_ns=number(r,"callback_budget_ns",50'000'000);inst.enabled=boolean(r,"enabled",true);
        inst.manual_test_requests=boolean(r,"manual_test_requests");if(inst.manual_test_requests && !test_faults_)throw std::runtime_error("manual producer is test-only");
        if(inst.identity.strategy_instance_id.empty() || !inst.identity.configuration_epoch || inst.budget_ns==0)throw std::runtime_error("strategy identity and budget required");
        for(const auto& x:instances_)if(x.identity.strategy_instance_id==inst.identity.strategy_instance_id)throw std::runtime_error("duplicate strategy instance");
        limits_.strategy_budget[inst.identity.strategy_instance_id]=number(r,"budget_tick");if(limits_.strategy_budget.at(inst.identity.strategy_instance_id)<=0)throw std::runtime_error("strategy budget required");
        for(const auto& asset:r.at("assets").as_array()) {auto a=std::string(asset.as_string());if(!instruments_.contains(a))throw std::runtime_error("strategy subscribed to unknown asset");inst.assets.push_back(a);}
        if(inst.assets.empty())throw std::runtime_error("empty strategy subscription");
        if(inst.identity.strategy_id=="market_making") {
            const auto& m=r.at("model").as_object();auto cfg=maker_model_config(m,inst.identity.strategy_instance_id,inst.identity.model_version,number(config,"max_book_age_ns"));
            if(cfg.base_quote_size_lots<=0 || cfg.base_quote_size_lots>limits_.asset_lots || cfg.min_inventory_lots<0 || cfg.max_inventory_lots>limits_.asset_lots || cfg.quote_ttl_ns==0 || cfg.quote_ttl_ns>60'000'000'000ULL)throw std::runtime_error("invalid maker model bounds");
            auto policy=text(r,"replacement_policy","CANCEL_THEN_NEW");
            if(policy=="OVERLAP_WITH_EXTRA_RESERVATION")inst.replacement=ex::MakerReplacementPolicy::OverlapWithExtraReservation;
            else if(policy=="BLOCK_REPLACEMENT")inst.replacement=ex::MakerReplacementPolicy::BlockReplacement;
            else if(policy!="CANCEL_THEN_NEW")throw std::runtime_error("unknown replacement policy");
            inst.maker=std::make_unique<MakerPlugin>(cfg,inst.identity);
        } else if(inst.identity.strategy_id!="combinatorial")throw std::runtime_error("strategy plugin unavailable");
        instances_.push_back(std::move(inst));
    }
    if(instances_.empty() || instances_.size()>8)throw std::runtime_error("bounded strategies required");
    std::sort(instances_.begin(),instances_.end(),[](const auto& a,const auto& b){return std::tie(a.priority,a.identity.strategy_instance_id)<std::tie(b.priority,b.identity.strategy_instance_id);});
    oms_.configure_maker(&adapter_,simulation_,[this](const auto& action,const auto& order){before_effect(action,order);},[this](const auto& order,const auto& report){on_report(order,report);});
}
StrategySupervisor::Instance& StrategySupervisor::instance(const std::string& id) {for(auto& i:instances_)if(i.identity.strategy_instance_id==id)return i;throw std::runtime_error("unknown strategy owner");}
risk::PortfolioLeg StrategySupervisor::resource(const std::string& asset,bool buy,std::int64_t qty,std::int64_t price) const {
    const auto& i=instruments_.at(asset);return {i.asset,i.market,i.venue+":"+i.market,i.venue,i.account,i.currency,buy,qty,price,simulation_.fee_per_lot_tick};
}
risk::PortfolioAccountView StrategySupervisor::account_view() const {
    risk::PortfolioAccountView result;result.cash_tick=paper_.ledger().cash_ledger().cash_tick;
    for(const auto& [asset,p]:paper_.ledger().position_ledger().positions()) {
        const auto& i=instruments_.at(asset);result.holdings[asset]={p.qty_lots,p.cost_basis_tick,i.venue+":"+i.market,i.venue,i.account};
        // Attribute canonical remaining cost proportionally; attribution is a projection, never tradable inventory.
        if(p.qty_lots>0)for(const auto& [owner,lots]:attributed_lots_) {auto it=lots.find(asset);if(it!=lots.end())result.strategy_inventory_cost[owner]+=p.cost_basis_tick*it->second/p.qty_lots;}
    }
    return result;
}
void StrategySupervisor::reject(const StrategyRequest& r,const std::string& reason) {
    emit_({{"type","PORTFOLIO_REJECT"},{"request",meta(r.metadata)},{"reason",reason}});
    if(auto q=std::get_if<mm::QuoteIntent>(&r.payload))if(instance(r.metadata.strategy_instance_id).maker) {
        instance(r.metadata.strategy_instance_id).maker->rejected(q->asset_index);emit_({{"type","STRATEGY_DESIRED_RESET"},{"strategy_instance_id",r.metadata.strategy_instance_id},{"asset_index",q->asset_index}});
    }
}
void StrategySupervisor::request(const StrategyRequest& r,const signal::SignalEvidenceView* evidence,const IntentSink* combination_sink) {
    auto& owner=instance(r.metadata.strategy_instance_id);
    if(owner.identity.configuration_epoch!=r.metadata.configuration_epoch || owner.identity.model_version!=r.metadata.model_version || owner.identity.strategy_id!=r.metadata.strategy_id)throw std::runtime_error("unified request provenance mismatch");
    std::string payload;std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,mm::QuoteIntent>)payload="Q:"+std::to_string(mm::compute_quote_intent_hash(v));
        else if constexpr(std::is_same_v<T,mm::CancelQuoteIntent>)payload="C:"+std::to_string(mm::compute_cancel_quote_intent_hash(v));
        else if constexpr(std::is_same_v<T,signal::OpportunityIntent>)payload="B:"+std::to_string(v.idempotency_hash)+":"+std::to_string(v.snapshot_version_hash)+":"+std::to_string(static_cast<int>(v.status));
        else payload="unsupported";},r.payload);
    const auto key=r.metadata.strategy_instance_id+":"+std::to_string(r.metadata.configuration_epoch)+":"+r.metadata.idempotency_key;
    auto existing=request_keys_.find(key);if(existing!=request_keys_.end()) {
        if(existing->second!=payload)stop("REQUEST_IDENTITY_CONFLICT");
        emit_({{"type","DUPLICATE_STRATEGY_REQUEST"},{"request",meta(r.metadata)}});return;
    }
    if(request_keys_.size()>100000)throw std::runtime_error("request identity budget exhausted");request_keys_[key]=payload;
    j::object native;if(auto q=std::get_if<mm::QuoteIntent>(&r.payload))native=quote_payload_json(*q);
    else if(auto cancel=std::get_if<mm::CancelQuoteIntent>(&r.payload))native={{"cancel_intent_id",cancel->cancel_intent_id},{"quote_group_id",cancel->quote_group_id},{"active_quote_id",cancel->active_quote_id},{"asset_index",cancel->asset_index},{"created_ts_ns",cancel->created_ts_ns},{"idempotency_hash",cancel->idempotency_hash},{"reason",static_cast<int>(cancel->reason)}};
    else if(auto intent=std::get_if<signal::OpportunityIntent>(&r.payload)) {
        native={{"intent_id",intent->intent_id},{"idempotency_hash",intent->idempotency_hash},{"bundle_id",intent->bundle_id},{"oracle_artifact_hash",intent->oracle_artifact_hash},{"proof_hash",intent->proof_hash},
            {"snapshot_version_hash",intent->snapshot_version_hash},{"status",static_cast<int>(intent->status)},{"estimated_cost_tick",intent->estimated_cost_tick},{"guaranteed_payout_tick",intent->guaranteed_payout_tick},
            {"created_ts_ns",intent->created_ts_ns},{"expires_at_ns",intent->expires_at_ns}};
    }
    emit_({{"type","UNIFIED_STRATEGY_REQUEST"},{"request",meta(r.metadata)},{"request_type",payload.substr(0,1)},{"native_payload_hash",payload},{"native_payload",native}});
    if(std::holds_alternative<mm::CancelQuoteIntent>(r.payload))cancel(r);
    else if(std::holds_alternative<mm::QuoteIntent>(r.payload))quote(r);
    else if(auto intent=std::get_if<signal::OpportunityIntent>(&r.payload)) {
        if(owner.maker || !evidence || !combination_sink)throw std::runtime_error("combinatorial request lacks typed evidence");
        active_metadata=r.metadata;(*combination_sink)(*intent,*evidence);
    }
    else reject(r,"UNSUPPORTED_REQUEST_TYPE");
}
void StrategySupervisor::cancel(const StrategyRequest& r) {
    const auto& cancel=std::get<mm::CancelQuoteIntent>(r.payload);
    for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==r.metadata.strategy_instance_id && o.quote.quote_group_id==cancel.quote_group_id && !ex::maker_terminal(o.status))oms_.cancel_maker(id,now_);
}
void StrategySupervisor::quote(const StrategyRequest& r) {
    const auto request_started=monotonic_ns();
    auto& inst=instance(r.metadata.strategy_instance_id);auto q=std::get<mm::QuoteIntent>(r.payload);
    if(std::find(inst.assets.begin(),inst.assets.end(),q.asset_id)==inst.assets.end()) {reject(r,"UNSUBSCRIBED_ASSET");return;}
    if(frozen_ || !inst.enabled || inst.lifecycle!="RUNNING") {reject(r,"PORTFOLIO_OR_STRATEGY_FROZEN");return;}
    const auto key=r.metadata.strategy_instance_id+":"+q.asset_id;
    bool old_order=false;
    for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==r.metadata.strategy_instance_id && o.resource.asset==q.asset_id && !ex::maker_terminal(o.status))old_order=true;
    if(old_order && inst.replacement!=ex::MakerReplacementPolicy::OverlapWithExtraReservation) {
        if(inst.replacement==ex::MakerReplacementPolicy::BlockReplacement) {reject(r,"BLOCK_REPLACEMENT");return;}
        replacements_[key]=r;for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==r.metadata.strategy_instance_id && o.resource.asset==q.asset_id && !ex::maker_terminal(o.status))oms_.cancel_maker(id,now_);
        emit_({{"type","MAKER_REPLACE_WAIT"},{"request",meta(r.metadata)},{"asset_id",q.asset_id}});return;
    }
    auto book=snapshot_(q.asset_id);const auto& instrument=instruments_.at(q.asset_id);
    if(!book.ok || !book.value.usable_for_depth || book.value.recovering || book.value.closed || now_<book.value.last_book_update_ns || now_-book.value.last_book_update_ns>quote_policy_.max_book_age_ns || q.expires_at_ns<=now_) {reject(r,"BOOK_OR_QUOTE_NOT_ADMITTED");return;}
    auto depth=state::market_depth_view_from_snapshot(book.value,q.asset_index);
    if(r.metadata.market_state_version!=depth.snapshot_version_hash) {reject(r,"DESIRED_QUOTE_STATE_CHANGED");return;}
    auto view=account_view();risk::QuoteRiskInput input;input.quote=&q;input.depth=&depth;input.policy=&quote_policy_;input.now_ns=now_;
    input.current_position_lots=view.holdings[q.asset_id].lots;
    std::vector<risk::PortfolioLeg> legs;
    for(bool buy:{true,false})if(buy?q.has_bid:q.has_ask) {
        auto& leg=buy?q.bid:q.ask;
        if(round_ticks_ && leg.price_tick%instrument.tick) {
            auto original=leg.price_tick;leg.price_tick=buy?original/instrument.tick*instrument.tick:(original+instrument.tick-1)/instrument.tick*instrument.tick;
            leg.edge_to_fair_tick=buy?leg.fair_value_tick-leg.price_tick:leg.price_tick-leg.fair_value_tick;
            emit_({{"type","MAKER_QUOTE_PRICE_ROUNDED"},{"request",meta(r.metadata)},{"original_tick",original},{"effective_tick",leg.price_tick},{"policy","PASSIVE_OUTWARD_V1"},{"buy",buy}});
        }
        if(leg.price_tick%instrument.tick || leg.quantity_lots%instrument.step || leg.quantity_lots<instrument.min_qty ||
            (buy?(depth.ask_count && leg.price_tick>=depth.asks[0].price_tick):(depth.bid_count && leg.price_tick<=depth.bids[0].price_tick))) {reject(r,"POST_ONLY_TICK_QUANTITY_OR_CROSS");return;}
        for(const auto& [id,o]:oms_.maker_orders())if(!ex::maker_terminal(o.status) && o.resource.asset==q.asset_id && o.resource.buy!=buy &&
            (buy?leg.price_tick>=o.resource.price_tick:leg.price_tick<=o.resource.price_tick)) {reject(r,"OWN_ORDER_CROSS");return;}
        legs.push_back(resource(q.asset_id,buy,leg.quantity_lots,leg.price_tick));
    }
    auto local=quote_risk_.evaluate(input);if(!local.approved_quote) {reject(r,"QUOTE_RISK:"+local.decision.reason);return;}
    auto check=risk_.check_portfolio(r.metadata.strategy_instance_id,legs,view,limits_,frozen_);if(!check.ok) {reject(r,check.reason);return;}
    timing_.observe("maker_request_to_risk_admission",monotonic_ns()-request_started);
    for(const auto& leg:legs) {
        const auto started=monotonic_ns();auto reservation=risk_.reserve_portfolio(r.metadata.strategy_instance_id,r.metadata.idempotency_key+(leg.buy?":B":":S"),{leg},account_view(),limits_,frozen_);
        if(!reservation.ok)throw std::runtime_error("atomic single-writer admission diverged");
        emit_({{"type","MAKER_RISK_RESERVED"},{"request",meta(r.metadata)},{"reservation_id",reservation.reservation_id},{"cash_tick",risk::portfolio_cash({leg})},{"quantity",leg.quantity},{"buy",leg.buy},{"asset_id",leg.asset}});
        auto approved=*local.approved_quote;approved.has_bid=leg.buy;approved.has_ask=!leg.buy;
        auto plan=oms_.prepare_maker(approved,r.metadata,leg,reservation.reservation_id,now_);
        plan.suppress_ack=fault_=="maker_ack_unknown";plan.suppress_cancel_ack=fault_=="maker_cancel_timeout";plan.cancel_reject=fault_=="maker_cancel_reject";
        for(std::uint16_t n=0;n<(leg.buy?depth.bid_count:depth.ask_count);++n) {const auto& level=leg.buy?depth.bids[n]:depth.asks[n];if(level.price_tick==leg.price_tick)plan.queue_ahead_lots=static_cast<std::int64_t>(level.size);}
        emit_({{"type","MAKER_ORDER_PREPARED"},{"order",ex::maker_order_json(plan)},{"plan_sha256",sha256(ex::ExecutionGateway::maker_plan_identity(plan))}});
        prepared_at_[plan.order_id]=monotonic_ns();
        timing_.observe("risk_reservation_to_durable_plan",monotonic_ns()-started);
        if(fault_=="maker_crash_after_plan")::_exit(85);
        if(shadow_ && fault_!="shadow_adapter_attempt") {
            emit_({{"type","MAKER_SHADOW_WOULD_SUBMIT"},{"order",ex::maker_order_json(plan)}});risk_.release_portfolio_confirmed(reservation.reservation_id);
            emit_({{"type","MAKER_SHADOW_RESERVATION_RELEASED"},{"reservation_id",reservation.reservation_id}});
        }else oms_.submit_maker(plan);
    }
}
void StrategySupervisor::before_effect(const std::string& action,const ex::MakerOrder& order) {
    if(shadow_)throw std::runtime_error("SHADOW maker adapter invariant");
    auto r=risk_.portfolio_reservations().find(order.reservation_id);
    if(r==risk_.portfolio_reservations().end() || !r->second.active || r->second.owner!=order.request.strategy_instance_id)throw std::runtime_error("maker adapter has no central risk authorization");
    if(action=="MAKER_SUBMIT_STARTED" && (frozen_ || r->second.remaining.size()!=1 || r->second.remaining[0]!=order.resource))throw std::runtime_error("maker submit authorization changed");
    emit_({{"type",action},{"order",ex::maker_order_json(order)},{"plan_sha256",sha256(ex::ExecutionGateway::maker_plan_identity(order))},{"transport","SIMULATED_PAPER_ONLY"}});
    if(action=="MAKER_ADAPTER_INVOKED") {auto t=prepared_at_.find(order.order_id);if(t!=prepared_at_.end()){timing_.observe("maker_durable_plan_to_adapter",monotonic_ns()-t->second);prepared_at_.erase(t);}}
    if(action=="MAKER_SUBMIT_STARTED" && fault_=="maker_crash_after_submit")::_exit(86);
}
void StrategySupervisor::account_fill(const ex::MakerOrder& order,const ex::MakerOrderReport& report) {
    const auto before=monotonic_ns();paper::PaperFill fill;fill.receipt_id=report.receipt_id;fill.fill_id=fill.report_id=strategy_identity_hash(report.receipt_id);
    fill.order_id=strategy_identity_hash(order.order_id);fill.asset_id=order.resource.asset;fill.asset_index=instruments_.at(fill.asset_id).asset_index;
    fill.side=order.resource.buy?ex::OrderSide::Buy:ex::OrderSide::Sell;fill.liquidity_role=ex::FillLiquidityRole::Maker;
    fill.qty_lots=report.quantity;fill.fill_price_tick=report.price_tick;fill.fee_tick=report.fee_tick;fill.ts_ns=report.ts_ns;
    const auto& before_positions=paper_.ledger().position_ledger().positions();auto old=before_positions.find(fill.asset_id);
    auto before_cost=old==before_positions.end()?0:old->second.cost_basis_tick;
    auto result=paper_.on_paper_fill(fill);if(!result.applied && result.status!=paper::PaperLedgerApplyStatus::DuplicateIgnored)throw std::runtime_error("maker fill and canonical ledger mismatch");
    if(result.applied) {
        auto owner=order.request.strategy_instance_id;strategy_cashflow_[owner]+=(order.resource.buy?-1:1)*report.quantity*report.price_tick-report.fee_tick;strategy_fees_[owner]+=report.fee_tick;
        if(order.resource.buy)attributed_lots_[owner][fill.asset_id]+=report.quantity;
        else {
            auto after=paper_.ledger().position_ledger().positions().find(fill.asset_id);auto removed_cost=before_cost-(after==paper_.ledger().position_ledger().positions().end()?0:after->second.cost_basis_tick);
            auto remaining=report.quantity,unallocated=removed_cost;
            for(auto& [strategy,assets]:attributed_lots_) {auto take=std::min(remaining,assets[fill.asset_id]);
                if(take) {auto transfer=take==remaining?unallocated:removed_cost*take/report.quantity;unallocated-=transfer;
                    if(strategy!=owner){strategy_cashflow_[strategy]+=transfer;strategy_cashflow_[owner]-=transfer;}}
                assets[fill.asset_id]-=take;remaining-=take;
            }
            if(remaining)stop("ATTRIBUTION_INVENTORY_MISMATCH");
        }
    }
    timing_.observe("simulated_report_to_ledger",monotonic_ns()-before);
}
void StrategySupervisor::on_report(const ex::MakerOrder& order,const ex::MakerOrderReport& report) {
    const auto before=monotonic_ns();emit_({{"type","MAKER_ORDER_REPORT"},{"order",ex::maker_order_json(order)},{"report",ex::maker_report_json(report)}});
    if(report.type==ex::MakerReportType::Fill && fault_=="maker_crash_after_fill")::_exit(87);
    if(report.type==ex::MakerReportType::Fill)account_fill(order,report);
    auto found=risk_.portfolio_reservations().find(order.reservation_id);
    if(found==risk_.portfolio_reservations().end() || !found->second.active) {if(report.type==ex::MakerReportType::Fill)stop("LATE_FILL_AFTER_TERMINAL_RELEASE");}
    else if(ex::maker_terminal(order.status))risk_.release_portfolio_confirmed(order.reservation_id);
    else {auto leg=order.resource;leg.quantity=order.remaining_qty;risk_.resize_portfolio(order.reservation_id,{leg});}
    emit_({{"type","MAKER_RESERVATION_STATE"},{"reservation_id",order.reservation_id},{"remaining_qty",order.remaining_qty},{"terminal_confirmed",ex::maker_terminal(order.status)}});
    if(report.type==ex::MakerReportType::Timeout || order.status==ex::MakerOrderStatus::AckUnknown || order.status==ex::MakerOrderStatus::RecoveryBlocked) {
        frozen_=true;freeze_reason_="UNKNOWN_ORDER_OR_CANCEL_ACK";emit_({{"type","UNKNOWN_MAKER_EXPOSURE_FREEZE"},{"order_id",order.order_id}});
    }
    auto& inst=instance(order.request.strategy_instance_id);if(inst.maker && (ex::maker_terminal(order.status) || report.type==ex::MakerReportType::Fill)) {
        auto asset_index=instruments_.at(order.resource.asset).asset_index;inst.maker->on_order_event(asset_index,true);
        emit_({{"type","STRATEGY_DESIRED_RESET"},{"strategy_instance_id",order.request.strategy_instance_id},{"asset_index",asset_index}});
    }
    timing_.observe("oms_transition",monotonic_ns()-before);
}
void StrategySupervisor::stop(const std::string& reason) {frozen_=true;freeze_reason_=reason;emit_({{"type","PORTFOLIO_FREEZE"},{"reason",reason}});}
risk::PortfolioAdmission StrategySupervisor::check_bundle(const order_decision::OrderDecisionLite& d,const signal::OpportunityIntent& i) const {
    std::vector<risk::PortfolioLeg> legs;for(std::uint16_t n=0;n<d.leg_count;++n) {
        auto leg=resource(i.legs[n].asset_id,d.legs[n].side==order_decision::Side::Buy,d.legs[n].quantity_lots,d.legs[n].estimated_vwap_tick);
        leg.fee_per_lot_tick=0;legs.push_back(leg);
    }
    return risk_.check_portfolio(active_metadata.strategy_instance_id,legs,account_view(),limits_,frozen_);
}
void StrategySupervisor::atomic_fill(const ex::ExecutionReport& report,const ex::ChildOrder& child) {
    if(report.filled_lots<=0)return;auto owner=active_metadata.strategy_instance_id;
    attributed_lots_[owner][child.asset_id]+=report.filled_lots;strategy_cashflow_[owner]-=report.filled_lots*report.avg_fill_price_tick+report.fee_tick;strategy_fees_[owner]+=report.fee_tick;
}
void StrategySupervisor::reconcile() {
    std::map<std::string,std::int64_t> sells;std::set<std::uint64_t> seen;
    for(const auto& [id,o]:oms_.maker_orders())if(!ex::maker_terminal(o.status)) {
        auto r=risk_.portfolio_reservations().find(o.reservation_id);
        if(r==risk_.portfolio_reservations().end() || !r->second.active || r->second.remaining.size()!=1 || r->second.remaining[0].quantity!=o.remaining_qty || !seen.insert(o.reservation_id).second) {stop("OMS_RESERVATION_MISMATCH");return;}
        if(!o.resource.buy)sells[o.resource.asset]+=o.remaining_qty;
    }
    for(const auto& [id,r]:risk_.portfolio_reservations())if(r.active && !seen.contains(id)) {stop("ORPHAN_RESERVATION_MISMATCH");return;}
    auto account=account_view();for(const auto& [asset,qty]:sells)if(account.holdings[asset].lots<qty)stop("SELL_INVENTORY_MISMATCH");
    if(account.cash_tick<risk_.ledger_snapshot().total_reserved_cost_tick || account.cash_tick<0)stop("CASH_RESERVATION_MISMATCH");
}
void StrategySupervisor::boundary(std::uint64_t now,const std::string& event_id,bool market_event,const j::object& input,StrategyPlugin& combinatorial,const IntentSink& sink) {
    now_=now;fault_=test_faults_?text(input,"test_fault"):"";last_input_=input;
    std::vector<MakerFrameAsset> assets;for(const auto& [asset,i]:instruments_) {
        auto b=snapshot_(asset);if(!b.ok)continue;auto depth=state::market_depth_view_from_snapshot(b.value,i.asset_index);
        if(now<depth.last_ws_recv_ns || now-depth.last_ws_recv_ns>quote_policy_.max_book_age_ns)depth.usable_for_depth=false;
        const auto& positions=paper_.ledger().position_ledger().positions();auto p=positions.find(asset);
        assets.push_back({i.market,asset,i.market_index,i.asset_index,depth,p==positions.end()?0:p->second.qty_lots});
    }
    if(!shadow_) {
        std::vector<ex::PaperMakerMarketEvent> events;
        std::set<std::string> changed;j::object public_trade;
        if(auto trade=input.if_contains("public_trade")) {public_trade=trade->as_object();changed.insert(text(public_trade,"asset_id"));}
        auto inspect=[&](const j::value& value,auto&& inspect_ref)->void {
            if(value.is_array()){for(const auto& child:value.as_array())inspect_ref(child,inspect_ref);return;}
            if(!value.is_object())return;const auto& row=value.as_object();auto asset=text(row,"asset_id");if(!asset.empty())changed.insert(asset);
            if(auto changes=row.if_contains("price_changes"))inspect_ref(*changes,inspect_ref);
            if(text(row,"event_type")=="tick_size_change" && instruments_.contains(asset)) {
                try {if(cross_venue::money(text(row,"new_tick_size"))!=instruments_.at(asset).tick)stop("INSTRUMENT_TICK_CHANGED_REQUIRES_NEW_CONFIG");}
                catch(const std::exception&){stop("INVALID_INSTRUMENT_TICK_EVENT");}
            }
            if(text(row,"event_type")=="last_trade_price" && !asset.empty()) {
                try {public_trade={{"asset_id",asset},{"price_tick",cross_venue::money(text(row,"price"))},{"quantity_lots",cross_venue::money(text(row,"size"))/cross_venue::unit},{"side",text(row,"side")=="SELL"?"Sell":"Buy"}};}
                catch(const std::exception&) {public_trade.clear();}
            }
        };
        if(auto payload=input.if_contains("payload")) {if(payload->is_string()){auto parsed=j::parse(payload->as_string());inspect(parsed,inspect);}else inspect(*payload,inspect);}
        if(auto raw=input.if_contains("raw_payload")) {boost::system::error_code ec;auto parsed=j::parse(raw->as_string(),ec);if(!ec)inspect(parsed,inspect);}
        if(market_event)for(const auto& asset:assets)if(changed.contains(asset.asset_id) && asset.depth.usable_for_depth && !asset.depth.closed && !asset.depth.recovering) {
            ex::PaperMakerMarketEvent event;event.asset_index=asset.asset_index;event.ts_ns=now;event.depth=&asset.depth;event.observed_ns=number(input,"callback_recv_monotonic_ns",now);
            if(text(public_trade,"asset_id")==asset.asset_id) {event.has_trade=true;event.trade_price_tick=number(public_trade,"price_tick");event.trade_qty_lots=number(public_trade,"quantity_lots");event.trade_aggressor_side=text(public_trade,"side")=="Sell"?ex::OrderSide::Sell:ex::OrderSide::Buy;}
            events.push_back(event);
        }
        oms_.advance_maker(now,events,event_id);
        // Fill events changed canonical inventory; refresh the immutable strategy frame.
        for(auto& asset:assets) {const auto& positions=paper_.ledger().position_ledger().positions();auto p=positions.find(asset.asset_id);asset.position_lots=p==positions.end()?0:p->second.qty_lots;}
        for(const auto& [id,o]:oms_.maker_orders())if(!ex::maker_terminal(o.status)) {
            auto a=std::find_if(assets.begin(),assets.end(),[&](auto& x){return x.asset_id==o.resource.asset;});
            if(a==assets.end() || !a->depth.usable_for_depth || a->depth.closed || a->depth.recovering)oms_.cancel_maker(id,now);
        }
    }
    std::vector<std::string> ready;for(const auto& [key,r]:replacements_) {
        auto& q=std::get<mm::QuoteIntent>(r.payload);bool outstanding=false;
        for(const auto& [id,o]:oms_.maker_orders())if(!ex::maker_terminal(o.status) && o.resource.asset==q.asset_id && o.request.strategy_instance_id==r.metadata.strategy_instance_id)outstanding=true;
        if(!outstanding)ready.push_back(key);
    }
    for(const auto& key:ready) {auto r=replacements_.at(key);replacements_.erase(key);r.metadata.availability_ns=now;quote(r);}
    for(auto& inst:instances_) {
        if(!inst.enabled || inst.lifecycle!="RUNNING" || frozen_ || inst.manual_test_requests)continue;
        active_metadata=inst.identity;active_metadata.availability_ns=now;
        const auto started=monotonic_ns();
        std::vector<StrategyRequest> requests;std::vector<std::pair<signal::OpportunityIntent,signal::SignalEvidenceView>> intents;
        try {
            if(fault_=="plugin_exception" && inst.maker)throw std::runtime_error("injected plugin exception");
            if(fault_=="plugin_slow" && inst.maker)std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::vector<MakerFrameAsset> subscribed;for(const auto& a:assets)if(std::find(inst.assets.begin(),inst.assets.end(),a.asset_id)!=inst.assets.end())subscribed.push_back(a);
            if(inst.maker)inst.maker->begin_frame();
            auto& selected=inst.maker?static_cast<StrategyPlugin&>(*inst.maker):combinatorial;
            selected.on_immutable_frame(subscribed,now,[&](const auto& r){requests.push_back(r);},[&](const auto& i,const auto& e){intents.emplace_back(i,e);});
            if(inst.maker) {
                if(record_frames_)for(std::size_t i=0;i<subscribed.size();++i) {
                    const auto& result=inst.maker->last_results()[i];j::array quotes,cancels;for(std::uint16_t n=0;n<result.quote_count;++n)quotes.push_back(quote_payload_json(result.quotes[n]));
                    for(std::uint16_t n=0;n<result.cancel_count;++n)cancels.push_back(result.cancels[n].idempotency_hash);
                    emit_({{"type","STRATEGY_FRAME"},{"strategy_instance_id",inst.identity.strategy_instance_id},{"frame",maker_frame_json(subscribed[i],now)},
                        {"result_hash",result.output_hash},{"fair_value_tick",result.fair_value_tick},{"quotes",quotes},{"cancel_hashes",cancels}});
                }
            }
            auto elapsed=monotonic_ns()-started;timing_.observe(inst.maker?"maker_callback":"combinatorial_callback",elapsed);
            if(elapsed>inst.budget_ns) {inst.lifecycle="QUARANTINED";inst.error="CALLBACK_BUDGET_EXCEEDED";emit_({{"type","STRATEGY_FAILURE"},{"strategy_instance_id",inst.identity.strategy_instance_id},{"reason",inst.error}});
                if(!shadow_)for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==inst.identity.strategy_instance_id && !ex::maker_terminal(o.status))oms_.cancel_maker(id,now);continue;}
        }catch(const std::exception&) {
            inst.lifecycle="QUARANTINED";inst.error="PLUGIN_OR_REQUEST_FAILURE";emit_({{"type","STRATEGY_FAILURE"},{"strategy_instance_id",inst.identity.strategy_instance_id},{"reason",inst.error}});
            if(!shadow_)for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==inst.identity.strategy_instance_id && !ex::maker_terminal(o.status))oms_.cancel_maker(id,now);
            continue;
        }
        // Financial/journal failures escape; they cannot be downgraded to plugin faults.
        for(const auto& r:requests)request(r);
        for(const auto& [i,e]:intents) {
            active_metadata.market_state_version=i.snapshot_version_hash;active_metadata.request_id=std::to_string(i.intent_id);active_metadata.idempotency_key=std::to_string(i.idempotency_hash);
            if(i.status!=signal::IntentStatus::PaperOpportunity)active_metadata.idempotency_key+="@"+std::to_string(now);
            active_metadata.source_timestamp=i.created_ts_ns;request({active_metadata,i},&e,&sink);
        }
    }
    active_metadata={};reconcile();
}
j::object StrategySupervisor::financial_state() const {
    j::array orders,reservations;for(const auto& [id,o]:oms_.maker_orders())orders.push_back(ex::maker_order_json(o));
    for(const auto& [id,r]:risk_.portfolio_reservations())if(r.active) {j::array legs;for(const auto& l:r.remaining)legs.push_back(j::object{{"asset",l.asset},{"buy",l.buy},{"quantity",l.quantity},{"price_tick",l.price_tick},{"fee_per_lot_tick",l.fee_per_lot_tick}});reservations.push_back(j::object{{"id",id},{"owner",r.owner},{"legs",legs}});}
    return {{"maker_orders",orders},{"portfolio_reservations",reservations}};
}
j::object StrategySupervisor::telemetry() const {
    auto result=financial_state();j::array strategies;for(const auto& i:instances_)strategies.push_back(j::object{{"strategy_id",i.identity.strategy_id},{"strategy_instance_id",i.identity.strategy_instance_id},
        {"configuration_epoch",i.identity.configuration_epoch},{"model_hash",i.identity.model_version},{"lifecycle",i.lifecycle},{"enabled",i.enabled},{"priority",i.priority},{"budget_tick",limits_.strategy_budget.at(i.identity.strategy_instance_id)},{"error",i.error}});
    result["strategies"]=strategies;result["portfolio_frozen"]=frozen_;result["freeze_reason"]=freeze_reason_;result["allocation_policy"]="priority ascending, instance_id ascending; each callback request order; existing commitments first";
    result["canonical_account_owner"]="PaperLedger";result["order_owner"]="ExecutionGateway";result["reservation_owner"]="RiskEngine/ReservationBook";
    result["execution_model"]=static_cast<int>(simulation_.model);result["verified_live_realized_pnl"]=nullptr;
    auto portfolio=risk_.ledger_snapshot();j::object sells;std::int64_t unknown=0;
    for(const auto& [id,r]:risk_.portfolio_reservations())if(r.active)for(const auto& leg:r.remaining)if(!leg.buy) {
        auto p=sells.if_contains(leg.asset);sells[leg.asset]=(p?p->to_number<std::int64_t>():0)+leg.quantity;
    }
    for(const auto& [id,o]:oms_.maker_orders())if(!ex::maker_terminal(o.status) && (o.status==ex::MakerOrderStatus::AckUnknown || o.status==ex::MakerOrderStatus::RecoveryBlocked ||
        (o.status==ex::MakerOrderStatus::CancelPending && !o.cancel_due_ns)))unknown+=risk::portfolio_cash({o.resource});
    result["portfolio_risk"]=j::object{{"max_total_exposure_tick",limits_.total_cost_tick},{"max_event_exposure_tick",limits_.event_cost_tick},{"max_asset_inventory_lots",limits_.asset_lots},
        {"reserved_cash_tick",portfolio.total_reserved_cost_tick},{"available_cash_tick",paper_.ledger().cash_ledger().cash_tick-portfolio.total_reserved_cost_tick-limits_.safety_cash_tick},
        {"sell_inventory_reserved",sells},{"unknown_reserved_cash_tick",unknown},{"safety_cash_tick",limits_.safety_cash_tick},{"freeze_reason",freeze_reason_},
        {"underlying_event_policy","exact venue plus condition identity; positive exposure sums; no title-based netting"}};
    std::map<std::string,std::int64_t> marks;std::int64_t marked=0,cost=0;bool marks_valid=true;
    for(const auto& [asset,p]:paper_.ledger().position_ledger().positions()) {
        auto book=snapshot_(asset);auto mid=book.ok && book.value.bid_count && book.value.ask_count?(book.value.bids[0].price_tick+book.value.asks[0].price_tick)/2:0;
        marks[asset]=mid;marked+=p.qty_lots*mid;cost+=p.cost_basis_tick;
        if(!book.ok || !book.value.usable_for_depth || now_<book.value.last_book_update_ns || now_-book.value.last_book_update_ns>quote_policy_.max_book_age_ns)marks_valid=false;
    }
    auto cash=paper_.ledger().cash_ledger();result["portfolio_pnl"] = j::object{{"gross_trading_pnl_tick",cash.cash_tick+marked-cash.starting_cash_tick+cash.fees_paid_tick},
        {"fees_tick",cash.fees_paid_tick},{"inventory_mark_to_market_tick",marked-cost},{"realized_pnl_tick",cash.realized_pnl_tick},{"unrealized_pnl_tick",marked-cost},
        {"net_equity_pnl_tick",cash.cash_tick+marked-cash.starting_cash_tick},{"estimated_rewards_tick",0},{"confirmed_rewards_tick",0},{"terminal_payoff_estimate",nullptr},
        {"settled_cash_pnl",nullptr},{"marks_usable",marks_valid},{"accounting_source","CANONICAL_PAPER_LEDGER"},{"execution_source","SIMULATED"}};
    j::array attribution;for(const auto& i:instances_) {auto id=i.identity.strategy_instance_id;auto get=[&](const auto& m){auto it=m.find(id);return it==m.end()?0:it->second;};
        std::int64_t value=0;if(auto own=attributed_lots_.find(id);own!=attributed_lots_.end())for(const auto& [asset,qty]:own->second)value+=qty*marks[asset];
        attribution.push_back(j::object{{"strategy_instance_id",id},{"cashflow_tick",get(strategy_cashflow_)},{"marked_inventory_value_tick",value},{"net_equity_pnl_tick",get(strategy_cashflow_)+value},
            {"fees_tick",get(strategy_fees_)},{"estimated_rewards_tick",0},{"confirmed_rewards_tick",0},{"settled_cash_pnl",nullptr}});}
    result["strategy_pnl_attribution"]=attribution;return result;
}
void StrategySupervisor::control(const j::object& input,std::uint64_t now,bool allowed) {
    now_=now;fault_=allowed?text(input,"test_fault"):"";
    const auto operation=text(input,"operation");
    if(operation=="strategy_stop") {
        auto& inst=instance(text(input,"strategy_instance_id"));inst.enabled=false;inst.lifecycle="SUSPENDED";
        emit_({{"type","STRATEGY_SUSPENDED"},{"strategy_instance_id",inst.identity.strategy_instance_id}});
        if(!shadow_)for(const auto& [id,o]:oms_.maker_orders())if(o.request.strategy_instance_id==inst.identity.strategy_instance_id && !ex::maker_terminal(o.status))oms_.cancel_maker(id,now);
        return;
    }
    if(operation=="reduce_limits") {
        const auto next=number(input,"max_total_exposure_tick");if(next<=0 || next>limits_.total_cost_tick)throw std::runtime_error("limit control only permits reductions");
        limits_.total_cost_tick=next;auto account=account_view();std::int64_t filled=0;for(const auto& [a,h]:account.holdings)filled+=h.cost_tick;
        emit_({{"type","PORTFOLIO_LIMIT_REDUCED"},{"max_total_exposure_tick",next}});
        if(filled+risk_.ledger_snapshot().total_reserved_exposure_tick>next)stop("LIMIT_REDUCTION_WITH_OUTSTANDING_EXPOSURE");return;
    }
    if(!allowed)throw std::runtime_error("maker fault controls require explicit test profile");
    if(operation=="settlement_receipt") {stop("UNVERIFIED_SETTLEMENT_RECEIPT_NO_CASH_RELEASE");return;}
    auto& inst=instance(text(input,"strategy_instance_id","maker"));
    if(operation=="report") {
        std::string id=text(input,"order_id");auto target=text(input,"target_request_id");if(id.empty())for(const auto& [key,o]:oms_.maker_orders())if(o.request.strategy_instance_id==inst.identity.strategy_instance_id &&
            (target.empty()?!ex::maker_terminal(o.status):o.request.request_id==target)){id=key;break;}
        auto& order=oms_.maker_orders().at(id);ex::MakerOrderReport report;report.order_id=id;report.ts_ns=now;report.receipt_id=text(input,"receipt_id","SIMULATED_TEST:"+std::to_string(now));
        report.source_event_id="EXPLICIT_SYNTHETIC_TEST";report.quantity=number(input,"quantity");report.price_tick=number(input,"price_tick",order.resource.price_tick);report.fee_tick=report.quantity*order.resource.fee_per_lot_tick;
        auto type=text(input,"report_type");bool found=false;for(int i=0;i<=static_cast<int>(ex::MakerReportType::Timeout);++i)if(type==ex::maker_report_name(static_cast<ex::MakerReportType>(i))){report.type=static_cast<ex::MakerReportType>(i);found=true;}
        if(!found)throw std::runtime_error("unknown test receipt type");oms_.apply_maker_report(report);reconcile();return;
    }
    if(operation=="cancel") {
        mm::CancelQuoteIntent payload;payload.quote_group_id=number(input,"quote_group_id",1);payload.created_ts_ns=now;payload.idempotency_hash=strategy_identity_hash(text(input,"request_id",std::to_string(now)));payload.cancel_intent_id=payload.idempotency_hash;
        auto metadata=inst.identity;metadata.availability_ns=now;metadata.source_timestamp=now;metadata.request_id=text(input,"request_id",std::to_string(now));metadata.idempotency_key=metadata.request_id;
        request({metadata,payload});return;
    }
    if(operation=="quote") {
        if(!inst.maker)throw std::runtime_error("quote request emitted by non-maker strategy");
        auto asset=text(input,"asset_id",inst.assets.front());auto book=snapshot_(asset);if(!book.ok)throw std::runtime_error("test quote missing book");auto& instrument=instruments_.at(asset);
        mm::QuoteIntent q;q.market_id=instrument.market;q.asset_id=asset;q.market_index=instrument.market_index;q.asset_index=instrument.asset_index;q.strategy_id=strategy_identity_hash(inst.identity.strategy_instance_id);
        q.quote_group_id=number(input,"quote_group_id",1);q.created_ts_ns=number(input,"created_ns",now);q.expires_at_ns=q.created_ts_ns+number(input,"ttl_ns",10'000'000'000ULL);
        q.has_bid=boolean(input,"has_bid",true);q.has_ask=boolean(input,"has_ask",false);q.fair_value_tick=number(input,"fair_value_tick",500000);q.snapshot_version_hash=book.value.snapshot_version_hash;
        auto leg=[&](bool buy){mm::QuoteLeg l;l.market_id=q.market_id;l.asset_id=asset;l.market_index=q.market_index;l.asset_index=q.asset_index;l.side=buy?mm::QuoteSide::Bid:mm::QuoteSide::Ask;
            l.price_tick=number(input,buy?"bid_tick":"ask_tick",buy?450000:550000);l.quantity_lots=number(input,"quantity",10);l.fair_value_tick=q.fair_value_tick;l.edge_to_fair_tick=buy?q.fair_value_tick-l.price_tick:l.price_tick-q.fair_value_tick;
            l.book_version=book.value.version;l.snapshot_version_hash=q.snapshot_version_hash;return l;};q.bid=leg(true);q.ask=leg(false);
        auto metadata=inst.identity;metadata.availability_ns=now;metadata.source_timestamp=now;metadata.market_state_version=q.snapshot_version_hash;
        metadata.request_id=text(input,"request_id",std::to_string(now));metadata.idempotency_key=metadata.request_id;q.quote_intent_id=q.idempotency_hash=strategy_identity_hash(metadata.request_id);q.type=mm::QuoteIntentType::PlaceQuote;
        request({metadata,q});return;
    }
    if(operation=="orphan_reservation") {
        auto leg=resource(inst.assets.front(),true,1,100000);auto r=risk_.reserve_portfolio(inst.identity.strategy_instance_id,"test-orphan",{leg},account_view(),limits_);if(!r.ok)throw std::runtime_error("orphan fixture admission failed");reconcile();return;
    }
    throw std::runtime_error("unknown maker control operation");
}
void StrategySupervisor::restore_tail(const std::vector<j::object>& events) {
    std::map<std::string,ex::MakerOrder> orders;std::set<std::string> submitted;
    for(const auto& e:events) {
        const auto type=text(e,"type");
        if(type=="MAKER_SUBMIT_STARTED" || type=="MAKER_CANCEL_STARTED" || type=="MAKER_ORDER_REPORT") {
            auto order=ex::maker_order_from_json(e.at("order").as_object());submitted.insert(order.order_id);orders[order.order_id]=order;
            if(type=="MAKER_ORDER_REPORT") {auto report=ex::maker_report_from_json(e.at("report").as_object());if(report.type==ex::MakerReportType::Fill)account_fill(order,report);}
        }
    }
    for(auto& [id,o]:orders) {
        if(!ex::maker_terminal(o.status)) {auto leg=o.resource;leg.quantity=o.remaining_qty;risk_.restore_portfolio({o.reservation_id,o.request.strategy_instance_id,o.request.idempotency_key,{leg},true});}
        else {auto existing=risk_.portfolio_reservations().find(o.reservation_id);if(existing!=risk_.portfolio_reservations().end())risk_.release_portfolio_confirmed(o.reservation_id);}
        oms_.restore_maker_blocked(o);
    }
    frozen_=true;freeze_reason_=submitted.empty()?"NOT_SUBMITTED_INCOMPLETE_TRANSACTION":"RECOVERY_BLOCKED_QUERY_BEFORE_RETRY";
}
}

#include "engine/runtime/Runtime.h"
#include "engine/runtime/DurableJournal.h"
#include "engine/runtime/StrategyPlugin.h"
#include "engine/runtime/LatencyMetrics.h"
#include "engine/runtime/VenueMarketFacade.h"
#include "engine/runtime/LivePacketSource.h"
#include "engine/runtime/StrategySupervisor.h"
#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "engine/signal/reader/MarketStateViewSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/order_decision/core/OrderDecisionEngine.h"
#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/guards/ExposureGuard.h"
#include "engine/risk/guards/InventoryGuard.h"
#include "engine/execution/core/ExecutionGateway.h"
#include "engine/execution/adapter/PaperExecutionAdapter.h"
#include "engine/execution/publish/ExecutionReportPublisher.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"
#include "engine/paper/core/PaperTradingEngine.h"
#include "apps/paper_backend/DashboardApiRoutes.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <set>
#include <map>
#include <unistd.h>

namespace trading_engine::runtime {
namespace {
namespace j = boost::json;
namespace fs = std::filesystem;
volatile std::sig_atomic_t stopping = 0;
void signal_stop(int) { stopping = 1; }
std::string read(const fs::path& p) {
    std::ifstream f(p,std::ios::binary); if(!f) throw std::runtime_error("cannot read "+p.string());
    return {std::istreambuf_iterator<char>(f),{}};
}
std::string str(const j::object& o, const char* k, std::string fallback={}) {
    auto p=o.if_contains(k); return p ? std::string(p->as_string()) : fallback;
}
std::int64_t num(const j::object& o, const char* k,std::int64_t fallback=0) {
    auto p=o.if_contains(k); return p ? p->to_number<std::int64_t>() : fallback;
}
std::uint64_t uid(const j::object& o,const char* k) { return o.at(k).to_number<std::uint64_t>(); }
bool flag(const j::object& o,const char* k,bool fallback=false) {
    auto p=o.if_contains(k); return p ? p->as_bool() : fallback;
}
struct Config {
    j::object raw; j::array markets;
    std::string mode,format,config_hash,input_hash,artifact_hash,binary_hash;
    fs::path input,artifact,out,kill;
    std::int64_t cash=0, max_records=0;
    std::uint64_t stale_ns=0;
    bool fault_tests=false, operational=false,multi=false;
    std::string execution_policy,market_hash;
    std::uint64_t duration=0,queue_capacity=4096;
    risk::RiskPolicySnapshot policy;
};
Config load(const fs::path& path) {
    Config c; auto bytes=read(path); c.raw=j::parse(bytes).as_object(); c.config_hash=sha256(bytes);
    auto schema=num(c.raw,"schema_version");
    if(schema!=1 && schema!=2 && schema!=3) throw std::runtime_error("unsupported configuration schema");
    c.operational=schema>=2;c.multi=schema==3;
    if(flag(c.raw,"live_enabled",true)) throw std::runtime_error("live_enabled must explicitly be false");
    c.mode=str(c.raw,"mode");
    if(c.mode!="REPLAY" && c.mode!="PAPER" && c.mode!="SHADOW") throw std::runtime_error("only REPLAY/PAPER/SHADOW are supported");
    if((!c.multi && str(c.raw,"strategy")!="combinatorial") || (c.multi && str(c.raw,"strategy")!="multi_strategy")) throw std::runtime_error("strategy plugin not integrated; fail closed");
    for(const char* key:{"input","oracle_artifact","output_dir"})
        if(str(c.raw,key).empty()) throw std::runtime_error(std::string("missing path: ")+key);
    c.input=fs::absolute(str(c.raw,"input")); c.artifact=fs::absolute(str(c.raw,"oracle_artifact"));
    c.out=fs::absolute(str(c.raw,"output_dir")); c.format=str(c.raw,"input_format","jsonl");
    if(c.format!="jsonl" && c.format!="raw" && c.format!="live_ws" && c.format!="venue_journal" && c.format!="venue_jsonl") throw std::runtime_error("unsupported feed format");
    c.execution_policy=c.mode=="REPLAY"?str(c.raw,"replay_execution_policy","PAPER"):c.mode;
    if(c.execution_policy!="PAPER" && c.execution_policy!="SHADOW") throw std::runtime_error("invalid replay execution policy");
    if(c.operational && c.mode!="REPLAY" && c.format!="live_ws" && !flag(c.raw,"recorded_fault_fixture"))
        throw std::runtime_error("schema v2 PAPER/SHADOW require live_ws; use versioned recorded test profile");
    if(c.format=="live_ws" || c.format=="venue_journal" || c.format=="venue_jsonl") {
        c.markets=j::parse(read(str(c.raw,"markets_file"))).as_array();
        c.market_hash=sha256(j::serialize(c.markets));
        c.duration=num(c.raw,"duration_seconds"); c.queue_capacity=num(c.raw,"queue_capacity",4096);
        if(c.format=="live_ws" && (c.duration==0 || c.duration>72*3600 || c.mode=="REPLAY")) throw std::runtime_error("live source requires finite PAPER/SHADOW duration");
    }
    c.cash=num(c.raw,"starting_cash_tick"); c.max_records=num(c.raw,"max_records");
    c.stale_ns=static_cast<std::uint64_t>(num(c.raw,"max_book_age_ns"));
    if(c.cash<=0 || c.cash>1'000'000'000'000LL || c.max_records<=0 || c.max_records>1'000'000 || !c.stale_ns || c.stale_ns>60'000'000'000ULL)
        throw std::runtime_error("bounded positive cash, record budget and book age required");
    c.kill=str(c.raw,"kill_switch_file"); c.fault_tests=flag(c.raw,"allow_test_faults");
    auto& p=c.policy;
    p.policy_version=num(c.raw,"config_version",1);
    if(p.policy_version==0) throw std::runtime_error("config_version must be positive");
    p.max_book_age_ns=c.stale_ns; p.max_intent_age_ns=c.stale_ns;
    p.max_snapshot_skew_ns=1'000'000; p.min_depth_margin_ratio=1.0; p.min_depth_margin_bps=10'000;
    p.max_total_cost_tick=num(c.raw,"max_order_cost_tick");
    p.max_total_exposure_tick=num(c.raw,"max_total_exposure_tick");
    p.max_single_market_exposure_tick=num(c.raw,"max_market_exposure_tick");
    p.max_inventory_lots_per_asset=num(c.raw,"max_asset_inventory_lots");
    if(p.max_total_cost_tick<=0 || p.max_total_cost_tick>c.cash || p.max_total_exposure_tick<=0 ||
       p.max_single_market_exposure_tick<=0 || p.max_inventory_lots_per_asset<=0 || p.max_inventory_lots_per_asset>1'000'000)
        throw std::runtime_error("explicit finite portfolio limits required");
    p.max_pending_intents_total=c.multi?128:1;
    p=risk::with_computed_policy_hash(p);
    c.input_hash=c.format=="live_ws"?"APPEND_ONLY_LIVE_INPUT":file_sha256(c.input);
    c.binary_hash=file_sha256("/proc/self/exe");
    std::vector<fs::path> files;
    for(auto& e:fs::recursive_directory_iterator(c.artifact)) if(e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(),files.end()); std::string manifest;
    for(auto& f:files) manifest+=fs::relative(f,c.artifact).string()+":"+file_sha256(f)+"\n";
    c.artifact_hash=sha256(manifest);
    return c;
}
std::vector<j::object> inputs(const Config& c) {
    std::vector<j::object> result;
    if(c.format=="live_ws" || c.format=="venue_journal") {
        if(!fs::exists(c.input))return result;
        DurableJournal raw(c.input);
        result=raw.recovered();
    } else if(c.format=="jsonl" || c.format=="venue_jsonl") {
        std::ifstream in(c.input); std::string line;
        while(std::getline(in,line)) {
            if(line.empty()) throw std::runtime_error("empty feed record");
            result.push_back(j::parse(line).as_object());
            if(result.size()>static_cast<std::size_t>(c.max_records)) throw std::runtime_error("feed exceeds configured record budget");
        }
    } else {
        feed::RawLogReader reader(c.input.string());
        for(;;) {
            auto r=reader.next();
            if(r.eof()) break;
            if(!r.ok()) throw std::runtime_error("invalid raw feed record");
            const auto& p=*r.packet;
            if(p.header.codec!=feed::Codec::None || p.header.source_id!=feed::SourceId::PolymarketMarket ||
               feed::has_flag(p.header.flags,feed::PacketUnsafe)) throw std::runtime_error("unsupported or unsafe raw source");
            result.push_back({{"kind","packet"},{"recv_ns",p.header.recv_monotonic_ns},{"wall_ns",p.header.recv_wall_ns},
                              {"connection_id",p.header.connection_id},{"packet_id",p.header.packet_id},{"payload",p.payload}});
            if(result.size()>static_cast<std::size_t>(c.max_records)) throw std::runtime_error("feed exceeds configured record budget");
        }
    }
    if(result.empty() && c.format!="live_ws") throw std::runtime_error("empty input feed");
    return result;
}

class Session final : public execution::ExecutionReportPublisher, public execution::ReservationDispositionPublisher {
public:
    Session(const Config& c,DurableJournal& journal)
    : c(c), journal(journal), view(store), snapshot_reader(view), edge(fees,latency),
      paper(c.cash), gateway(&adapter,this,this) {
        if(c.operational) paper.set_history_limit(1000);
        auto loaded=artifacts.load(c.artifact); if(!loaded.ok) throw std::runtime_error("artifact load: "+loaded.error);
        for(const auto& b:artifacts.active_bundles()) {
            if(b.required_true_mask || b.required_false_mask || b.invalid_mask)
                throw std::runtime_error("conditional settlement masks require chain authority; plugin disabled");
        }
        for(const auto& b:artifacts.active_bundles()) for(std::uint16_t i=0;i<b.leg_count;++i) {
            auto& leg=b.legs[i]; universe.active_asset_ids.insert(leg.asset_id); universe.active_market_ids.insert(leg.market_id);
            market_by_asset[leg.asset_id]=leg.market_id;
        }
        for(const auto& plan:artifacts.active_runtime_plans())
            for(std::uint16_t n=0;n<plan.leg_count;++n)
                paper.bind_asset_index(*plan.asset_ids[n],plan.asset_indices[n]);
        filter=std::make_unique<state::MarketStateEventFilter>(universe);
        signal::SignalConfig sc; sc.emit_rejections=true; sc.max_lob_age_ns=c.stale_ns;
        sc.max_snapshot_version_skew=1'000'000; sc.intent_ttl_ns=c.stale_ns;
        if(!c.markets.empty()) facade=std::make_unique<VenueMarketFacade>(c.markets,c.stale_ns);
        const signal::IMarketSnapshotReader* reads=facade?static_cast<const signal::IMarketSnapshotReader*>(facade.get()):&snapshot_reader;
        engine=std::make_unique<signal::SignalEngine>(sc,reads,&artifacts,&settlement,&vwap,&edge,&ranker,&intent_publisher);
        plugin=std::make_unique<CombinatorialPlugin>(*engine);
        risk::RiskRuntimeContext rc; rc.policy=&c.policy; risk_engine.set_runtime_context(rc);
        gateway.set_plan_observer([this](const execution::OrderPlan& p) { paper.on_order_plan(p); });
        gateway.set_before_adapter([this](const execution::OrderPlan& p) {
            if(!adapter_authorized || this->journal.dirty() || (input_journal && input_journal->dirty()) || execution::ExecutionGateway::plan_identity(p)!=authorized_plan)
                throw std::runtime_error("adapter blocked: durable prepared authorization missing");
            emit({{"type","ADAPTER_INVOKED"},{"plan_id",p.plan_id},{"plan_sha256",sha256(authorized_plan)},{"transport","SIMULATED_PAPER_ONLY"}});
            timing.observe("durable_plan_to_adapter",monotonic_ns()-durable_plan_at);
        });
        if(c.multi) supervisor=std::make_unique<StrategySupervisor>(c.raw,risk_engine,gateway,paper,
            [this](const std::string& asset){auto b=facade?facade->snapshot(asset,now):view.get_snapshot(asset);if(!facade && (!connected || !fresh.contains(asset))){b.value.usable_for_depth=false;b.value.recovering=true;}return b;},
            [this](j::object e){emit(std::move(e));},timing,c.execution_policy=="SHADOW");
    }
    void emit(j::object e) {
        const auto event_type=str(e,"type");
        if(event_type.find("FAILURE")!=std::string::npos || event_type.find("FREEZE")!=std::string::npos || event_type.find("MISMATCH")!=std::string::npos) e["severity"]="ERROR";
        else if(event_type=="FEED_RECOVERY" || event_type=="STALE_MARKET_DATA") e["severity"]="WARNING";
        else e["severity"]="INFO";
        e["input_index"]=index; trace+=j::serialize(e)+"\n";
        if(!recovering) {
            const auto type=str(e,"type");
            const bool critical=!c.operational || type.starts_with("MAKER_") || type=="STRATEGY_INTENT" || type=="ORDER_DECISION" || type=="RISK_RESERVED" ||
                type=="PLAN_PREPARED" || type=="SUBMIT_STARTED" || type=="ADAPTER_INVOKED" || type=="EXECUTION_REPORT" ||
                type=="RESERVATION_DISPOSITION" || type=="OMS_RESULT" || type=="SHADOW_WOULD_SUBMIT" || type=="SHADOW_RESERVATION_RELEASED" ||
                type=="GLOBAL_STOP" || str(e,"severity")=="ERROR" || (type=="RISK_DECISION" && flag(e,"approved"));
            // Before any economic event, force its raw-input dependency durable first.
            if(critical && input_journal)input_journal->flush();
            journal.append(e,critical);timing.observe("journal_write",journal.last_write_ns());
            if(journal.last_fsync_ns())timing.observe("journal_fsync",journal.last_fsync_ns());
        }
        const auto type=str(e,"type");
        if(type.find("FAILURE")!=std::string::npos || type.find("MISMATCH")!=std::string::npos || type.find("FREEZE")!=std::string::npos) e["severity"]="ERROR";
        else if(type=="STALE_MARKET_DATA" || type=="FEED_RECOVERY") e["severity"]="WARNING";
        auto keep=[&](j::array& a) { a.push_back(e); if(a.size()>100) a.erase(a.begin()); };
        if(type=="STRATEGY_INTENT" || (type=="UNIFIED_STRATEGY_REQUEST" && str(e,"request_type")=="Q")) keep(recent_intents);
        if(type=="RISK_DECISION" || type=="PORTFOLIO_REJECT" || type=="MAKER_RISK_RESERVED") keep(recent_risk);
        if(type=="PLAN_PREPARED" || type=="EXECUTION_REPORT" || type=="OMS_RESULT") keep(recent_orders);
        if(type.find("FAILURE")!=std::string::npos || type.find("FREEZE")!=std::string::npos ||
           type.find("MISMATCH")!=std::string::npos || type=="STALE_MARKET_DATA" || type=="FEED_RECOVERY") keep(incidents);
    }
    void publish(const execution::ExecutionReport& supplied) override {
        auto r=supplied;
        if(r.filled_lots>0 && r.fill_id.empty()) r.fill_id="SIMULATED:"+std::to_string(r.plan_id)+":"+std::to_string(r.child_order_id)+":"+std::to_string(r.event_ts_ns);

        // Persistence precedes accounting. Reports are delta fills, not cumulative counters.
        emit({{"type","EXECUTION_REPORT"},{"plan_id",r.plan_id},{"child_order_id",r.child_order_id},
              {"status",execution::to_string(r.status)},{"venue_order_id",r.venue_order_id},
              {"filled_lots",r.filled_lots},{"remaining_lots",r.remaining_lots},
              {"avg_fill_price_tick",r.avg_fill_price_tick},{"event_ts_ns",r.event_ts_ns},{"reject_reason",r.reject_reason},{"fill_id",r.fill_id},{"fee_tick",r.fee_tick},{"fill_source","SIMULATED_PAPER"}});
        if(fault=="crash_after_first_report" && !recovering) ::_exit(87);
        paper.on_execution_report(r);
        if(supervisor && r.filled_lots>0) {
            if(auto plan=gateway.find_plan(r.plan_id))for(std::uint16_t n=0;n<plan->order_count;++n)if(plan->orders[n].order_id==r.child_order_id)supervisor->atomic_fill(r,plan->orders[n]);
        }
    }
    void publish(const execution::ReservationDisposition& d) override {
        emit({{"type","RESERVATION_DISPOSITION"},{"reservation_id",d.reservation_id},
              {"plan_id",d.plan_id},{"disposition",static_cast<int>(d.type)},{"reason",d.reason}});
        paper.on_reservation_disposition(d);
        // PaperAtomic is the only enabled adapter policy. Any residual blocks further trading.
        if(d.type==execution::ReservationDispositionType::Release || d.type==execution::ReservationDispositionType::Consume)
            risk_engine.release_reservation(std::stoull(d.reservation_id));
    }
    risk::RiskLedgerSnapshot portfolio_risk() const {
        auto r=risk_engine.ledger_snapshot();
        for(auto& [asset,p]:paper.ledger().position_ledger().positions()) {
            r.reserved_asset_lots[asset]+=p.qty_lots;
            auto worst_loss=std::max<std::int64_t>(p.cost_basis_tick,0);
            r.total_reserved_exposure_tick+=worst_loss;
            r.reserved_market_exposure_tick[market_by_asset.at(asset)]+=worst_loss;
        }
        return r;
    }
    j::object account() const {
        const auto& ledger=paper.ledger(); j::array positions;
        for(auto& [asset,p]:ledger.position_ledger().positions()) positions.push_back(j::object{
            {"asset_id",asset},{"qty_lots",p.qty_lots},{"cost_basis_tick",p.cost_basis_tick}});
        j::object result{{"cash_tick",ledger.cash_ledger().cash_tick},{"positions",positions},
                {"reserved_cost_tick",risk_engine.ledger_snapshot().total_reserved_cost_tick}};
        if(supervisor)result["unified_financial_state"]=supervisor->financial_state();return result;
    }
    void intent(const signal::OpportunityIntent& original,const signal::SignalEvidenceView& evidence) {
        auto i=original;
        if(supervisor) {const auto& m=supervisor->active_metadata;const auto prefix=m.strategy_instance_id+":"+std::to_string(m.configuration_epoch)+":";
            i.intent_id=strategy_identity_hash(prefix+std::to_string(original.intent_id));i.idempotency_hash=strategy_identity_hash(prefix+std::to_string(original.idempotency_hash));i.idempotency_key=prefix+std::to_string(original.idempotency_hash);}
        paper.on_opportunity_intent(i);
        if(i.status!=signal::IntentStatus::PaperOpportunity) {
            ++signal_rejections;
            if(c.operational) {
                auto handoff=signal::make_signal_risk_handoff(i,evidence,now);auto input=risk::make_risk_input_view(handoff);
                auto audit=risk_engine.evaluate(input);if(audit.decision.approved())throw std::runtime_error("non-trading intent unexpectedly reserved risk");paper.on_risk_decision(audit.decision);
                emit({{"type","RISK_DECISION"},{"intent_id",i.intent_id},{"approved",false},{"reject_reason",static_cast<int>(audit.decision.reject_reason)},{"signal_status",static_cast<int>(i.status)}});
            }
            return;
        }
        ++opportunities;
        j::array intent_legs;
        for(std::uint16_t n=0;n<i.leg_count;++n) {
            const auto& leg=i.legs[n];
            intent_legs.push_back(j::object{{"market_id",leg.market_id},{"asset_id",leg.asset_id},
                {"asset_index",leg.asset_index},{"side",static_cast<int>(leg.side)},
                {"quantity_lots",leg.quantity_lots},{"requested_qty_lots",leg.requested_qty_lots},
                {"estimated_vwap_tick",leg.estimated_vwap_tick},{"worst_price_tick",leg.worst_price_tick}});
        }
        emit({{"type","STRATEGY_INTENT"},{"intent_id",i.intent_id},{"idempotency_hash",i.idempotency_hash},
              {"bundle_id",i.bundle_id},{"oracle_hash",i.oracle_artifact_hash},{"proof_hash",i.proof_hash},
              {"snapshot_hash",i.snapshot_version_hash},{"edge_tick",i.total_edge_tick},
              {"created_ts_ns",i.created_ts_ns},{"expires_at_ns",i.expires_at_ns},
              {"constraint_hash",i.constraint_hash},{"bundle_hash",i.bundle_hash},{"bundle_qty",i.bundle_qty},{"legs",intent_legs}});
        if(halted || (!facade && (!connected || fresh.size()!=universe.active_asset_ids.size()))) { ++stopped_intents; emit({{"type","SUPERVISOR_REJECT"},{"reason","stopped_or_snapshot_recovery"}}); return; }
        if(seen.size()>=100000) throw std::runtime_error("intent idempotency storage limit; retain evidence and stop");
        if(!seen.insert(i.idempotency_hash).second) { ++duplicates; emit({{"type","DUPLICATE_INTENT"},{"hash",i.idempotency_hash}}); return; }
        auto handoff=signal::make_signal_risk_handoff(i,evidence,now);
        auto ri=risk::make_risk_input_view(handoff); auto projection=portfolio_risk(); ri.ledger=&projection;
        const oracle::CandidateBundle* bundle=nullptr;
        for(auto& b:artifacts.active_bundles()) if(b.bundle_id==i.bundle_id) bundle=&b;
        if(!bundle || !evidence.depth_views) throw std::runtime_error("missing strategy evidence");
        auto decision=decision_engine.decide(i,*bundle,{evidence.depth_views,evidence.depth_view_count},c.policy,now);
        if(!decision.ok) { ++decision_rejections; emit({{"type","DECISION_REJECT"},{"intent_id",i.intent_id},{"reason_code",static_cast<int>(decision.reject_reason)},{"reason",decision.error}}); return; }
        auto& d=decision.decision;
        emit({{"type","ORDER_DECISION"},{"intent_id",i.intent_id},{"decision_hash",d.decision_hash},
            {"qty",d.chosen_bundle_qty},{"cost_tick",d.estimated_total_cost_tick},{"policy_hash",d.policy_hash}});
        if(supervisor) {auto admission=supervisor->check_bundle(d,i);if(!admission.ok) {++risk_rejections;emit({{"type","PORTFOLIO_REJECT"},{"reason",admission.reason},{"strategy_instance_id",supervisor->active_metadata.strategy_instance_id}});return;}}
        // Reuse existing exposure and inventory guards against accounted positions plus open reservations.
        auto sized=i;
        for(std::uint16_t n=0;n<d.leg_count;++n) { sized.legs[n].quantity_lots=d.legs[n].quantity_lots; sized.legs[n].estimated_cost_tick=d.legs[n].estimated_cost_tick; }
        risk::CostRevalidationResult cost; cost.ok=true; cost.risk_total_cost_tick=d.estimated_total_cost_tick;
        auto exposure=risk::ExposureGuard{}.check(projection,sized,cost,c.policy);
        auto inventory=risk::InventoryGuard{}.check(projection,sized,cost,c.policy);
        if(d.estimated_total_cost_tick>paper.ledger().cash_ledger().cash_tick-projection.total_reserved_cost_tick || !exposure.pass || !inventory.pass) {
            ++risk_rejections; emit({{"type","PORTFOLIO_REJECT"},{"cash_tick",paper.ledger().cash_ledger().cash_tick},
                {"reason",!exposure.pass?exposure.reason:!inventory.pass?inventory.reason:"insufficient funded cash"}}); return;
        }
        auto risk_started=monotonic_ns();
        auto r=risk_engine.evaluate_decision(i,d,ri); paper.on_risk_decision(r.decision);
        timing.observe("strategy_to_risk",monotonic_ns()-(strategy_started?strategy_started:risk_started));
        const auto risk_approved_at=monotonic_ns();
        emit({{"type","RISK_DECISION"},{"intent_id",i.intent_id},{"approved",r.decision.approved()},
              {"reject_reason",static_cast<int>(r.decision.reject_reason)},{"policy_hash",r.decision.policy_hash}});
        if(!r.decision.approved()) { ++risk_rejections; return; } ++approvals;
        if(supervisor) {std::vector<std::pair<std::string,std::int64_t>> sized;for(std::uint16_t n=0;n<d.leg_count;++n)sized.emplace_back(i.legs[n].asset_id,d.legs[n].quantity_lots);risk_engine.bind_actual_quantities(r.reservation.reservation_id,sized);}
        emit({{"type","RISK_RESERVED"},{"reservation_id",r.reservation.reservation_id},
              {"reserved_cost_tick",d.estimated_total_cost_tick},{"intent_id",i.intent_id}});
        auto approved=r.approved_intent;
        // Match the existing canonical handoff, retaining native IDs and hashes.
        approved.intent=i; if(approved.intent.idempotency_key.empty()) approved.intent.idempotency_key=std::to_string(i.idempotency_hash);
        approved.decision=r.decision; approved.reservation_id=std::to_string(r.reservation.reservation_id);
        approved.reservation_id_hash=r.reservation.reservation_id; approved.approved_at_ns=now; approved.expires_at_ns=i.expires_at_ns;
        paper.on_approved_intent(approved);
        auto envelope=order_decision::make_approved_order_decision_envelope(approved,d,now);
        execution::ExecutionContext ctx; ctx.now_ns=now; ctx.depth_views=evidence.depth_views; ctx.depth_view_count=evidence.depth_view_count;
        ctx.config.execution_enabled=true; ctx.config.mode=execution::ExecutionMode::Paper;
        ctx.config.paper_mode=execution::PaperExecutionMode::PaperAtomic; ctx.config.max_child_orders_per_plan=16;
        ctx.config.max_order_age_ns=c.stale_ns;
        auto prepared=gateway.prepare(envelope,ctx);
        if(!prepared.ok) throw std::runtime_error("OMS preparation rejected authorization");
        const auto& plan=prepared.plan; j::array orders;
        for(std::uint16_t n=0;n<plan.order_count;++n) {
            auto& o=plan.orders[n];orders.push_back(j::object{{"order_id",o.order_id},{"plan_id",o.plan_id},{"client_order_id",o.client_order_id},
              {"market_id",o.market_id},{"asset_id",o.asset_id},{"market_index",o.market_index},{"asset_index",o.asset_index},
              {"side",execution::to_string(o.side)},{"qty_lots",o.quantity_lots},{"limit_tick",o.limit_price_tick},
              {"estimated_vwap_tick",o.estimated_vwap_tick},{"worst_allowed_price_tick",o.worst_allowed_price_tick}});
        }
        authorized_plan=execution::ExecutionGateway::plan_identity(plan);adapter_authorized=false;
        emit({{"type","PLAN_PREPARED"},{"plan_id",plan.plan_id},{"source_intent_id",plan.source_intent_id},
              {"approved_intent_id",plan.approved_intent_id},{"reservation_id",plan.reservation_id},{"bundle_id",plan.bundle_id},
              {"idempotency_key",plan.idempotency_key},{"orders",orders},{"plan_sha256",sha256(authorized_plan)},
              {"max_total_cost_tick",plan.max_total_cost_tick},{"min_expected_edge_tick",plan.min_expected_edge_tick},
              {"max_slippage_tick",plan.max_slippage_tick},{"chosen_bundle_qty",plan.chosen_bundle_qty},
              {"guaranteed_payout_tick",plan.guaranteed_payout_tick},{"expected_terminal_pnl_tick",plan.expected_terminal_pnl_tick},
              {"created_ts_ns",plan.created_ts_ns},{"expire_after_ns",plan.expire_after_ns},
              {"authorization",j::object{{"decision_id",r.decision.decision_id},{"decision_hash",d.decision_hash},{"policy_hash",r.decision.policy_hash},
                {"reservation_id",r.reservation.reservation_id},{"reserved_cost_tick",d.estimated_total_cost_tick},{"approved_at_ns",now},{"expires_at_ns",i.expires_at_ns},{"proof_hash",i.proof_hash}}}});
        const auto durable_plan_time=monotonic_ns();durable_plan_at=durable_plan_time;timing.observe("risk_to_durable_plan",durable_plan_time-risk_approved_at);
        if(fault=="crash_after_plan_prepared" && !recovering) ::_exit(85);
        if(c.execution_policy=="SHADOW") {
            emit({{"type","SHADOW_WOULD_SUBMIT"},{"intent_id",i.intent_id},{"plan_id",plan.plan_id}});
            risk_engine.release_reservation(r.reservation.reservation_id);emit({{"type","SHADOW_RESERVATION_RELEASED"},{"reservation_id",r.reservation.reservation_id}});return;
        }
        emit({{"type","SUBMIT_STARTED"},{"intent_id",i.intent_id},{"plan_id",plan.plan_id},{"plan_sha256",sha256(authorized_plan)},
              {"reservation_id",r.reservation.reservation_id},{"adapter","PaperExecutionAdapter"}});
        adapter_authorized=true;
        if(fault=="crash_after_submit_started" && !recovering) ::_exit(86);

        auto result=gateway.submit_prepared(plan,envelope,ctx);++submissions;adapter_authorized=false;
        emit({{"type","OMS_RESULT"},{"plan_id",result.plan_id},{"status",execution::to_string(result.status)},{"ok",result.ok}});
        (void)gateway.poll();
        if(result.status!=execution::PlanStatus::Filled && result.status!=execution::PlanStatus::Failed && result.status!=execution::PlanStatus::Expired && result.status!=execution::PlanStatus::Cancelled) {
            halted=true; emit({{"type","UNRESOLVED_ORDER_FREEZE"},{"plan_id",result.plan_id}});
        }
        if(!supervisor && risk_engine.ledger_snapshot().active_reservations!=0) { halted=true; emit({{"type","RESERVATION_MISMATCH_FREEZE"}}); }
    }
    void dispatch(const j::object& row,bool market_event) {
        if(supervisor)supervisor->boundary(now,sha256(str(row,"raw_payload",row.if_contains("payload")?j::serialize(row.at("payload")):std::to_string(index))),market_event,row,*plugin,[this](auto& i,auto& e){intent(i,e);});
        else {strategy_started=monotonic_ns();++strategy_callbacks;plugin->on_market_update(now,[this](auto& i,auto& e){intent(i,e);if(fault=="duplicate_intent")intent(i,e);});}
    }
    void process(const j::object& row,std::size_t idx,bool replay) {
        index=idx; recovering=replay; trace.clear(); intent_publisher.clear(); ++records;
        auto stamp=num(row,"recv_ns"); if(stamp<=0 || static_cast<std::uint64_t>(stamp)<now) throw std::runtime_error("feed monotonic clock regressed or missing");
        now=stamp; auto kind=str(row,"kind","packet"); fault=str(row,"test_fault");
        if(!fault.empty() && !c.fault_tests) throw std::runtime_error("test faults disabled");
        if(fault=="strategy_exception") throw std::runtime_error("injected strategy exception");
        if(fault=="disk_full") journal.inject_fault("ENOSPC");
        if(fault=="partial_journal_write") journal.inject_fault("PARTIAL_WRITE");
        if(kind=="venue_packet") {
            if(!facade) throw std::runtime_error("venue packet requires configured facade");
            auto record=row;
            const auto received=num(row,"callback_recv_monotonic_ns",now);
            if(received<=0 || static_cast<std::uint64_t>(received)>now) throw std::runtime_error("future or invalid local receive timestamp");
            // Processing availability is not a freshness refresh: retain physical receive time.
            record["recv_monotonic_ns"]=received;record["availability_monotonic_ns"]=now;
            record["raw_hash"]=sha256(str(row,"raw_payload"));
            auto decode_start=monotonic_ns();
            auto callback=num(row,"callback_recv_monotonic_ns");
            if(callback>0 && decode_start>=static_cast<std::uint64_t>(callback))timing.observe("ws_receive_to_decode",decode_start-callback);
            const bool valid=facade->process(record);++normalized;
            timing.observe("decode_to_book_state",monotonic_ns()-decode_start);
            if(facade->last_duplicate()) {emit({{"type","DUPLICATE_MARKET_MESSAGE"},{"venue",str(row,"venue")}});return;}
            if(!valid) rebuild_venue=str(row,"venue");
            if(!valid) emit({{"type","VENUE_INTEGRITY_FAILURE"},{"venue",str(row,"venue")}});
            auto callback_start=monotonic_ns();strategy_started=callback_start;timing.observe("book_update_to_strategy",callback_start-decode_start);
            dispatch(row,str(row,"transport")=="ws" && str(row,"classification")=="market_data");
            timing.observe("strategy_callback",monotonic_ns()-callback_start);
            for(auto& asset:universe.active_asset_ids) {auto b=facade->snapshot(asset,now);if(b.ok)paper.on_mark_snapshot(b.value);}
            emit({{"type","VENUE_STATE"},{"venue",str(row,"venue")},{"classification",str(row,"classification")},{"message_processed",valid}});
            return;
        }
        if(kind=="disconnect" || kind=="reconnect") {
            connected=kind=="reconnect"; fresh.clear(); ++reconnects;
            emit({{"type","FEED_RECOVERY"},{"connected",connected},{"fresh_snapshots_required",true}});if(supervisor)dispatch(row,false);return;
        }
        if(kind=="stop" || kind=="kill") { halted=true;if(supervisor)supervisor->stop(kind);emit({{"type","GLOBAL_STOP"},{"reason",kind}});return; }
        if(kind=="maker_control") {if(!supervisor)throw std::runtime_error("maker control requires unified runtime");supervisor->control(row,now,c.fault_tests);return;}
        if(kind=="account_snapshot") {
            const auto got=row.at("account").as_object();
            if(got!=account()) { halted=true;if(supervisor)supervisor->stop("ACCOUNT_MISMATCH");++mismatches; emit({{"type","RECONCILIATION_MISMATCH"},{"expected",account()},{"reported",got}}); }
            else emit({{"type","RECONCILIATION_MATCH"},{"source","paper_test_account_snapshot"}});
            return;
        }
        if(kind=="tick") {
            std::uint64_t stale=0;
            for(auto& asset:universe.active_asset_ids) {
                auto b=facade?facade->snapshot(asset,now):view.get_snapshot(asset);
                if(!b.ok || now<b.value.last_book_update_ns || now-b.value.last_book_update_ns>c.stale_ns) ++stale;
            }
            if(stale) { ++stale_events; emit({{"type","STALE_MARKET_DATA"},{"stale_assets",stale}}); }
            dispatch(row,false);return;
        }
        if(kind!="packet") throw std::runtime_error("unsupported feed control event");
        feed::RawPacket p;
        p.header.packet_id=num(row,"packet_id",idx+1); p.header.connection_id=num(row,"connection_id",1);
        p.header.recv_monotonic_ns=now; p.header.recv_wall_ns=num(row,"wall_ns",now); p.header.source_id=feed::SourceId::PolymarketMarket;
        p.payload=row.at("payload").is_string()?std::string(row.at("payload").as_string()):j::serialize(row.at("payload"));
        if(connection && p.header.connection_id!=connection) { fresh.clear(); ++reconnects; }
        connection=p.header.connection_id;
        decode::NormalizedEventBatch batch;
        auto decoded=decoder.decode(feed::to_decode_input_view(p),&batch);
        if(!decoded.ok() && decoded.payload_kind!=decode::JsonDecodeKind::NonJsonControl) {
            ++decode_errors; fresh.clear(); emit({{"type","DECODE_FAILURE"}});if(supervisor)dispatch(row,false);return;
        }
        for(auto& ev:state::from_normalized_batch(batch)) {
            ++normalized;
            if(!filter->filter(ev).passed()) continue;
            auto applied=store.apply(ev);
            if(!applied.ok()) { ++state_errors; fresh.clear(); emit({{"type","STATE_FAILURE"}}); continue; }
            if(ev.type==state::MarketStateEventType::WsBookSnapshot) fresh.insert(ev.asset_id);
            auto b=view.get_snapshot(ev.asset_id); if(b.ok) paper.on_mark_snapshot(b.value);
            if(!supervisor)dispatch(row,true);
        }
        if(supervisor)dispatch(row,true);
        emit({{"type","MARKET_STATE"},{"state_hash",view.global_hash()},{"normalized",normalized}});
    }
    void restore_pending(const std::vector<j::object>& history) {
        std::size_t begin=history.size();
        for(std::size_t n=0;n<history.size();++n) {
            if(str(history[n],"type")=="INPUT_BEGIN") begin=n;
            if(str(history[n],"type")=="INPUT_COMMIT") begin=history.size();
        }
        bool started=false;
        for(std::size_t n=begin;n<history.size();++n) {
            auto& e=history[n]; auto type=str(e,"type");
            if(type=="SUBMIT_STARTED") started=true;
            if(type=="RISK_RESERVED") pending_reserve+=num(e,"reserved_cost_tick");
            if(type=="PLAN_PREPARED") {
                execution::OrderPlan p; p.plan_id=uid(e,"plan_id"); p.source_intent_id=uid(e,"source_intent_id");
                p.reservation_id=num(e,"reservation_id");
                for(auto& ov:e.at("orders").as_array()) {
                    if(p.order_count>=execution::kMaxChildOrdersPerPlan) throw std::runtime_error("invalid recovered plan size");
                    auto& o=p.orders[p.order_count++]; auto& v=ov.as_object();
                    o.order_id=num(v,"order_id"); o.plan_id=p.plan_id; o.asset_id=str(v,"asset_id");
                    o.asset_index=num(v,"asset_index"); o.market_id=str(v,"market_id"); o.quantity_lots=num(v,"qty_lots");
                    o.limit_price_tick=num(v,"limit_tick"); o.client_order_id=str(v,"client_order_id");
                    o.side=str(v,"side")=="Sell"?execution::OrderSide::Sell:execution::OrderSide::Buy;
                }
                paper.on_order_plan(p);
            }
            if(type=="EXECUTION_REPORT") {
                execution::ExecutionReport r; r.plan_id=uid(e,"plan_id"); r.child_order_id=num(e,"child_order_id");
                r.filled_lots=num(e,"filled_lots"); r.remaining_lots=num(e,"remaining_lots");
                r.avg_fill_price_tick=num(e,"avg_fill_price_tick"); r.event_ts_ns=num(e,"event_ts_ns");r.fill_id=str(e,"fill_id");r.fee_tick=num(e,"fee_tick");
                r.status=str(e,"status")=="Filled"?execution::ChildOrderStatus::Filled:
                    str(e,"status")=="PartiallyFilled"?execution::ChildOrderStatus::PartiallyFilled:execution::ChildOrderStatus::Failed;
                paper.on_execution_report(r); ++pending_reports;
            }
        }
        recovery_submission_state=started?"ACK_UNKNOWN_QUERY_BEFORE_RETRY":"NOT_SUBMITTED";
        if(!started && pending_reports==0) pending_reserve=0;
        if(supervisor && begin<history.size()) {
            std::vector<j::object> tail(history.begin()+begin,history.end());supervisor->restore_tail(tail);
            bool maker_started=false;for(const auto& e:tail)if(str(e,"type")=="MAKER_SUBMIT_STARTED")maker_started=true;
            if(maker_started)recovery_submission_state="ACK_UNKNOWN_QUERY_BEFORE_RETRY";
        }
    }
        // Preparation is side-effect free; absence of the durable submit marker proves no adapter call.
    void telemetry(std::string lifecycle, bool recovery_incomplete=false) {
        auto snapshot=paper.latest_dashboard_snapshot();
        snapshot.account.reserved_cash_tick=risk_engine.ledger_snapshot().total_reserved_cost_tick;
        auto obj=j::parse(paper_backend::dashboard_snapshot_json(snapshot)).as_object();
        j::array markets;
        for(auto& asset:universe.active_asset_ids) {
            auto b=facade?facade->snapshot(asset,now):view.get_snapshot(asset);
            markets.push_back(j::object{{"asset_id",asset},{"market_id",market_by_asset.at(asset)},
                {"book_present",b.ok},{"usable_for_depth",b.ok && b.value.usable_for_depth},
                {"stale",!b.ok || now<b.value.last_book_update_ns || now-b.value.last_book_update_ns>c.stale_ns}});
        }
        obj["markets"]=markets; obj["intents"]=recent_intents; obj["risk_decisions"]=recent_risk;
        obj["order_states"]=recent_orders; obj["incidents"]=incidents;
        obj["latency"]=j::object{{"end_to_end_ns",last_latency_ns},{"scope","local_processing_including_durable_journal"}};
        auto risk=portfolio_risk();
        j::array positions=account().at("positions").as_array();
        obj["runtime"]=j::object{{"schema_version",1},{"mode",c.mode},{"lifecycle",lifecycle},{"strategy","combinatorial"},
            {"strategy_lifecycle",(halted || lifecycle!="RUNNING")?"STOPPED":"RUNNING"},{"live_enabled",false},{"adapter","PaperExecutionAdapter"},
            {"feed_kind",c.format=="live_ws"?"LIVE_WEBSOCKET":c.format+"_recorded"},{"feed_connected",connected},{"fresh_snapshot_assets",fresh.size()},
            {"required_snapshot_assets",universe.active_asset_ids.size()},{"records",records},{"normalized_events",normalized},
            {"decode_errors",decode_errors},{"state_errors",state_errors},{"stale_events",stale_events},
            {"reconnects",reconnects},{"opportunities",opportunities},{"approvals",approvals},{"paper_submissions",submissions},
            {"signal_rejections",signal_rejections},{"decision_rejections",decision_rejections},{"risk_rejections",risk_rejections},
            {"stopped_intents",stopped_intents},{"duplicate_intents",duplicates},{"reconciliation_mismatches",mismatches},
            {"reconciliation_status",recovery_incomplete?"INCOMPLETE_JOURNAL_REVIEW_REQUIRED":mismatches?"MISMATCH":"INTERNAL_PAPER_ONLY"},
            {"recovery_incomplete",recovery_incomplete},{"pending_recovery_encumbrance_tick",pending_reserve},
            {"pending_durable_reports_restored",pending_reports},{"positions",positions},{"cash_tick",paper.ledger().cash_ledger().cash_tick},
            {"active_reservations",risk_engine.ledger_snapshot().active_reservations},{"reserved_cost_tick",risk.total_reserved_cost_tick},
            {"portfolio_cost_exposure_tick",risk.total_reserved_exposure_tick},{"config_sha256",c.config_hash},
            {"input_sha256",c.input_hash},{"artifact_sha256",c.artifact_hash},{"binary_sha256",c.binary_hash},{"journal_head",journal.head()},
            {"verified_live_realized_pnl",nullptr},{"exchange_account_reconciled",false}};
        obj["runtime"].as_object()["release_version"]=c.multi?"0.2.1":"0.2.0";
        if(supervisor) {auto unified=supervisor->telemetry();for(const auto& item:unified)obj[std::string(item.key())]=item.value();obj["runtime"].as_object()["strategy"]="multi_strategy";obj["runtime"].as_object()["portfolio_frozen"]=supervisor->frozen();}
        obj["runtime"].as_object()["health"]=halted||recovery_incomplete?"DEGRADED":lifecycle!="RUNNING"?"STOPPED":facade?operational_health():"RECORDED_TEST";
        if(supervisor) {
            if(supervisor->frozen())obj["runtime"].as_object()["health"]="DEGRADED";
            obj["runtime"].as_object()["adapter"]="PaperAtomic + ManagedPaperMaker";
            if(facade)for(const auto* key:{"feed_connected","fresh_snapshot_assets","required_snapshot_assets","reconnects","decode_errors","state_errors"})obj["runtime"].as_object().erase(key);
            if(auto strategies=obj.if_contains("strategies"))for(auto& value:strategies->as_array()) {
                auto& row=value.as_object();if(str(row,"lifecycle")=="RUNNING") {
                    if(lifecycle!="RUNNING")row["lifecycle"]="STOPPED";
                    else if(supervisor->frozen())row["lifecycle"]="SUSPENDED_PORTFOLIO";
                }
            }
        }
        obj["runtime"].as_object()["execution_policy"]=c.execution_policy;
        obj["runtime"].as_object()["strategy_callbacks"]=strategy_callbacks;
        obj["runtime"].as_object()["recovery_submission_state"]=recovery_submission_state;
        obj["runtime"].as_object()["available_cash_tick"]=paper.ledger().cash_ledger().cash_tick-risk.total_reserved_cost_tick-pending_reserve;
        obj["runtime"].as_object()["logical_now_ns"]=now;
        obj["runtime"].as_object()["config_version"]=c.policy.policy_version;
        obj["runtime"].as_object()["fees_paid_tick"]=paper.ledger().cash_ledger().fees_paid_tick;
        obj["runtime"].as_object()["journal_integrity"]="VERIFIED_PREFIX";
        obj["runtime"].as_object()["journal_durable_head"]=journal.durable_head();
        obj["runtime"].as_object()["market_audit_checkpoint_ms"]=c.operational?250:0;
        obj["runtime"].as_object()["economic_events_synchronously_durable"]=true;
        obj["runtime"].as_object()["stop_latched"]=halted;
        obj["runtime"].as_object()["source_health"]=source_health;
        if(facade) {obj["markets"]=facade->health(now);obj["runtime"].as_object()["venue_counters"]=facade->counters();}
        obj["account"].as_object()["account_id"]="SIMULATED:"+c.execution_policy;
        obj["runtime"].as_object()["private_reconciliation_status"]="UNCONFIGURED";
        obj["published_wall_ns"]=feed::now_wall_ns();
        obj["latency_metrics"]=timing.report();
        const auto publish_start=monotonic_ns();atomic_json(c.out/"dashboard.json",obj);
        if(c.operational) {
            std::ofstream history(c.out/"health_history.jsonl",std::ios::app);
            history.exceptions(std::ios::failbit|std::ios::badbit);
            history<<j::serialize(j::object{{"logical_now_ns",now},{"published_wall_ns",obj.at("published_wall_ns")},
                {"health",obj.at("runtime").as_object().at("health")},{"lifecycle",lifecycle},
                {"markets",obj.at("markets")},{"source_health",source_health},{"cash_tick",paper.ledger().cash_ledger().cash_tick},
                {"reserved_cost_tick",risk.total_reserved_cost_tick}})<<'\n';
        }
        timing.observe("dashboard_publication",monotonic_ns()-publish_start);
        atomic_json(c.out/"latency.json",timing.report());
    }
    std::string operational_health() const {
        if(!facade)return "RECORDED_TEST";
        auto health=facade->health(now);for(auto& v:health)if(!flag(v.as_object(),"admitted"))return "DEGRADED";
        return health.empty()?"DEGRADED":"HEALTHY";
    }
    DurableJournal* input_journal=nullptr;
    j::object source_health;LatencyMetrics timing;
    std::string authorized_plan,recovery_submission_state="NONE";bool adapter_authorized=false;
    j::array recent_intents,recent_risk,recent_orders,incidents;
    std::string trace,fault,rebuild_venue; bool recovering=false,halted=false,connected=true;
    std::size_t index=0; std::uint64_t now=0,last_latency_ns=0;
private:
    const Config& c; DurableJournal& journal;
    state::StateUniverse universe; state::MarketStateStore store; state::MarketStateView view;
    std::unique_ptr<state::MarketStateEventFilter> filter;
    std::unique_ptr<VenueMarketFacade> facade;
    decode::DecodePipeline decoder;
    signal::OracleArtifactReader artifacts; signal::MarketStateViewSnapshotReader snapshot_reader;
    signal::SettlementMaskChecker settlement; signal::VWAPPrecheck vwap; signal::FeeModel fees{0};
    signal::LatencyBufferModel latency{0}; signal::TheoreticalEdgeCalculator edge; signal::OpportunityRanker ranker;
    signal::CapturingIntentPublisher intent_publisher;
    std::unique_ptr<signal::SignalEngine> engine; std::unique_ptr<StrategyPlugin> plugin;
    std::unique_ptr<StrategySupervisor> supervisor;
    order_decision::OrderDecisionEngine decision_engine; risk::RiskEngine risk_engine;
    paper::PaperTradingEngine paper; execution::PaperExecutionAdapter adapter; execution::ExecutionGateway gateway;
    std::set<std::uint64_t> seen; std::set<std::string> fresh; std::map<std::string,std::string> market_by_asset;
    std::int64_t pending_reserve=0; std::uint64_t pending_reports=0;
    std::uint64_t strategy_callbacks=0,strategy_started=0,durable_plan_at=0;
    std::uint64_t connection=0,records=0,normalized=0,decode_errors=0,state_errors=0,stale_events=0,reconnects=0,
        opportunities=0,approvals=0,submissions=0,signal_rejections=0,decision_rejections=0,risk_rejections=0,stopped_intents=0,duplicates=0,mismatches=0;
};
}
int run(const fs::path& path,bool recover) {
    auto c=load(path); auto rows=inputs(c);
    fs::create_directories(c.out);
    DurableJournal journal(c.out/"events.jsonl");
    j::object frozen{{"type","RUN_CONFIG"},{"config_sha256",c.config_hash},{"input_sha256",c.input_hash},
        {"artifact_sha256",c.artifact_hash},{"binary_sha256",c.binary_hash},{"policy_hash",c.policy.policy_hash},{"mode",c.mode},{"live_enabled",false}};
    auto& history=journal.recovered();
    if(c.operational) {
        frozen["release_version"]=c.multi?"0.2.1":"0.2.0";frozen["markets_sha256"]=c.market_hash;
        frozen["started_wall_ns"]=history.empty()?feed::now_wall_ns():uid(history.front(),"started_wall_ns");
    }
    if(!history.empty() && !recover) throw std::runtime_error("existing journal requires --recover");
    if(history.empty() && recover) throw std::runtime_error("cannot recover empty journal");
    if(history.empty()) { journal.append(frozen); atomic_json(c.out/"frozen_config.json",c.raw); }
    else if(j::serialize(history.front())!=j::serialize(frozen)) throw std::runtime_error("frozen config, feed or rule artifact changed");
    Session session(c,journal);
    std::vector<std::string> commits,account_digests; bool open=false; std::size_t current=0; std::string recorded_trace; bool stop_latched=false;
    for(std::size_t n=1;n<history.size();++n) {
        auto& e=history[n]; auto type=str(e,"type");
        if(type=="INPUT_BEGIN") {
            if(open || num(e,"input_index")!=static_cast<std::int64_t>(commits.size())) throw std::runtime_error("invalid input transaction order");
            open=true; current=commits.size(); recorded_trace.clear();
            if(current>=rows.size() || e.at("input_sha256").as_string()!=sha256(j::serialize(rows[current]))) throw std::runtime_error("input identity mismatch");
        } else if(type=="INPUT_COMMIT") {
            if(!open || num(e,"input_index")!=static_cast<std::int64_t>(current) ||
               e.at("events_sha256").as_string()!=sha256(recorded_trace)) throw std::runtime_error("journal transaction mismatch");
            commits.push_back(str(e,"events_sha256")); account_digests.push_back(str(e,"account_sha256")); open=false;
        } else if(type=="RUNTIME_STOPPED") {
            if(str(e,"reason")=="supervisor_stop") stop_latched=true;
        } else if(type!="RUNTIME_STARTED") {
            if(!open) throw std::runtime_error("event outside input transaction");
            recorded_trace+=j::serialize(e)+"\n";
        }
    }
    for(std::size_t i=0;i<commits.size();++i) {
        session.process(rows[i],i,true);
        if(sha256(session.trace)!=commits[i] || sha256(j::serialize(session.account()))!=account_digests[i])
            throw std::runtime_error("deterministic event or account recovery diverged");
    }
    if(open) {
        session.restore_pending(history);
        session.halted=true; session.telemetry("RECOVERY_BLOCKED",true);
        std::cerr<<"uncommitted transaction retained; no retry or reservation release; inspect journal\n";
        return 2;
    }
    if(stop_latched || session.halted) { session.halted=true; session.telemetry("STOP_LATCHED"); return 2; }
    stopping=0; std::signal(SIGINT,signal_stop); std::signal(SIGTERM,signal_stop);
    journal.append({{"type","RUNTIME_STARTED"},{"recovered_inputs",commits.size()}});
    atomic_text(c.out/"runtime.pid",std::to_string(::getpid())+"\n");
    struct RemovePid {
        fs::path path;
        ~RemovePid() { std::error_code error; fs::remove(path,error); }
    } remove_pid{c.out/"runtime.pid"};
    const auto session_start=monotonic_ns();
    try {
        std::size_t index=commits.size();
        std::uint64_t last_publish=0,last_checkpoint=0;
        auto transaction=[&](const j::object& row,bool raw_durable=false) {
            if(index>=static_cast<std::size_t>(c.max_records)) throw std::runtime_error("record budget exhausted");
            auto start=monotonic_ns();
            journal.append({{"type","INPUT_BEGIN"},{"input_index",index},{"input_sha256",sha256(j::serialize(row))}},!c.operational);
            session.process(row,index,false);
            journal.append({{"type","INPUT_COMMIT"},{"input_index",index},{"events_sha256",sha256(session.trace)},
                {"account_sha256",sha256(j::serialize(session.account()))}},!c.operational);
            if(c.operational && monotonic_ns()-last_checkpoint>=250'000'000ULL) {
                if(session.input_journal && session.input_journal->dirty()) {
                    session.input_journal->flush();session.timing.observe("raw_fsync",session.input_journal->last_fsync_ns());
                }
                if(journal.dirty()){journal.flush();session.timing.observe("journal_fsync",journal.last_fsync_ns());}
                last_checkpoint=monotonic_ns();
            }
            ++index;session.last_latency_ns=monotonic_ns()-start;
            session.timing.observe("receive_to_decode",num(row,"callback_recv_monotonic_ns")>0?start-num(row,"callback_recv_monotonic_ns"):0);
            session.timing.observe("end_to_end",session.last_latency_ns);
            if(!c.operational || monotonic_ns()-last_publish>=1'000'000'000ULL) {session.telemetry("RUNNING");last_publish=monotonic_ns();}
        };
        for(;index<rows.size() && !stopping && !session.halted;) {
            if(!c.kill.empty() && fs::exists(c.kill)) {session.halted=true;break;}
            transaction(rows[index]);
        }
        if(c.format=="live_ws" && !stopping && !session.halted) {
            const auto started=uid(frozen,"started_wall_ns");const auto elapsed=(feed::now_wall_ns()-started)/1'000'000'000ULL;
            if(elapsed<c.duration) {
                std::uint64_t epoch_seed=0;
                for(auto& row:rows)epoch_seed=std::max(epoch_seed,static_cast<std::uint64_t>(num(row,"connection_epoch")));
                DurableJournal raw(c.input);session.input_journal=&raw;
                LivePacketSource live(c.markets,c.queue_capacity,c.duration-elapsed,flag(c.raw,"require_kalshi_auth",true),
                    num(c.raw,"diagnostic_reconnect_after_seconds"),epoch_seed);
                auto next_timer=monotonic_ns()+250'000'000ULL;
                while(!stopping && !session.halted && !live.finished()) {
                    session.source_health=live.health();
                    if(flag(session.source_health,"overflow")) throw std::runtime_error("LIVE_FEED_BACKPRESSURE_CRITICAL");
                    if(!c.kill.empty() && fs::exists(c.kill)) {session.halted=true;break;}
                    j::object row;
                    if(monotonic_ns()>=next_timer) {row={{"kind","tick"}};next_timer=monotonic_ns()+250'000'000ULL;}
                    else if(!live.pop(row,50)) continue;
                    row["recv_ns"]=monotonic_ns();
                    const auto persist_start=monotonic_ns();raw.append(row,false);
                    session.timing.observe("raw_persistence",monotonic_ns()-persist_start);
                    transaction(row,true);
                    if(!session.rebuild_venue.empty()) {live.request_rebuild(session.rebuild_venue);session.rebuild_venue.clear();}
                }
                live.stop();session.source_health=live.health();
                for(auto venue:{"kalshi","polymarket_international"}) {
                    j::object closing{{"kind","venue_packet"},{"venue",venue},{"transport","event"},{"classification","disconnect"},
                        {"raw_payload","CONTROLLED_RUNTIME_SHUTDOWN"},{"recv_ns",monotonic_ns()},{"recv_wall_ns",feed::now_wall_ns()},{"connection_epoch",uid(session.source_health.at(venue).as_object(),"epoch")}};
                    raw.append(closing,false);transaction(closing,true);
                }
                raw.flush();journal.flush();session.input_journal=nullptr;
                atomic_json(c.out/"recording_manifest.json",j::object{{"input_sha256",file_sha256(c.input)},
                    {"raw_journal_head",raw.head()},{"records",index},{"actual_elapsed_ns",monotonic_ns()-session_start},
                    {"requested_seconds",c.duration},{"markets_sha256",c.market_hash},{"release_version",c.multi?"0.2.1":"0.2.0"}});
            }
        }
        journal.append({{"type","RUNTIME_STOPPED"},{"reason",session.halted?"supervisor_stop":stopping?"signal":"bounded_input_complete"}});
        session.telemetry(session.halted?"STOPPED_BY_SUPERVISOR":"STOPPED");
        std::cout<<"Runtime stopped safely. Evidence: "<<c.out<<'\n';
        return session.halted?2:0;
    } catch(...) { session.halted=true;try{session.emit({{"type","RUNTIME_FAILURE"},{"reason","UNRESOLVED_RUNTIME_OPERATION"}});}catch(...){} try{session.telemetry("FAILED_REQUIRES_RECOVERY",true);}catch(...){} throw; }
}
}

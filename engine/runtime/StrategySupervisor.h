#pragma once
#include "engine/runtime/MakerPlugin.h"
#include "engine/runtime/LatencyMetrics.h"
#include "engine/execution/core/ExecutionGateway.h"
#include "engine/paper/core/PaperTradingEngine.h"
#include "engine/risk/core/RiskEngine.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/state/MarketStateQueryResult.h"
#include <memory>

namespace trading_engine::runtime {
class StrategySupervisor {
public:
    using Emit=std::function<void(boost::json::object)>;
    using Snapshot=std::function<state::StateQueryResult<state::MarketStateSnapshot>(const std::string&)>;
    StrategySupervisor(const boost::json::object&,risk::RiskEngine&,execution::ExecutionGateway&,
        paper::PaperTradingEngine&,Snapshot,Emit,LatencyMetrics&,bool shadow);
    void boundary(std::uint64_t now,const std::string& event_id,bool market_event,const boost::json::object& input,
        StrategyPlugin& combinatorial,const IntentSink&);
    risk::PortfolioAdmission check_bundle(const order_decision::OrderDecisionLite&,const signal::OpportunityIntent&) const;
    void atomic_fill(const execution::ExecutionReport&,const execution::ChildOrder&);
    void stop(const std::string& reason);
    void control(const boost::json::object& input,std::uint64_t now,bool allow_test_faults);
    void restore_tail(const std::vector<boost::json::object>&);
    boost::json::object financial_state() const;
    boost::json::object telemetry() const;
    StrategyRequestMetadata active_metadata;
    bool frozen() const {return frozen_;}
    const std::string& freeze_reason() const {return freeze_reason_;}
private:
    struct Instrument {std::string asset,market,venue,account,currency;std::uint32_t asset_index=0,market_index=0;std::int64_t tick=1,step=1,min_qty=1;};
    struct Instance {StrategyRequestMetadata identity;int priority=0;std::uint64_t budget_ns=50'000'000;bool enabled=true,manual_test_requests=false;
        std::string lifecycle="RUNNING",error;std::vector<std::string> assets;std::unique_ptr<MakerPlugin> maker;
        execution::MakerReplacementPolicy replacement=execution::MakerReplacementPolicy::CancelThenNew;};
    void request(const StrategyRequest&,const signal::SignalEvidenceView* evidence=nullptr,const IntentSink* combination_sink=nullptr);
    void quote(const StrategyRequest&);
    void cancel(const StrategyRequest&);
    void on_report(const execution::MakerOrder&,const execution::MakerOrderReport&);
    void before_effect(const std::string&,const execution::MakerOrder&);
    void account_fill(const execution::MakerOrder&,const execution::MakerOrderReport&);
    risk::PortfolioAccountView account_view() const;
    risk::PortfolioLeg resource(const std::string&,bool,std::int64_t,std::int64_t) const;
    Instance& instance(const std::string&);
    void reject(const StrategyRequest&,const std::string&);
    void reconcile();
    risk::RiskEngine& risk_;execution::ExecutionGateway& oms_;paper::PaperTradingEngine& paper_;
    Snapshot snapshot_;Emit emit_;LatencyMetrics& timing_;bool shadow_=false,frozen_=false;std::string freeze_reason_;
    std::map<std::string,Instrument> instruments_;std::vector<Instance> instances_;
    risk::PortfolioLimits limits_;risk::QuoteRiskPolicy quote_policy_;risk::QuoteRiskEvaluator quote_risk_;
    execution::PaperMakerExecutionAdapter adapter_;execution::ManagedMakerConfig simulation_;
    std::map<std::string,StrategyRequest> replacements_;
    std::map<std::string,std::string> request_keys_;
    std::map<std::string,std::int64_t> strategy_cost_,strategy_cashflow_,strategy_fees_;
    std::map<std::string,std::map<std::string,std::int64_t>> attributed_lots_;
    std::map<std::string,std::uint64_t> prepared_at_;
    std::uint64_t now_=0;bool test_faults_=false,record_frames_=false,round_ticks_=false;std::string fault_;boost::json::object last_input_;
};
}

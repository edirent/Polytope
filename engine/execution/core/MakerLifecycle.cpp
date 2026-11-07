#include "engine/execution/core/ExecutionGateway.h"
#include <algorithm>
#include <stdexcept>

namespace trading_engine::execution {
namespace j=boost::json;
namespace {
std::string s(const j::object& o,const char* k){return std::string(o.at(k).as_string());}
std::uint64_t u(const j::object& o,const char* k){return o.at(k).to_number<std::uint64_t>();}
std::int64_t n(const j::object& o,const char* k){return o.at(k).to_number<std::int64_t>();}
j::object metadata_json(const StrategyRequestMetadata& m) {
    return {{"strategy_id",m.strategy_id},{"strategy_instance_id",m.strategy_instance_id},{"configuration_epoch",m.configuration_epoch},
        {"model_version",m.model_version},{"market_state_version",m.market_state_version},{"request_id",m.request_id},
        {"idempotency_key",m.idempotency_key},{"source_timestamp",m.source_timestamp},{"availability_ns",m.availability_ns}};
}
}
bool maker_terminal(MakerOrderStatus x) {return x==MakerOrderStatus::Filled || x==MakerOrderStatus::Canceled || x==MakerOrderStatus::Rejected || x==MakerOrderStatus::ExpiredConfirmed;}
const char* maker_status_name(MakerOrderStatus x) {
    static const char* names[]={"CREATED","PREPARED","RISK_RESERVED","SUBMIT_PENDING","ACK_UNKNOWN","ACKNOWLEDGED","OPEN","PARTIALLY_FILLED",
        "CANCEL_REQUESTED","CANCEL_PENDING","REPLACE_PENDING","FILLED","CANCELED","REJECTED","EXPIRED_CONFIRMED","RECOVERY_BLOCKED"};
    return names[static_cast<int>(x)];
}
const char* maker_report_name(MakerReportType x) {
    static const char* names[]={"ACK","FILL","CANCEL_ACK","CANCEL_REJECT","REJECT","EXPIRE_CONFIRMED","TIMEOUT"};return names[static_cast<int>(x)];
}
j::object maker_order_json(const MakerOrder& o) {
    const auto& l=o.resource;
    const auto& leg=l.buy?o.quote.bid:o.quote.ask;
    return {{"order_id",o.order_id},{"client_order_id",o.client_order_id},{"venue_order_id",o.venue_order_id},{"authorization_hash",o.authorization_hash},
        {"request",metadata_json(o.request)},{"asset_id",l.asset},{"market_id",l.market},{"event_id",l.event},{"venue",l.venue},{"account",l.account},
        {"settlement_asset",l.settlement_asset},{"buy",l.buy},{"price_tick",l.price_tick},{"fee_per_lot_tick",l.fee_per_lot_tick},
        {"reservation_id",o.reservation_id},{"original_qty",o.original_qty},{"remaining_qty",o.remaining_qty},{"filled_qty",o.filled_qty},
        {"open_qty",maker_terminal(o.status)?0:o.remaining_qty},{"cancel_pending_qty",o.status==MakerOrderStatus::CancelPending?o.remaining_qty:0},
        {"unknown_qty",o.status==MakerOrderStatus::AckUnknown || o.status==MakerOrderStatus::RecoveryBlocked?o.remaining_qty:0},
        {"created_ns",o.created_ns},{"ack_due_ns",o.ack_due_ns},{"cancel_due_ns",o.cancel_due_ns},{"last_ack_ns",o.last_ack_ns},
        {"status",maker_status_name(o.status)},{"acknowledged",o.acknowledged},{"last_evidence",o.last_evidence},
        {"quote_group_id",o.quote.quote_group_id},{"approved_quote_id",o.quote.approved_quote_id},{"quote_intent_id",o.quote.quote_intent_id},
        {"expires_at_ns",o.quote.expires_at_ns},{"approved_ts_ns",o.quote.approved_ts_ns},{"quote_idempotency_hash",o.quote.idempotency_hash},
        {"policy_hash",o.quote.policy_hash},{"snapshot_version_hash",o.quote.snapshot_version_hash},
        {"asset_index",leg.asset_index},{"market_index",leg.market_index},{"book_version",leg.book_version},
        {"fair_value_tick",leg.fair_value_tick},{"post_only",true},{"time_in_force","GTD_SIMULATED"},
        {"queue_ahead_lots",o.queue_ahead_lots},{"suppress_ack",o.suppress_ack},{"suppress_cancel_ack",o.suppress_cancel_ack},{"cancel_reject",o.cancel_reject}};
}
MakerOrder maker_order_from_json(const j::object& v) {
    MakerOrder o;o.order_id=s(v,"order_id");o.client_order_id=s(v,"client_order_id");o.venue_order_id=s(v,"venue_order_id");o.authorization_hash=s(v,"authorization_hash");
    auto& m=v.at("request").as_object();o.request={s(m,"strategy_id"),s(m,"strategy_instance_id"),s(m,"model_version"),s(m,"request_id"),s(m,"idempotency_key"),
        u(m,"configuration_epoch"),u(m,"market_state_version"),u(m,"source_timestamp"),u(m,"availability_ns")};
    o.original_qty=n(v,"original_qty");o.remaining_qty=n(v,"remaining_qty");o.filled_qty=n(v,"filled_qty");
    o.resource={s(v,"asset_id"),s(v,"market_id"),s(v,"event_id"),s(v,"venue"),s(v,"account"),s(v,"settlement_asset"),v.at("buy").as_bool(),o.remaining_qty,n(v,"price_tick"),n(v,"fee_per_lot_tick")};
    o.reservation_id=u(v,"reservation_id");o.created_ns=u(v,"created_ns");o.ack_due_ns=u(v,"ack_due_ns");o.cancel_due_ns=u(v,"cancel_due_ns");o.last_ack_ns=u(v,"last_ack_ns");
    for(int i=0;i<=static_cast<int>(MakerOrderStatus::RecoveryBlocked);++i)if(s(v,"status")==maker_status_name(static_cast<MakerOrderStatus>(i)))o.status=static_cast<MakerOrderStatus>(i);
    o.acknowledged=v.at("acknowledged").as_bool();o.last_evidence=s(v,"last_evidence");o.queue_ahead_lots=n(v,"queue_ahead_lots");
    o.suppress_ack=v.at("suppress_ack").as_bool();o.suppress_cancel_ack=v.at("suppress_cancel_ack").as_bool();o.cancel_reject=v.at("cancel_reject").as_bool();
    auto& q=o.quote;q.quote_group_id=u(v,"quote_group_id");q.approved_quote_id=u(v,"approved_quote_id");q.quote_intent_id=u(v,"quote_intent_id");
    q.expires_at_ns=u(v,"expires_at_ns");q.approved_ts_ns=u(v,"approved_ts_ns");q.idempotency_hash=u(v,"quote_idempotency_hash");q.policy_hash=u(v,"policy_hash");q.snapshot_version_hash=u(v,"snapshot_version_hash");
    q.has_bid=o.resource.buy;q.has_ask=!o.resource.buy;auto& l=q.has_bid?q.bid:q.ask;l.asset_id=o.resource.asset;l.market_id=o.resource.market;l.quantity_lots=o.original_qty;
    l.price_tick=o.resource.price_tick;l.side=q.has_bid?QuoteSide::Bid:QuoteSide::Ask;l.asset_index=u(v,"asset_index");l.market_index=u(v,"market_index");l.book_version=u(v,"book_version");l.fair_value_tick=n(v,"fair_value_tick");l.snapshot_version_hash=q.snapshot_version_hash;
    return o;
}
j::object maker_report_json(const MakerOrderReport& r) {
    return {{"receipt_id",r.receipt_id},{"order_id",r.order_id},{"source_event_id",r.source_event_id},{"report_type",maker_report_name(r.type)},
        {"quantity",r.quantity},{"price_tick",r.price_tick},{"fee_tick",r.fee_tick},{"ts_ns",r.ts_ns},{"authority","SIMULATED_PAPER_ADAPTER"}};
}
MakerOrderReport maker_report_from_json(const j::object& v) {
    MakerOrderReport r;r.receipt_id=s(v,"receipt_id");r.order_id=s(v,"order_id");r.source_event_id=s(v,"source_event_id");r.quantity=n(v,"quantity");r.price_tick=n(v,"price_tick");r.fee_tick=n(v,"fee_tick");r.ts_ns=u(v,"ts_ns");
    for(int i=0;i<=static_cast<int>(MakerReportType::Timeout);++i)if(s(v,"report_type")==maker_report_name(static_cast<MakerReportType>(i)))r.type=static_cast<MakerReportType>(i);
    return r;
}
std::string ExecutionGateway::maker_plan_identity(const MakerOrder& o) {
    auto row=maker_order_json(o);
    for(auto key:{"authorization_hash","venue_order_id","remaining_qty","filled_qty","open_qty","cancel_pending_qty","unknown_qty","ack_due_ns","cancel_due_ns","last_ack_ns","status","acknowledged","last_evidence","queue_ahead_lots","suppress_ack","suppress_cancel_ack","cancel_reject"})row.erase(key);
    return j::serialize(row);
}
void ExecutionGateway::configure_maker(PaperMakerExecutionAdapter* adapter,ManagedMakerConfig config,
    std::function<void(const std::string&,const MakerOrder&)> before_effect,
    std::function<void(const MakerOrder&,const MakerOrderReport&)> durable_report) {
    if(!adapter || !before_effect || !durable_report || config.submit_latency_ns==0 || config.cancel_latency_ns==0 || config.ack_timeout_ns<=config.submit_latency_ns)
        throw std::runtime_error("invalid managed maker contract");
    maker_adapter_=adapter;maker_config_=config;maker_before_effect_=std::move(before_effect);maker_durable_report_=std::move(durable_report);
}
MakerOrder ExecutionGateway::prepare_maker(const risk::ApprovedQuote& quote,const StrategyRequestMetadata& request,const risk::PortfolioLeg& resource,
    std::uint64_t reservation,std::uint64_t now) const {
    if(!maker_adapter_ || !reservation || request.strategy_instance_id.empty() || request.request_id.empty() || request.idempotency_key.empty() ||
       request.model_version.empty() || !request.configuration_epoch || !request.market_state_version || request.availability_ns!=now ||
       (quote.has_bid==quote.has_ask) || quote.expires_at_ns<=now)throw std::runtime_error("invalid maker authorization");
    const auto& leg=resource.buy?quote.bid:quote.ask;
    if(resource.buy!=quote.has_bid || leg.quantity_lots!=resource.quantity || leg.price_tick!=resource.price_tick ||
       leg.asset_id!=resource.asset || leg.market_id!=resource.market || resource.quantity<=0 || resource.quantity>1'000'000 ||
       resource.price_tick<=0 || resource.price_tick>=1'000'000 || resource.fee_per_lot_tick!=maker_config_.fee_per_lot_tick)
        throw std::runtime_error("maker plan differs from risk authorization");
    MakerOrder o;o.request=request;o.quote=quote;o.resource=resource;o.reservation_id=reservation;o.created_ns=now;
    o.order_id="M:"+std::to_string(strategy_identity_hash(request.strategy_instance_id+":"+std::to_string(request.configuration_epoch)+":"+request.idempotency_key+(resource.buy?":B":":S")));
    o.client_order_id=o.order_id;o.original_qty=o.remaining_qty=resource.quantity;o.status=MakerOrderStatus::Prepared;
    o.authorization_hash=std::to_string(strategy_identity_hash(maker_plan_identity(o)));return o;
}
void ExecutionGateway::submit_maker(const MakerOrder& prepared) {
    if(!maker_adapter_ || !maker_before_effect_ || !maker_durable_report_)throw std::runtime_error("maker adapter is not authorized");
    if(prepared.authorization_hash!=std::to_string(strategy_identity_hash(maker_plan_identity(prepared))) || prepared.status!=MakerOrderStatus::Prepared)
        throw std::runtime_error("prepared maker identity changed");
    auto found=maker_orders_.find(prepared.order_id);
    if(found!=maker_orders_.end()) {
        if(maker_plan_identity(found->second)!=maker_plan_identity(prepared))throw std::runtime_error("maker client identity collision");return;
    }
    if(maker_orders_.size()>=100000)throw std::runtime_error("maker order history budget exhausted");
    auto next=prepared;next.status=MakerOrderStatus::SubmitPending;next.ack_due_ns=next.created_ns+maker_config_.submit_latency_ns;
    maker_before_effect_("MAKER_SUBMIT_STARTED",next);
    maker_before_effect_("MAKER_ADAPTER_INVOKED",next);
    if(!maker_adapter_->accept_managed_submission(next.quote,next.created_ns))throw std::runtime_error("paper adapter rejected prepared maker");
    maker_orders_.emplace(next.order_id,next);
}
bool ExecutionGateway::cancel_maker(const std::string& id,std::uint64_t now) {
    auto it=maker_orders_.find(id);if(it==maker_orders_.end())return false;auto next=it->second;
    if(maker_terminal(next.status) || next.status==MakerOrderStatus::CancelPending)return false;
    if(next.status==MakerOrderStatus::RecoveryBlocked)return false;
    next.status=MakerOrderStatus::CancelPending;next.cancel_due_ns=now+maker_config_.cancel_latency_ns*(maker_config_.model==ManagedMakerModel::AdverseSelection?4:1);
    maker_before_effect_("MAKER_CANCEL_STARTED",next);
    if(!maker_adapter_->accept_managed_cancel())throw std::runtime_error("paper cancel request not accepted");
    it->second=next;return true;
}
bool ExecutionGateway::apply_maker_report(const MakerOrderReport& report) {
    if(!maker_durable_report_ || report.receipt_id.empty() || report.ts_ns==0)throw std::runtime_error("unidentified maker receipt");
    auto economic=maker_report_json(report);economic.erase("ts_ns");
    const auto fingerprint=j::serialize(economic);auto prior=maker_receipts_.find(report.receipt_id);
    if(prior!=maker_receipts_.end()) {
        if(prior->second!=fingerprint)throw std::runtime_error("conflicting maker receipt identity");return false;
    }
    auto& old=maker_orders_.at(report.order_id);auto next=old;
    if(report.ts_ns<old.created_ns)throw std::runtime_error("maker receipt predates submission");
    const bool terminal=maker_terminal(old.status);
    switch(report.type) {
    case MakerReportType::Ack:
        if(report.ts_ns<old.ack_due_ns)throw std::runtime_error("simulated acknowledgement before configured arrival latency");
        if(terminal || old.acknowledged)return false;
        next.acknowledged=true;next.last_ack_ns=report.ts_ns;next.venue_order_id="SIMULATED:"+old.order_id;
        if(old.status!=MakerOrderStatus::CancelPending)next.status=MakerOrderStatus::Open;break;
    case MakerReportType::Fill:
        if(report.quantity<=0 || report.quantity>old.remaining_qty || report.price_tick!=old.resource.price_tick || report.fee_tick!=report.quantity*old.resource.fee_per_lot_tick)
            throw std::runtime_error("invalid maker fill economics");
        if(report.ts_ns<old.ack_due_ns || (old.acknowledged && report.ts_ns<old.last_ack_ns))throw std::runtime_error("maker fill before simulated arrival");
        next.remaining_qty-=report.quantity;next.filled_qty+=report.quantity;next.resource.quantity=next.remaining_qty;
        if(!terminal)next.status=next.remaining_qty==0?MakerOrderStatus::Filled:old.status==MakerOrderStatus::CancelPending?MakerOrderStatus::CancelPending:MakerOrderStatus::PartiallyFilled;
        break;
    case MakerReportType::CancelAck:
        if(terminal)return false;
        if(old.status!=MakerOrderStatus::CancelPending && old.status!=MakerOrderStatus::AckUnknown)throw std::runtime_error("unsolicited maker cancel acknowledgement");
        next.status=MakerOrderStatus::Canceled;next.last_ack_ns=report.ts_ns;break;
    case MakerReportType::CancelReject:
        if(terminal)return false;
        if(old.status!=MakerOrderStatus::CancelPending)throw std::runtime_error("unsolicited cancel rejection");
        next.status=old.acknowledged?(old.filled_qty?MakerOrderStatus::PartiallyFilled:MakerOrderStatus::Open):MakerOrderStatus::AckUnknown;
        next.cancel_due_ns=0;break;
    case MakerReportType::Reject:
        if(terminal)return false;
        if(old.filled_qty || old.acknowledged)throw std::runtime_error("submission rejection after acknowledgement");
        next.status=MakerOrderStatus::Rejected;break;
    case MakerReportType::ExpireConfirmed:
        if(terminal)return false;
        next.status=MakerOrderStatus::ExpiredConfirmed;break;
    case MakerReportType::Timeout:
        if(terminal)return false;
        if(old.status!=MakerOrderStatus::CancelPending)next.status=MakerOrderStatus::AckUnknown;
        next.cancel_due_ns=0;break;
    }
    next.last_evidence=report.receipt_id;
    maker_durable_report_(next,report); // Must persist BEFORE the authoritative transition or ledger posting.
    old=next;maker_receipts_.emplace(report.receipt_id,fingerprint);return true;
}
void ExecutionGateway::advance_maker(std::uint64_t now,const std::vector<PaperMakerMarketEvent>& events,const std::string& event_id) {
    for(auto& [id,o]:maker_orders_) {
        if(maker_terminal(o.status) || o.status==MakerOrderStatus::RecoveryBlocked)continue;
        auto send=[&](MakerReportType type,const std::string& suffix){MakerOrderReport r;r.order_id=id;r.type=type;r.ts_ns=now;r.receipt_id="SIMULATED_MAKER:"+id+":"+suffix;apply_maker_report(r);};
        if(!o.acknowledged && !o.suppress_ack && now>=o.ack_due_ns)send(MakerReportType::Ack,"ack");
        if(!o.acknowledged && o.suppress_ack && now>=o.created_ns+maker_config_.ack_timeout_ns && o.status==MakerOrderStatus::SubmitPending)send(MakerReportType::Timeout,"ack_timeout");
        if(o.status==MakerOrderStatus::CancelPending && o.cancel_due_ns && now>=o.cancel_due_ns) {
            send(o.suppress_cancel_ack?MakerReportType::Timeout:o.cancel_reject?MakerReportType::CancelReject:MakerReportType::CancelAck,"cancel:"+std::to_string(o.cancel_due_ns));
        }
        if(!maker_terminal(o.status) && !o.suppress_ack && now>=o.quote.expires_at_ns)send(MakerReportType::ExpireConfirmed,"expiry");
    }
    if(events.empty() || event_id.empty() || !maker_market_events_.insert(event_id).second)return;
    if(maker_market_events_.size()>100000)throw std::runtime_error("maker market evidence budget exhausted");
    for(const auto& event:events) {
        if(event.ts_ns!=now)throw std::runtime_error("maker market evidence clock mismatch");
        std::int64_t trade_remaining=event.trade_qty_lots;
        std::int64_t ask_remaining=event.depth && event.depth->ask_count?static_cast<std::int64_t>(event.depth->asks[0].size):0;
        std::int64_t bid_remaining=event.depth && event.depth->bid_count?static_cast<std::int64_t>(event.depth->bids[0].size):0;
        for(auto& [id,o]:maker_orders_) {
            const auto& leg=o.resource.buy?o.quote.bid:o.quote.ask;
            if(leg.asset_index!=event.asset_index || !o.acknowledged || maker_terminal(o.status) || o.status==MakerOrderStatus::RecoveryBlocked || now<o.ack_due_ns ||
                (event.observed_ns && event.observed_ns<o.ack_due_ns) || now>=o.quote.expires_at_ns)continue;
            auto source=event;auto mode=PaperMakerFillMode::Conservative;
            if(maker_config_.model==ManagedMakerModel::Optimistic)mode=PaperMakerFillMode::BookCross;
            if(maker_config_.model==ManagedMakerModel::AdverseSelection)mode=PaperMakerFillMode::MidCross;
            if(maker_config_.model==ManagedMakerModel::Conservative || maker_config_.model==ManagedMakerModel::QueueAware) {
                if(!source.has_trade || (o.resource.buy?(source.trade_price_tick>=leg.price_tick || source.trade_aggressor_side!=OrderSide::Sell):
                    (source.trade_price_tick<=leg.price_tick || source.trade_aggressor_side!=OrderSide::Buy)))continue;
                source.trade_qty_lots=trade_remaining;
            }
            if(maker_config_.model==ManagedMakerModel::QueueAware) {
                if(now-o.last_ack_ns<maker_config_.queue_min_rest_ns)continue;
                auto used=std::min(o.queue_ahead_lots,std::max<std::int64_t>(0,trade_remaining));o.queue_ahead_lots-=used;trade_remaining-=used;source.trade_qty_lots=trade_remaining;
            }
            PaperMakerQuote projection;projection.quote_id=strategy_identity_hash(id);projection.approved_quote_id=o.quote.approved_quote_id;projection.quote_group_id=o.quote.quote_group_id;
            projection.asset_id=leg.asset_id;projection.asset_index=leg.asset_index;projection.has_bid=o.resource.buy;projection.has_ask=!o.resource.buy;
            projection.bid=o.quote.bid;projection.ask=o.quote.ask;projection.created_ts_ns=o.last_ack_ns;projection.expires_at_ns=o.quote.expires_at_ns;projection.status=MakerQuoteStatus::ActivePaper;
            if(o.resource.buy)projection.bid.quantity_lots=o.remaining_qty;else projection.ask.quantity_lots=o.remaining_qty;
            for(auto& fill:maker_adapter_->evaluate_managed(projection,source,mode)) {
                auto available=source.has_trade && mode==PaperMakerFillMode::Conservative?trade_remaining:o.resource.buy?ask_remaining:bid_remaining;
                auto qty=std::min(fill.filled_qty_lots,std::max<std::int64_t>(0,available));if(qty<=0)continue;
                MakerOrderReport report;report.type=MakerReportType::Fill;report.order_id=id;report.quantity=qty;report.price_tick=leg.price_tick;report.fee_tick=qty*o.resource.fee_per_lot_tick;
                report.ts_ns=now;report.source_event_id=event_id;report.receipt_id="SIMULATED_MAKER:"+id+":"+event_id+":"+std::to_string(o.filled_qty+qty);
                apply_maker_report(report);
                if(source.has_trade && mode==PaperMakerFillMode::Conservative)trade_remaining-=qty;else if(o.resource.buy)ask_remaining-=qty;else bid_remaining-=qty;
            }
        }
    }
}
void ExecutionGateway::restore_maker_blocked(MakerOrder order) {
    if(!maker_terminal(order.status))order.status=MakerOrderStatus::RecoveryBlocked;
    maker_orders_.insert_or_assign(order.order_id,std::move(order));
}
}

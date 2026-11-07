#pragma once
#include "engine/common/StrategyRequest.h"
#include "engine/risk/public/PortfolioRisk.h"
#include "engine/risk/public/ApprovedQuote.h"
#include <boost/json.hpp>
#include <map>

namespace trading_engine::execution {
enum class MakerOrderStatus {Created,Prepared,RiskReserved,SubmitPending,AckUnknown,Acknowledged,Open,PartiallyFilled,
    CancelRequested,CancelPending,ReplacePending,Filled,Canceled,Rejected,ExpiredConfirmed,RecoveryBlocked};
enum class MakerReportType {Ack,Fill,CancelAck,CancelReject,Reject,ExpireConfirmed,Timeout};
enum class MakerReplacementPolicy {CancelThenNew,OverlapWithExtraReservation,BlockReplacement};
enum class ManagedMakerModel {Optimistic,Conservative,QueueAware,AdverseSelection};
struct ManagedMakerConfig {
    ManagedMakerModel model=ManagedMakerModel::Conservative;
    std::uint64_t submit_latency_ns=50'000'000,cancel_latency_ns=50'000'000,ack_timeout_ns=1'000'000'000,queue_min_rest_ns=250'000'000;
    std::int64_t fee_per_lot_tick=0;
};
struct MakerOrder {
    std::string order_id,client_order_id,venue_order_id,authorization_hash,last_evidence;
    StrategyRequestMetadata request;
    risk::ApprovedQuote quote;
    risk::PortfolioLeg resource;
    std::uint64_t reservation_id=0,created_ns=0,ack_due_ns=0,cancel_due_ns=0,last_ack_ns=0;
    std::int64_t original_qty=0,remaining_qty=0,filled_qty=0,queue_ahead_lots=0;
    MakerOrderStatus status=MakerOrderStatus::Created;
    bool acknowledged=false,cancel_reject=false,suppress_ack=false,suppress_cancel_ack=false;
};
struct MakerOrderReport {
    std::string receipt_id,order_id,source_event_id;
    MakerReportType type=MakerReportType::Ack;
    std::int64_t quantity=0,price_tick=0,fee_tick=0;
    std::uint64_t ts_ns=0;
};
bool maker_terminal(MakerOrderStatus);
const char* maker_status_name(MakerOrderStatus);
const char* maker_report_name(MakerReportType);
boost::json::object maker_order_json(const MakerOrder&);
MakerOrder maker_order_from_json(const boost::json::object&);
boost::json::object maker_report_json(const MakerOrderReport&);
MakerOrderReport maker_report_from_json(const boost::json::object&);
}

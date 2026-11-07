#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
#include "engine/decode/parser/polymarket/CrossVenueClobNormalizer.h"
#include "engine/common/math/FractionalVwap.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include <algorithm>
#include <memory>

namespace trading_engine::cross_venue::research {
struct ResearchState::Impl {
    Config config;std::vector<Market> markets;std::vector<ApprovedPair> approved;
    SynchronizedBookStore store;
    std::map<std::string,std::unique_ptr<KalshiBookNormalizer>> kalshi;
    std::map<std::string,std::unique_ptr<CrossVenueClobNormalizer>> poly;
    std::map<std::string,std::uint64_t> local_seq;
    std::map<std::uint64_t,KalshiStreamSequence> subscriptions;
    std::map<std::string,std::string> source;
    std::map<std::string,std::uint64_t> counts,failures;
    Array diagnostics;
    std::set<std::string> changed;
    std::uint64_t gross_positive=0,fee_qualified=0;
    Impl(Config c,std::vector<Market> m,std::vector<ApprovedPair> p):config(std::move(c)),markets(std::move(m)),approved(std::move(p)),
        store(get_text(config.original,"reconciliation_policy")=="quiet_request_v0.1.1",config.policy.asof_enabled) {
        config.policy.max_liveness_age_ns=config.heartbeat_ms*1'000'000;
        config.policy.max_authoritative_age_ns=config.authoritative_ms*1'000'000;
    }
    const Market* owner(const VenueInstrumentKey& k) const {
        for(const auto& m:markets)if(m.yes==k || m.no==k)return &m;return nullptr;
    }
    void invalidate_venue(const std::string& venue,const std::string& reason,std::uint64_t now) {
        for(const auto& m:markets)if(m.venue==venue)for(const auto& k:{m.yes,m.no}) {
            store.invalidate(k,reason,now);source.erase(k.identity());changed.insert(k.identity());kalshi.erase(k.identity());poly.erase(k.identity());local_seq.erase(k.identity());
        }
        ++failures[reason];
    }
};
ResearchState::ResearchState(Config c,std::vector<Market> m,std::vector<ApprovedPair> p):impl_(std::make_unique<Impl>(std::move(c),std::move(m),std::move(p))){}
ResearchState::~ResearchState()=default;
SynchronizedBookStore& ResearchState::books(){return impl_->store;}
const std::vector<ApprovedPair>& ResearchState::pairs()const{return impl_->approved;}
bool ResearchState::websocket_book(const VenueInstrumentKey& k)const {auto it=impl_->source.find(k.identity());return it!=impl_->source.end() && it->second=="ws";}
bool ResearchState::continuous_pair(const ApprovedPair& p)const {
    const auto a=impl_->source.find(p.leg_yes.identity()),b=impl_->source.find(p.leg_no.identity());
    const auto *yes=impl_->owner(p.leg_yes),*no=impl_->owner(p.leg_no);
    return yes && no && yes->tradable && no->tradable && a!=impl_->source.end() && b!=impl_->source.end() && a->second=="ws" && b->second=="ws";
}
const Market& ResearchState::market(const VenueInstrumentKey& k)const {
    const auto* m=impl_->owner(k);require(m!=nullptr,"unmapped instrument");return *m;
}

bool ResearchState::process(const Object& r) {
    auto& x=*impl_;const auto venue=get_text(r,"venue"),transport=get_text(r,"transport"),classification=get_text(r,"classification");
    const auto now=get_uint(r,"recv_monotonic_ns"),epoch=get_uint(r,"connection_epoch");
    const auto raw=get_text(r,"raw_payload");require(sha256(raw)==get_text(r,"raw_hash"),"raw input checksum mismatch");
    ++x.counts[venue+":"+transport+":"+classification];
    if(transport=="event") {
        if(classification=="connection_open" || classification=="disconnect" || classification=="gap" || classification=="timeout" || classification=="validation_reconnect")
            x.invalidate_venue(venue,classification,now);
        return true;
    }
    if(classification=="heartbeat") {
        for(const auto& m:x.markets)if(m.venue==venue)for(const auto& k:{m.yes,m.no})x.store.note_liveness(k,now);
        return true;
    }
    std::vector<const Market*> selected;
    if(const auto* key=r.if_contains("key");key && key->is_object()) {
        const auto k=parse_key(key->as_object());if(const auto* m=x.owner(k))selected.push_back(m);
    }
    if(transport=="rest" && get_uint(r,"http_status")!=200) {
        for(const auto* m:selected)for(const auto& k:{m->yes,m->no})x.store.invalidate(k,"HTTP_"+std::to_string(get_uint(r,"http_status")),now);
        ++x.failures["HTTP_"+std::to_string(get_uint(r,"http_status"))];return false;
    }
    json::value parsed;try{parsed=json::parse(raw);}catch(...){x.invalidate_venue(venue,"MALFORMED_PACKET",now);return false;}
    // Connection liveness is venue/session scoped. A valid frame on this
    // socket proves it is alive even when a watched market is quiet. Authority
    // and price-update timestamps remain separate and are never refreshed here.
    if(transport=="ws")for(const auto& market:x.markets)if(market.venue==venue)
        for(const auto& key:{market.yes,market.no})x.store.note_liveness(key,now);
    Array packets;if(parsed.is_array())packets=parsed.as_array();else packets.push_back(parsed);
    bool ok=true;
    for(const auto& packet:packets) {
        if(!packet.is_object()){ok=false;continue;}auto object=packet.as_object();
        std::string type=get_text(object,"event_type",get_text(object,"type",classification));
        Object body=object;if(const auto* v=object.if_contains("payload");v && v->is_object())body=v->as_object();
        if(venue=="kalshi" && transport=="ws") {
            if(type=="error"){
                if(x.diagnostics.size()<10000)x.diagnostics.push_back(Object{{"reason","KALSHI_SUBSCRIPTION_ERROR"},{"venue",venue},
                    {"record_sequence",get_uint(r,"record_sequence")},{"recv_monotonic_ns",now},{"raw_hash",get_text(r,"raw_hash")}});
                x.invalidate_venue(venue,"KALSHI_SUBSCRIPTION_ERROR",now);return false;
            }
            if(type!="orderbook_snapshot" && type!="orderbook_delta" && type!="market_lifecycle_v2")continue;
            if(!x.subscriptions[epoch].accept(object)){x.invalidate_venue(venue,"SUBSCRIPTION_SEQUENCE_GAP",now);return false;}
            const auto* msg=object.if_contains("msg");if(!msg || !msg->is_object()){ok=false;continue;}
            const auto ticker=get_text(msg->as_object(),"market_ticker");selected.clear();
            for(const auto& m:x.markets)if(m.venue==venue && m.id==ticker)selected.push_back(&m);
        } else if(venue=="polymarket_international" && transport=="ws") {
            const auto condition=get_text(body,"market",get_text(body,"conditionId",get_text(body,"condition_id")));
            selected.clear();for(const auto& m:x.markets)if(m.venue==venue && m.id==condition)selected.push_back(&m);
        }
        if(type=="tick_size_change" || type=="market_resolved" || type=="market_lifecycle_v2" || type=="market_closed") {
            // Kalshi lifecycle subscriptions are venue-wide. A status event
            // for an unrelated instrument must not stop this book session.
            if(selected.empty()){++x.counts["UNWATCHED_LIFECYCLE_OR_TICK_EVENT"];continue;}
            for(const auto* market:selected)for(auto& editable:x.markets)if(editable.id==market->id && editable.venue==market->venue){
                if(type=="tick_size_change")try{editable.tick=money(get_text(body,"new_tick_size",get_text(body,"newTickSize")));require(editable.tick>0,"bad tick");}
                    catch(...){editable.tradable=false;}
                else editable.tradable=false;
            }
            for(const auto* m:selected)for(const auto& k:{m->yes,m->no})x.store.invalidate(k,"MARKET_STATUS_OR_TICK_CHANGED_REBUILD_REQUIRED",now);
            ++x.failures["MARKET_STATUS_OR_TICK_CHANGED_REBUILD_REQUIRED"];ok=false;continue;
        }
        if(transport=="ws" && type!="book" && type!="price_change" && type!="orderbook_snapshot" && type!="orderbook_delta") {
            // Non-book market events provide liveness, never refresh prices.
            for(const auto* m:selected)for(const auto& k:{m->yes,m->no})x.store.note_liveness(k,now);
            continue;
        }
        for(const auto* m:selected) {
            if(!m->protocol_supported){++x.failures["UNSUPPORTED_PROTOCOL"];continue;}
            for(const auto& k:{m->yes,m->no}) {
                if(k.instrument_id.empty())continue;
                if(venue=="polymarket_international") {
                    if(transport=="rest" && parse_key(r.at("key").as_object())!=k)continue;
                    const auto asset=get_text(body,"asset_id",get_text(body,"assetId"));
                    if(type=="book" && !asset.empty() && asset!=k.instrument_id)continue;
                    if(type=="price_change") {
                        const auto* changes=body.if_contains("price_changes");if(!changes)changes=body.if_contains("priceChanges");
                        bool relevant=false;if(changes && changes->is_array())for(const auto& c:changes->as_array())
                            relevant|=get_text(c.as_object(),"asset_id",get_text(c.as_object(),"assetId"))==k.instrument_id;
                        if(!relevant)continue;
                    }
                }
                BookProvenance p;p.recv_monotonic_ns=now;p.recv_wall_ns=get_uint(r,"recv_wall_ns");p.source_ts_ns=get_uint(r,"exchange_timestamp_ns");
                if(venue=="polymarket_international")try {const auto ts=get_text(body,"timestamp");
                    if(!ts.empty() && ts.find_first_not_of("0123456789")==std::string::npos)p.source_ts_ns=std::stoull(ts)*1'000'000;
                }catch(...){}
                p.last_liveness_ns=now;p.connection_epoch=epoch;p.raw_log_offset=get_uint(r,"raw_log_offset");p.raw_hash=get_text(r,"raw_hash");
                p.authoritative_snapshot=transport=="rest" || type=="book" || type=="orderbook_snapshot";
                BookUpdate update;
                const auto wire=json::serialize(packet);
                if(venue=="kalshi") {
                    if(transport=="rest") {KalshiBookNormalizer decoder(k,k.instrument_id=="YES",true);update=decoder.rest_snapshot(wire,p);}
                    else {auto& decoder=x.kalshi[k.identity()];if(!decoder)decoder=std::make_unique<KalshiBookNormalizer>(k,k.instrument_id=="YES",true);update=decoder->websocket(wire,p,true);}
                } else {
                    if(transport=="rest") {CrossVenueClobNormalizer decoder(k,true);update=decoder.normalize(wire,p,1);}
                    else {auto& decoder=x.poly[k.identity()];if(!decoder)decoder=std::make_unique<CrossVenueClobNormalizer>(k,true);update=decoder->normalize(wire,p,++x.local_seq[k.identity()]);}
                }
                // The evidence always points at the original recorded frame,
                // including when a frame contains a JSON array of submessages.
                update.evidence.raw_hash=p.raw_hash;
                bool accepted=false;
                if(transport=="rest" && x.source[k.identity()]=="ws") {
                    SynchronizedBookStore reference(false,x.config.policy.asof_enabled);reference.apply(update);
                    const auto live=x.store.read(k),rest=reference.read(k);
                    // Comparing two asynchronous snapshots without a common
                    // time is not proof of packet loss. In V0.1.1, a mismatch
                    // during an in-flight update or against older source time
                    // is inconclusive and does NOT refresh authority.
                    auto comparison=x.store;
                    const bool equal=comparison.reconcile(k,rest,now);
                    const auto start=get_uint(r,"request_started_monotonic_ns");
                    const bool overlap=start && live.evidence.recv_monotonic_ns>start;
                    const bool older=rest.evidence.source_ts_ns && live.evidence.source_ts_ns && rest.evidence.source_ts_ns<live.evidence.source_ts_ns;
                    const bool safe_policy=get_text(x.config.original,"reconciliation_policy")=="quiet_request_v0.1.1";
                    if(!equal){
                        Object diagnostic{{"reason",safe_policy&&(overlap||older)?"RECONCILIATION_INCONCLUSIVE":"AUTHORITATIVE_SNAPSHOT_MISMATCH"},
                            {"venue",venue},{"market_id",m->id},{"instrument_id",k.instrument_id},{"record_sequence",get_uint(r,"record_sequence")},
                            {"recv_monotonic_ns",now},{"connection_epoch",epoch},{"raw_hash",p.raw_hash},
                            {"live_raw_hash",live.evidence.raw_hash},{"live_update_ns",live.evidence.recv_monotonic_ns},
                            {"request_started_monotonic_ns",start},{"request_overlap",overlap},{"older_source_timestamp",older},
                            {"live_source_ts_ns",live.evidence.source_ts_ns},{"rest_source_ts_ns",rest.evidence.source_ts_ns},
                            {"live_usable",live.depth.usable_for_depth},{"reference_usable",rest.depth.usable_for_depth}};
                        for(bool bid:{true,false}) {
                            const auto n=bid?live.depth.bid_count:live.depth.ask_count;
                            const auto count=bid?rest.depth.bid_count:rest.depth.ask_count;
                            for(std::uint16_t i=0;i<std::max(n,count);++i){
                                const auto lp=i<n?(bid?live.depth.bids[i].price_tick:live.depth.asks[i].price_tick):-1;
                                const auto rp=i<count?(bid?rest.depth.bids[i].price_tick:rest.depth.asks[i].price_tick):-1;
                                const auto lq=i<n?(bid?live.bid_qty_micro[i]:live.ask_qty_micro[i]):-1;
                                const auto rq=i<count?(bid?rest.bid_qty_micro[i]:rest.ask_qty_micro[i]):-1;
                                if(lp!=rp || lq!=rq){diagnostic[bid?"first_bid_difference":"first_ask_difference"]=Object{{"level",i},{"live_price",lp},{"reference_price",rp},{"live_size_micro",lq},{"reference_size_micro",rq}};break;}
                            }
                        }
                        // Drain at every live emission / audit record; bound
                        // diagnostics for older replay clients that don't drain.
                        if(x.diagnostics.size()<10000)x.diagnostics.push_back(std::move(diagnostic));
                    }
                    if(!equal && safe_policy && (overlap||older)) {++x.counts["RECONCILIATION_INCONCLUSIVE"];continue;}
                    accepted=x.store.reconcile(k,rest,now);
                } else {
                    accepted=x.store.apply(update);if(accepted)x.source[k.identity()]=transport;
                }
                if(!accepted){
                    if(x.diagnostics.size()<10000)x.diagnostics.push_back(Object{{"reason","BOOK_REJECTED_OR_RECONCILIATION_FAILED"},
                        {"venue",venue},{"market_id",m->id},{"instrument_id",k.instrument_id},{"transport",transport},
                        {"record_sequence",get_uint(r,"record_sequence")},{"recv_monotonic_ns",now},{"raw_hash",p.raw_hash},
                        {"update_kind",static_cast<int>(update.kind)},{"reference_complete",update.complete},
                        {"apply_rejection",x.store.read(k).evidence.health_reason}});
                    x.store.invalidate(k,"BOOK_REJECTED_OR_RECONCILIATION_FAILED",now);++x.failures["BOOK_REJECTED_OR_RECONCILIATION_FAILED"];ok=false;}
                else {++x.counts[venue+":valid_book_observation"];x.changed.insert(k.identity());}
            }
        }
    }
    return ok;
}

std::vector<AnalyzedOpportunity> ResearchState::scan(std::uint64_t now,std::uint64_t wall) {
    auto& x=*impl_;std::vector<AnalyzedOpportunity> out;
    for(const auto& pair:x.approved) {
        if(!x.changed.contains(pair.leg_yes.identity()) && !x.changed.contains(pair.leg_no.identity()))continue;
        const auto books=x.store.read_pair(pair);const auto* yes=x.owner(pair.leg_yes);const auto* no=x.owner(pair.leg_no);
        if(!yes || !no)continue;
        if(!yes->tradable || !no->tradable){++x.failures["MARKET_NOT_TRADABLE"];continue;}
        if(x.config.policy.asof_enabled && (!websocket_book(pair.leg_yes) || !websocket_book(pair.leg_no))){++x.failures["REST_NOT_CONTINUOUS_L2"];continue;}
        try{validate_books(pair,books,x.config.policy,now);}catch(const std::exception& e){++x.failures[e.what()];continue;}
        for(const auto q:x.config.policy.quantities) try {
            const auto y=common::math::buy_vwap_exact(books[0],q),n=common::math::buy_vwap_exact(books[1],q);
            require(y.ok && n.ok,"INCOMPLETE_DEPTH");
            if(multiply(q,unit)>add(y.total_cost_tick,n.total_cost_tick))++x.gross_positive;
            for(const auto* market:{yes,no}) {
                require(market->min_qty_micro>0 && market->qty_step_micro>0 && market->tick>0,"UNKNOWN_QUANTITY_OR_TICK_CONSTRAINT");
                require(multiply(q,unit)>=market->min_qty_micro && multiply(q,unit)%market->qty_step_micro==0,"INVALID_CANDIDATE_GRANULARITY");
            }
            for(std::size_t i=0;i<2;++i){const auto* market=i==0?yes:no;const auto worst=i==0?y.worst_price_tick:n.worst_price_tick;
                require(worst%market->tick==0,"PRICE_TICK_CONSTRAINT_MISMATCH");}
            const auto yf=fee_schedule(*yes),nf=fee_schedule(*no);
            FeeQuote fee;fee.qty=q;fee.fees={estimate_fee(yf,books[0],q,wall),estimate_fee(nf,books[1],q,wall)};
            fee.version=sha256(field(yf.version)+field(nf.version)+field(std::to_string(yes->tick))+field(std::to_string(no->tick))+
                field(std::to_string(yes->qty_step_micro))+field(std::to_string(no->qty_step_micro)));fee.pair_artifact_hash=pair.artifact_hash;
            fee.valid_from_ns=0;fee.valid_until_ns=now+x.config.policy.max_age_ns+1;
            // Explicit future-execution buffers; observed depth walking is
            // already in y/n cost and is never charged a second time.
            const auto future=money(get_text(x.config.original,"slippage_per_share","0.001"));
            const auto execution=money(get_text(x.config.original,"execution_risk_per_share","0.001"));
            fee.slippage={multiply(add(future,execution),q),multiply(add(future,execution),q)};
            auto opportunity=PairScanner::quote(pair,books,fee,x.config.policy,now);
            std::array<Money,2> local_cash{x.config.allocations[pair.leg_yes.venue==VenueId::Kalshi?0:1],x.config.allocations[pair.leg_no.venue==VenueId::Kalshi?0:1]};
            MultiVenueReservationBook risk_probe(pair,local_cash);
            (void)risk_probe.reserve(opportunity,pair,x.store,fee,x.config.policy,now);
            ++x.fee_qualified;
            const bool continuous=x.source[pair.leg_yes.identity()]=="ws" && x.source[pair.leg_no.identity()]=="ws";
            std::string liquidity=field(pair.pair_id);
            for(const auto& b:books)for(std::uint16_t i=0;i<b.depth.ask_count;++i)
                liquidity+=field(std::to_string(b.depth.asks[i].price_tick))+field(std::to_string(b.ask_qty_micro[i]));
            AnalyzedOpportunity a{opportunity,pair,fee,sha256(liquidity),now,wall,continuous,{}};
            a.row={{"candidate_id",opportunity.candidate_id},{"pair_id",pair.pair_id},{"lifecycle","SHADOW_ELIGIBLE"},
                {"evidence_level",continuous?"REAL_L2_QUOTED_ONLY":"REST_SNAPSHOT_DIAGNOSTIC"},{"observed_mono_ns",now},{"qty",q},
                {"gross_edge",decimal(add(multiply(q,unit),-add(y.total_cost_tick,n.total_cost_tick)))},
                {"fee_budget",decimal(add(fee.fees[0],fee.fees[1]))},{"future_slippage_budget",decimal(multiply(future,2*q))},
                {"execution_risk_budget",decimal(multiply(execution,2*q))},{"net_edge",decimal(opportunity.intent.total_edge_tick)},
                {"committed_capital",decimal(add(add(y.total_cost_tick,n.total_cost_tick),add(add(fee.fees[0],fee.fees[1]),add(fee.slippage[0],fee.slippage[1]))))},
                {"return_bps",opportunity.intent.edge_bps},{"liquidity_id",a.liquidity_id},{"artifact_hash",pair.artifact_hash},
                {"fee_version",fee.version},{"fee_source_yes",yf.source},{"fee_source_no",nf.source},{"fee_timestamp_yes",yf.observed_wall_ns},{"fee_timestamp_no",nf.observed_wall_ns},
                {"yes_raw_hash",books[0].evidence.raw_hash},{"no_raw_hash",books[1].evidence.raw_hash},
                {"yes_raw_offset",books[0].evidence.raw_log_offset},{"no_raw_offset",books[1].evidence.raw_log_offset}};
            a.row["lifecycle_trace"]=Array{"DETECTED","VALIDATED","SHADOW_ELIGIBLE"};
            out.push_back(std::move(a));
        } catch(const std::exception& e){++x.failures[e.what()];}
    }
    x.changed.clear();
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.opportunity.intent.total_edge_tick!=b.opportunity.intent.total_edge_tick)return a.opportunity.intent.total_edge_tick>b.opportunity.intent.total_edge_tick;
        if(a.opportunity.intent.edge_bps!=b.opportunity.intent.edge_bps)return a.opportunity.intent.edge_bps>b.opportunity.intent.edge_bps;
        return a.opportunity.candidate_id<b.opportunity.candidate_id;
    });
    return out;
}
Array ResearchState::health()const {
    Array out;const auto& x=*impl_;
    for(const auto& m:x.markets)for(const auto& k:{m.yes,m.no}) {
        const auto b=x.store.read(k);const auto& e=b.evidence;
        out.push_back(Object{{"venue",m.venue},{"market_id",m.id},{"instrument_id",k.instrument_id},{"usable",b.depth.usable_for_depth},
            {"transport",x.source.contains(k.identity())?x.source.at(k.identity()):"NONE"},
            {"snapshot_version",e.snapshot_version},{"last_market_update_ns",e.recv_monotonic_ns},{"last_authoritative_snapshot_ns",e.last_authoritative_snapshot_ns},
            {"last_liveness_ns",e.last_liveness_ns},{"sequence",e.sequence},{"sid",e.stream_sid},{"exchange_clock",e.exchange_clock},
            {"health_reason",e.health_reason},{"protocol",m.protocol},{"tradable",m.tradable}});
        auto& row=out.back().as_object();row["bounded_prefix"]=b.bounded_prefix;row["bid_levels"]=b.depth.bid_count;row["ask_levels"]=b.depth.ask_count;
    } return out;
}
Object ResearchState::counters()const {
    Object counts,failures;for(const auto& [k,v]:impl_->counts)counts[k]=v;for(const auto& [k,v]:impl_->failures)failures[k]=v;
    return {{"counts",counts},{"rejections",failures},{"positive_gross_observations",impl_->gross_positive},{"positive_net_observations",impl_->fee_qualified}};
}
Array ResearchState::take_diagnostics(){Array out;out.swap(impl_->diagnostics);return out;}
}  // namespace trading_engine::cross_venue::research

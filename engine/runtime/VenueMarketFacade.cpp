#include "engine/runtime/VenueMarketFacade.h"
#include <set>
#include <regex>
namespace trading_engine::runtime {
namespace cv=cross_venue;namespace r=cv::research;namespace j=boost::json;
VenueMarketFacade::VenueMarketFacade(const j::array& markets,std::uint64_t max_age) {
    for(auto& value:markets) {
        auto& row=value.as_object(); auto m=r::normalize_market(r::get_text(row,"venue"),row.at("raw").as_object());
        if(m.venue=="kalshi" && !std::regex_match(m.id,std::regex("[A-Z0-9_.-]+"))) throw std::runtime_error("invalid market identity");
        if(m.venue=="polymarket_international" && (!std::regex_match(m.id,std::regex("0x[0-9a-fA-F]{64}")) ||
           !std::regex_match(m.yes.instrument_id,std::regex("[0-9]+")) || !std::regex_match(m.no.instrument_id,std::regex("[0-9]+"))))
            throw std::runtime_error("invalid CLOB identity");
        if(!m.protocol_supported || !m.tradable) throw std::runtime_error("configured market is unsupported or not tradable");
        for(auto& key:{m.yes,m.no}) {
            // Legacy strategies use outcome asset strings; reject ambiguity rather than alias venue state.
            const auto alias=key.venue==cv::VenueId::Kalshi?key.identity():key.instrument_id;
            if(assets_.contains(alias)) throw std::runtime_error("ambiguous native asset identifier");
            assets_[alias]=key;
        }
        markets_.push_back(std::move(m));
    }
    if(markets_.empty()) throw std::runtime_error("no live markets configured");
    r::Config config;config.policy.asof_enabled=true;
    config.original={{"reconciliation_policy","quiet_request_v0.1.1"}};
    config.heartbeat_ms=30000;config.authoritative_ms=max_age/1'000'000;
    policy_.liveness_ns=30'000'000'000ULL;policy_.authority_ns=max_age;policy_.change_ns=max_age;
    state_=std::make_unique<r::ResearchState>(config,markets_,std::vector<cv::ApprovedPair>{});
}
bool VenueMarketFacade::process(const j::object& record) {
    duplicate_=false;
    if(r::get_text(record,"transport")=="ws" && r::get_text(record,"classification")=="market_data") {
        auto id=r::get_text(record,"venue")+":"+std::to_string(r::get_uint(record,"connection_epoch"))+":"+r::get_text(record,"raw_hash");
        if(!frames_.insert(id).second) {
            duplicate_=true;++duplicate_count_;auto live=record;live["classification"]="heartbeat";
            return state_->process(live);
        }
        frame_order_.push_back(id);
        if(frame_order_.size()>4096){frames_.erase(frame_order_.front());frame_order_.pop_front();}
    }
    if(r::get_text(record,"transport")=="event" &&
       (r::get_text(record,"classification")=="connection_error" || r::get_text(record,"classification")=="authentication_error")) {
        auto failed=record;failed["classification"]="disconnect";return state_->process(failed);
    }
    return state_->process(record);
}
bool VenueMarketFacade::admitted(const cv::VenueInstrumentKey& key,std::uint64_t now) const {
    const auto& market=state_->market(key);
    auto books=std::array{state_->books().read(market.yes),state_->books().read(market.no)};
    auto joint=cv::joint_book_asof(books,now,policy_,{state_->websocket_book(market.yes),state_->websocket_book(market.no)},
                                  {market.tradable,market.tradable},false);
    return joint.diagnostic_eligible;
}
state::StateQueryResult<state::MarketStateSnapshot> VenueMarketFacade::snapshot(const std::string& asset,std::uint64_t now) const {
    state::StateQueryResult<state::MarketStateSnapshot> result;
    auto it=assets_.find(asset);if(it==assets_.end()) return result;
    const auto& k=it->second;const auto b=state_->books().read(k);
    auto& s=result.value;s.entity_id=asset;s.market_id=k.market_id;s.version=b.evidence.snapshot_version;
    s.last_book_update_ns=std::max(b.evidence.recv_monotonic_ns,b.evidence.last_authoritative_snapshot_ns);s.live=admitted(k,now);s.recovering=!s.live;
    s.closed=!state_->market(k).tradable;s.resolved=false;s.crossed=b.depth.crossed;
    s.bid_count=b.depth.bid_count;s.ask_count=b.depth.ask_count;
    for(std::uint32_t n=0;n<s.bid_count;++n) s.bids[n]=b.depth.bids[n];
    for(std::uint32_t n=0;n<s.ask_count;++n) s.asks[n]=b.depth.asks[n];
    s.has_bid=s.bid_count>0;s.has_ask=s.ask_count>0;
    if(s.has_bid)s.best_bid_tick=s.bids[0].price_tick;if(s.has_ask)s.best_ask_tick=s.asks[0].price_tick;
    s.usable_for_depth=s.live && b.depth.usable_for_depth;s.usable_for_signal=s.usable_for_depth;
    s.state_hash=b.evidence.content_hash;s.snapshot_version_hash=b.depth.snapshot_version_hash;
    s.quality=s.live?state::BookQuality::Good:state::BookQuality::Unknown;
    result.ok=b.evidence.snapshot_version!=0;return result;
}
signal::SnapshotReadResult VenueMarketFacade::read_for_bundle(const signal::CandidateBundle& bundle,const signal::SignalConfig& config,std::uint64_t now) const {
    std::vector<state::MarketStateSnapshot> snapshots;std::set<std::string> seen;
    for(std::uint16_t n=0;n<bundle.leg_count;++n) if(seen.insert(bundle.legs[n].asset_id).second) {
        auto s=snapshot(bundle.legs[n].asset_id,now);if(s.ok)snapshots.push_back(s.value);
    }
    return signal::validate_bundle_snapshots(bundle,config,snapshots,now);
}
signal::DepthReadResult VenueMarketFacade::read_depth_for_plan(const signal::BundleRuntimePlan& plan,const signal::SignalConfig& config,std::uint64_t now) const {
    std::array<state::MarketDepthView,signal::kMaxIntentLegs> depth{};std::uint16_t count=0;
    for(std::uint16_t n=0;n<plan.unique_asset_count;++n) {
        auto s=snapshot(*plan.unique_asset_ids[n],now);
        if(s.ok)depth[count++]=state::market_depth_view_from_snapshot(s.value,plan.unique_asset_indices[n]);
    }
    return signal::validate_plan_depth_views(plan,config,depth,count,now);
}
j::array VenueMarketFacade::health(std::uint64_t now) const {
    auto result=state_->health();
    for(auto& v:result) {
        auto& row=v.as_object();bool ready=false;
        for(const auto& market:markets_)for(const auto& key:{market.yes,market.no})
            if(cv::venue_name(key.venue)==r::get_text(row,"venue") && key.market_id==r::get_text(row,"market_id") && key.instrument_id==r::get_text(row,"instrument_id"))
                ready=admitted(key,now);
        row["admitted"]=ready;
        const auto update=r::get_uint(row,"last_market_update_ns");
        row["book_age_ns"]=now>=update?now-update:0;row["authority_owner"]="sequence_aware_store";
    }
    return result;
}
j::object VenueMarketFacade::counters() const {auto result=state_->counters();result["duplicate_market_frames"]=duplicate_count_;return result;}
}

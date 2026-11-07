#pragma once
#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/signal/reader/SnapshotBatchReader.h"
#include "engine/state/cross_venue/JointBookAsOf.h"
#include <map>
#include <set>
#include <deque>
#include "state/MarketStateQueryResult.h"
namespace trading_engine::runtime {
// Existing sequence-aware ResearchState owns the ONLY live book mutator.
// Native snapshots/depth below are value projections, never another state store.
class VenueMarketFacade final : public signal::IMarketSnapshotReader, public signal::IDepthBatchReader {
public:
    VenueMarketFacade(const boost::json::array& markets,std::uint64_t max_age_ns);
    bool process(const boost::json::object& record);
    cross_venue::VersionedBook read(const cross_venue::VenueInstrumentKey& key) const { return state_->books().read(key); }
    bool last_duplicate() const {return duplicate_;}
    boost::json::array health(std::uint64_t now_ns) const;
    boost::json::object counters() const;
    state::StateQueryResult<state::MarketStateSnapshot> snapshot(const std::string& asset,std::uint64_t now_ns) const;
    signal::SnapshotReadResult read_for_bundle(const signal::CandidateBundle&,const signal::SignalConfig&,std::uint64_t) const override;
    signal::DepthReadResult read_depth_for_plan(const signal::BundleRuntimePlan&,const signal::SignalConfig&,std::uint64_t) const override;
private:
    std::vector<cross_venue::research::Market> markets_;
    std::unique_ptr<cross_venue::research::ResearchState> state_;
    std::map<std::string,cross_venue::VenueInstrumentKey> assets_;
    cross_venue::JointBookPolicy policy_;
    bool duplicate_=false;std::uint64_t duplicate_count_=0;
    std::set<std::string> frames_;std::deque<std::string> frame_order_;
    bool admitted(const cross_venue::VenueInstrumentKey&,std::uint64_t) const;
};
}

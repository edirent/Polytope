#pragma once
#include "engine/signal/core/SignalEngine.h"
#include "engine/common/StrategyRequest.h"
#include <functional>
#include <string_view>
namespace trading_engine::runtime {
// Evidence references are borrowed only for the synchronous callback lifetime.
// Plugins cannot access execution adapters or mutate account/risk ownership.
using IntentSink = std::function<void(const signal::OpportunityIntent&, const signal::SignalEvidenceView&)>;
using RequestSink = std::function<void(const StrategyRequest&)>;
struct MakerFrameAsset {
    std::string market_id,asset_id;
    std::uint32_t market_index=0,asset_index=0;
    state::MarketDepthView depth;
    std::int64_t position_lots=0;
};
class StrategyPlugin {
public:
    virtual ~StrategyPlugin() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual void on_market_update(std::uint64_t now_ns, const IntentSink& sink) = 0;
    virtual void on_immutable_frame(const std::vector<MakerFrameAsset>&,std::uint64_t now,
        const RequestSink&,const IntentSink& sink) {on_market_update(now,sink);}
    virtual void on_order_event(std::uint32_t,bool) {}
};
class CombinatorialPlugin final : public StrategyPlugin {
public:
    explicit CombinatorialPlugin(signal::SignalEngine& engine) : engine_(engine) {}
    std::string_view id() const noexcept override { return "combinatorial"; }
    void on_market_update(std::uint64_t now_ns, const IntentSink& sink) override {
        signal::SignalScanContext ctx;
        ctx.scan_id = scan_++; ctx.now_monotonic_ns = now_ns;
        ctx.settlement_masks_available = false;
        (void)engine_.scan_once(ctx);
        auto intents = engine_.last_published_intents();
        for(std::size_t i=0;i<intents.size();++i) sink(intents[i],engine_.last_published_evidence_at(i));
    }
private:
    signal::SignalEngine& engine_;
    std::uint64_t scan_ = 1;
};
}

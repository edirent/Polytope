#pragma once
#include "engine/runtime/StrategyPlugin.h"
#include "engine/common/StrategyRequest.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include <vector>

namespace trading_engine::runtime {
class MakerPlugin final : public StrategyPlugin {
public:
    MakerPlugin(strategy::market_making::MarketMakingConfig config,StrategyRequestMetadata identity)
      :engine_(config),identity_(std::move(identity)){}
    std::string_view id() const noexcept override {return "market_making";}
    void on_market_update(std::uint64_t,const IntentSink&) override {throw std::runtime_error("maker requires immutable frame");}
    void on_immutable_frame(const std::vector<MakerFrameAsset>& assets,std::uint64_t now,const RequestSink& requests,const IntentSink&) override {on_frame(assets,now,requests);}
    void on_frame(const std::vector<MakerFrameAsset>& assets,std::uint64_t now,const RequestSink& sink) {
        for(const auto& value:assets) {
            strategy::market_making::MarketMakingInput input;
            input.market_id=value.market_id;input.asset_id=value.asset_id;input.market_index=value.market_index;input.asset_index=value.asset_index;
            input.depth=&value.depth;input.current_position_lots=value.position_lots;input.now_ns=now;
            auto result=engine_.on_market_update(input);last_results_.push_back(result);
            for(std::uint16_t i=0;i<result.cancel_count;++i)send(result.cancels[i],value.depth,now,sink,"C");
            for(std::uint16_t i=0;i<result.quote_count;++i)send(result.quotes[i],value.depth,now,sink,"Q");
        }
    }
    void on_order_event(std::uint32_t asset,bool terminal) override {if(terminal)engine_.remove_active_quote(asset);}
    void rejected(std::uint32_t asset) {engine_.remove_active_quote(asset);}
    void begin_frame() {last_results_.clear();}
    const std::vector<strategy::market_making::MarketMakingResult>& last_results() const {return last_results_;}
private:
    template<class Payload>void send(const Payload& value,const state::MarketDepthView& depth,std::uint64_t now,const RequestSink& sink,const char* kind) {
        auto metadata=identity_;metadata.market_state_version=depth.snapshot_version_hash;metadata.source_timestamp=depth.last_ws_recv_ns;metadata.availability_ns=now;
        metadata.request_id=std::string(kind)+":"+std::to_string(value.idempotency_hash);metadata.idempotency_key=metadata.request_id;
        sink(StrategyRequest{metadata,value});
    }
    strategy::market_making::MarketMakingEngine engine_;
    StrategyRequestMetadata identity_;
    std::vector<strategy::market_making::MarketMakingResult> last_results_;
};
}

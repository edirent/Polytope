#include "engine/signal/edge/LatencyBufferModel.h"

namespace trading_engine::signal {

LatencyBufferModel::LatencyBufferModel(
    std::int64_t default_latency_buffer_tick
) : default_latency_buffer_tick_(default_latency_buffer_tick) {}

std::int64_t LatencyBufferModel::estimate_latency_buffer_tick(
    const CandidateBundle&
) const noexcept {
    return default_latency_buffer_tick_;
}

}  // namespace trading_engine::signal

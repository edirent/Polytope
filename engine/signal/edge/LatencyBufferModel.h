#pragma once

#include "engine/signal/reader/OracleArtifactReader.h"

#include <cstdint>

namespace trading_engine::signal {

class LatencyBufferModel {
public:
    explicit LatencyBufferModel(std::int64_t default_latency_buffer_tick);

    [[nodiscard]] std::int64_t estimate_latency_buffer_tick(
        const CandidateBundle& bundle
    ) const noexcept;

private:
    std::int64_t default_latency_buffer_tick_ = 0;
};

}  // namespace trading_engine::signal

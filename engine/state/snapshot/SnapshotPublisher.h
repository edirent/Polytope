#pragma once

#include "state/MarketStateQueryResult.h"
#include "state/snapshot/SnapshotBuffer.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

class SnapshotPublisher {
public:
    void publish(const MarketStateSnapshot& snapshot);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> read(
        const std::string& asset_id
    ) const;

private:
    using BufferPtr = std::shared_ptr<SnapshotBuffer>;

    [[nodiscard]] BufferPtr get_or_create_buffer(
        const std::string& asset_id
    );

    [[nodiscard]] BufferPtr find_buffer(
        const std::string& asset_id
    ) const;

private:
    mutable std::mutex buffers_mutex_;
    std::unordered_map<std::string, BufferPtr> buffers_;
};

}  // namespace trading_engine::state

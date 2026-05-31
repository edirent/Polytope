#pragma once

#include "state/MarketStateQueryResult.h"
#include "state/snapshot/SnapshotBuffer.h"
#include "state/view/MarketDepthView.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

class SnapshotPublisher {
public:
    void publish(const MarketStateSnapshot& snapshot);

    [[nodiscard]] StateQueryResult<MarketStateSnapshot> read(
        const std::string& asset_id
    ) const;

    [[nodiscard]] std::uint16_t read_many(
        std::span<const std::string* const> asset_ids,
        MarketStateSnapshot* out,
        std::uint16_t max_out
    ) const;

    [[nodiscard]] std::uint16_t read_depth_many(
        std::span<const std::string* const> asset_ids,
        std::span<const std::uint32_t> asset_indices,
        MarketDepthView* out,
        std::uint16_t max_out
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

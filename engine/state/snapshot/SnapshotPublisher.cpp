#include "state/snapshot/SnapshotPublisher.h"

namespace trading_engine::state {

void SnapshotPublisher::publish(const MarketStateSnapshot& snapshot) {
    const std::string& asset_id = snapshot.entity_id;
    if (asset_id.empty()) {
        return;
    }

    const BufferPtr buffer = get_or_create_buffer(asset_id);
    const std::uint8_t active =
        buffer->active_index.load(std::memory_order_acquire);
    const std::uint8_t inactive = static_cast<std::uint8_t>(1U - active);

    MarketStateSnapshot published = snapshot;
    const std::uint64_t next_version =
        buffer->slots[active].snapshot.version + 1U;
    if (published.version < next_version) {
        published.version = next_version;
    }

    buffer->slots[inactive].snapshot = published;
    buffer->published.store(true, std::memory_order_release);
    buffer->active_index.store(inactive, std::memory_order_release);
}

StateQueryResult<MarketStateSnapshot> SnapshotPublisher::read(
    const std::string& asset_id
) const {
    StateQueryResult<MarketStateSnapshot> out;
    out.entity_id = asset_id;

    const BufferPtr buffer = find_buffer(asset_id);
    if (!buffer ||
        !buffer->published.load(std::memory_order_acquire)) {
        out.error = StateQueryError::MissingEntity;
        return out;
    }

    const std::uint8_t active =
        buffer->active_index.load(std::memory_order_acquire);

    out.value = buffer->slots[active].snapshot;
    out.entity_id = out.value.entity_id;
    out.version = out.value.version;
    out.ok = true;
    out.error = StateQueryError::None;
    return out;
}

SnapshotPublisher::BufferPtr SnapshotPublisher::get_or_create_buffer(
    const std::string& asset_id
) {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    auto& buffer = buffers_[asset_id];
    if (!buffer) {
        buffer = std::make_shared<SnapshotBuffer>();
    }
    return buffer;
}

SnapshotPublisher::BufferPtr SnapshotPublisher::find_buffer(
    const std::string& asset_id
) const {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    const auto it = buffers_.find(asset_id);
    if (it == buffers_.end()) {
        return {};
    }
    return it->second;
}

}  // namespace trading_engine::state

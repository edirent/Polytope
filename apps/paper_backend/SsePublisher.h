#pragma once

#include "engine/paper/read/DashboardReadStore.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace trading_engine::paper_backend {

struct SseEvent {
    std::uint64_t seq_no = 0;
    std::string payload;
};

class SsePublisher {
public:
    explicit SsePublisher(std::size_t capacity = 256);

    void publish(const trading_engine::paper::DashboardSnapshot& snapshot);

    [[nodiscard]] std::vector<SseEvent> read_since(
        std::uint64_t since_seq,
        std::size_t max_count = static_cast<std::size_t>(-1)
    ) const;

    [[nodiscard]] std::uint64_t latest_seq() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;

private:
    std::size_t capacity_ = 256;
    mutable std::mutex mutex_;
    std::deque<SseEvent> events_;
    std::uint64_t latest_seq_ = 0;
    std::uint64_t dropped_events_ = 0;
};

[[nodiscard]] std::string dashboard_sse_payload(
    const trading_engine::paper::DashboardSnapshot& snapshot
);

}  // namespace trading_engine::paper_backend

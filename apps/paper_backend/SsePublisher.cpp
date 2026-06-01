#include "apps/paper_backend/SsePublisher.h"

#include "apps/paper_backend/DashboardApiRoutes.h"

#include <utility>

namespace trading_engine::paper_backend {

SsePublisher::SsePublisher(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void SsePublisher::publish(
    const trading_engine::paper::DashboardSnapshot& snapshot
) {
    SseEvent event;
    event.payload = dashboard_sse_payload(snapshot);

    std::lock_guard lock{mutex_};
    event.seq_no = snapshot.seq_no == 0 ? latest_seq_ + 1 : snapshot.seq_no;
    latest_seq_ = event.seq_no;
    events_.push_back(std::move(event));
    while (events_.size() > capacity_) {
        events_.pop_front();
        ++dropped_events_;
    }
}

std::vector<SseEvent> SsePublisher::read_since(
    std::uint64_t since_seq,
    std::size_t max_count
) const {
    std::lock_guard lock{mutex_};
    std::vector<SseEvent> out;
    for (const auto& event : events_) {
        if (event.seq_no <= since_seq) {
            continue;
        }
        if (out.size() >= max_count) {
            break;
        }
        out.push_back(event);
    }
    return out;
}

std::uint64_t SsePublisher::latest_seq() const noexcept {
    std::lock_guard lock{mutex_};
    return latest_seq_;
}

std::uint64_t SsePublisher::dropped_events() const noexcept {
    std::lock_guard lock{mutex_};
    return dropped_events_;
}

std::string dashboard_sse_payload(
    const trading_engine::paper::DashboardSnapshot& snapshot
) {
    return "id: " + std::to_string(snapshot.seq_no) +
           "\nevent: dashboard\n" +
           "data: " + dashboard_snapshot_json(snapshot) + "\n\n";
}

}  // namespace trading_engine::paper_backend

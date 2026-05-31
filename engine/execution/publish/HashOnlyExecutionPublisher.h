#pragma once

#include "engine/execution/publish/ExecutionReportPublisher.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace trading_engine::execution {

struct HashOnlyExecutionRecord {
    std::uint64_t plan_id = 0;
    ChildOrderStatus status = ChildOrderStatus::Created;
    std::int64_t filled_qty_lots = 0;
    std::int64_t total_cost_tick = 0;
    std::uint64_t reservation_id = 0;
    ReservationDispositionType disposition = ReservationDispositionType::None;
};

class HashOnlyExecutionPublisher final
    : public ExecutionReportPublisher,
      public ReservationDispositionPublisher {
public:
    explicit HashOnlyExecutionPublisher(std::size_t reserve_capacity = 0);

    void publish(const ExecutionReport& report) override;
    void publish(const ReservationDisposition& disposition) override;

    void reserve(std::size_t capacity);
    void clear();

    [[nodiscard]] const std::vector<HashOnlyExecutionRecord>& records()
        const noexcept;

    [[nodiscard]] std::uint64_t output_hash() const noexcept;

private:
    std::vector<HashOnlyExecutionRecord> records_;
};

}  // namespace trading_engine::execution

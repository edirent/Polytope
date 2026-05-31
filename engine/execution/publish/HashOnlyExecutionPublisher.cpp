#include "engine/execution/publish/HashOnlyExecutionPublisher.h"

#include <charconv>
#include <string_view>

namespace trading_engine::execution {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    mix_u64(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t hash_string(std::string_view value) noexcept {
    auto hash = kFnvOffset;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t reservation_id_from_string(
    std::string_view value
) noexcept {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc{} && ptr == end) {
        return parsed;
    }
    return hash_string(value);
}

}  // namespace

HashOnlyExecutionPublisher::HashOnlyExecutionPublisher(
    std::size_t reserve_capacity
) {
    if (reserve_capacity > 0) {
        records_.reserve(reserve_capacity);
    }
}

void HashOnlyExecutionPublisher::publish(const ExecutionReport& report) {
    records_.push_back({
        .plan_id = report.plan_id,
        .status = report.status,
        .filled_qty_lots = report.filled_lots,
        .total_cost_tick = report.filled_lots * report.avg_fill_price_tick,
        .reservation_id = 0,
        .disposition = ReservationDispositionType::None
    });
}

void HashOnlyExecutionPublisher::publish(
    const ReservationDisposition& disposition
) {
    records_.push_back({
        .plan_id = disposition.plan_id,
        .status = ChildOrderStatus::Created,
        .filled_qty_lots = 0,
        .total_cost_tick = 0,
        .reservation_id = reservation_id_from_string(disposition.reservation_id),
        .disposition = disposition.type
    });
}

void HashOnlyExecutionPublisher::reserve(std::size_t capacity) {
    records_.reserve(capacity);
}

void HashOnlyExecutionPublisher::clear() {
    records_.clear();
}

const std::vector<HashOnlyExecutionRecord>&
HashOnlyExecutionPublisher::records() const noexcept {
    return records_;
}

std::uint64_t HashOnlyExecutionPublisher::output_hash() const noexcept {
    if (records_.empty()) {
        return 0;
    }

    auto hash = kFnvOffset;
    for (const auto& record : records_) {
        mix_u64(&hash, record.plan_id);
        mix_u64(&hash, static_cast<std::uint64_t>(record.status));
        mix_i64(&hash, record.filled_qty_lots);
        mix_i64(&hash, record.total_cost_tick);
        mix_u64(&hash, record.reservation_id);
        mix_u64(&hash, static_cast<std::uint64_t>(record.disposition));
    }
    return hash;
}

}  // namespace trading_engine::execution

#include "engine/signal/publish/HashOnlySignalPublisher.h"

namespace trading_engine::signal {

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

}  // namespace

HashOnlySignalPublisher::HashOnlySignalPublisher(
    std::size_t reserve_capacity
) {
    if (reserve_capacity > 0) {
        records_.reserve(reserve_capacity);
    }
}

void HashOnlySignalPublisher::publish(const OpportunityIntent& intent) {
    records_.push_back({
        .intent_id = intent.intent_id,
        .bundle_id = intent.bundle_id,
        .status = intent.status,
        .idempotency_hash = intent.idempotency_hash,
        .proof_hash = intent.proof_hash,
        .snapshot_version_hash = intent.snapshot_version_hash,
        .oracle_artifact_hash = intent.oracle_artifact_hash,
        .bundle_hash = intent.bundle_hash,
        .bundle_qty = intent.bundle_qty,
        .unit_edge_tick = intent.unit_edge_tick,
        .total_edge_tick = intent.total_edge_tick,
        .edge_bps = intent.edge_bps
    });
}

void HashOnlySignalPublisher::reserve(std::size_t capacity) {
    records_.reserve(capacity);
}

void HashOnlySignalPublisher::clear() {
    records_.clear();
}

const std::vector<HashOnlySignalRecord>& HashOnlySignalPublisher::records()
    const noexcept {
    return records_;
}

std::uint64_t HashOnlySignalPublisher::output_hash() const noexcept {
    if (records_.empty()) {
        return 0;
    }

    auto hash = kFnvOffset;
    for (const auto& record : records_) {
        mix_u64(&hash, record.intent_id);
        mix_u64(&hash, record.bundle_id);
        mix_u64(&hash, static_cast<std::uint64_t>(record.status));
        mix_u64(&hash, record.idempotency_hash);
        mix_u64(&hash, record.proof_hash);
        mix_u64(&hash, record.snapshot_version_hash);
        mix_u64(&hash, record.oracle_artifact_hash);
        mix_u64(&hash, record.bundle_hash);
        mix_i64(&hash, record.bundle_qty);
        mix_i64(&hash, record.unit_edge_tick);
        mix_i64(&hash, record.total_edge_tick);
        mix_i64(&hash, record.edge_bps);
    }
    return hash;
}

}  // namespace trading_engine::signal

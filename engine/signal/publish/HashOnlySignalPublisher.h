#pragma once

#include "engine/signal/publish/IIntentPublisher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace trading_engine::signal {

struct HashOnlySignalRecord {
    std::uint64_t intent_id = 0;
    std::uint64_t bundle_id = 0;

    IntentStatus status = IntentStatus::CandidateOnly;

    std::uint64_t idempotency_hash = 0;
    std::uint64_t proof_hash = 0;
    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t oracle_artifact_hash = 0;
    std::uint64_t bundle_hash = 0;

    std::int64_t bundle_qty = 0;
    std::int64_t unit_edge_tick = 0;
    std::int64_t total_edge_tick = 0;
    std::int64_t edge_bps = 0;
};

class HashOnlySignalPublisher final : public IIntentPublisher {
public:
    explicit HashOnlySignalPublisher(std::size_t reserve_capacity = 0);

    void publish(const OpportunityIntent& intent) override;

    [[nodiscard]] bool requires_materialized_strings() const noexcept override {
        return false;
    }

    void reserve(std::size_t capacity);
    void clear();

    [[nodiscard]] const std::vector<HashOnlySignalRecord>& records()
        const noexcept;

    [[nodiscard]] std::uint64_t output_hash() const noexcept;

private:
    std::vector<HashOnlySignalRecord> records_;
};

}  // namespace trading_engine::signal

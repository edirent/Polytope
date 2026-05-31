#pragma once

#include "engine/signal/publish/IIntentPublisher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace trading_engine::signal {

inline constexpr std::uint16_t kMaxSignalEvidenceSnapshots = kMaxIntentLegs;

struct CapturedSignalEvidence {
    std::uint16_t snapshot_count = 0;
    std::array<
        trading_engine::state::MarketStateSnapshot,
        kMaxSignalEvidenceSnapshots
    > snapshots{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t read_ts_ns = 0;

    [[nodiscard]] SignalEvidenceView view() const noexcept;
};

class CapturingIntentPublisher final : public IIntentPublisher {
public:
    void publish(const OpportunityIntent& intent) override;
    void publish(
        const OpportunityIntent& intent,
        const SignalEvidenceView& evidence
    ) override;

    [[nodiscard]] const std::vector<OpportunityIntent>& intents() const noexcept;

    [[nodiscard]] SignalEvidenceView evidence_at(std::size_t index) const
        noexcept;

private:
    std::vector<OpportunityIntent> intents_;
    std::vector<CapturedSignalEvidence> evidence_;
};

}  // namespace trading_engine::signal

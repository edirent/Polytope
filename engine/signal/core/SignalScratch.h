#pragma once

#include "engine/signal/pricing/PriceVectorBuilder.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace trading_engine::signal {

inline constexpr std::uint16_t kSignalScratchMaxIntents = 1024;
inline constexpr std::size_t kSignalScratchNoEvidenceIndex =
    std::numeric_limits<std::size_t>::max();

struct SignalPublishCandidate {
    OpportunityIntent intent;
    std::size_t evidence_index = kSignalScratchNoEvidenceIndex;
};

struct SignalScratch {
    std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>
        depth_views{};
    std::uint16_t depth_view_count = 0;

    std::array<PriceLeg, kMaxIntentLegs> price_legs{};
    std::uint16_t price_leg_count = 0;

    std::array<IntentLeg, kMaxIntentLegs> intent_legs{};

    std::array<OpportunityIntent, kSignalScratchMaxIntents> intents{};
    std::uint16_t intent_count = 0;

    std::array<SignalPublishCandidate, kSignalScratchMaxIntents>
        publish_candidates{};
    std::uint16_t publish_candidate_count = 0;

    void reset() noexcept {
        depth_view_count = 0;
        price_leg_count = 0;
        intent_count = 0;
        publish_candidate_count = 0;
    }

    [[nodiscard]] bool push_depth_view(
        const trading_engine::state::MarketDepthView& view
    ) {
        if (depth_view_count >= depth_views.size()) {
            return false;
        }
        depth_views[depth_view_count++] = view;
        return true;
    }

    [[nodiscard]] bool push_price_leg(const PriceLeg& leg) {
        if (price_leg_count >= price_legs.size()) {
            return false;
        }
        price_legs[price_leg_count++] = leg;
        return true;
    }

    [[nodiscard]] bool push_intent(OpportunityIntent intent) {
        if (intent_count >= intents.size()) {
            return false;
        }
        intents[intent_count++] = std::move(intent);
        return true;
    }

    [[nodiscard]] bool push_publish_candidate(
        OpportunityIntent intent,
        std::size_t evidence_index = kSignalScratchNoEvidenceIndex
    ) {
        if (publish_candidate_count >= publish_candidates.size()) {
            return false;
        }
        auto& candidate = publish_candidates[publish_candidate_count++];
        candidate.intent = std::move(intent);
        candidate.evidence_index = evidence_index;
        return true;
    }
};

}  // namespace trading_engine::signal

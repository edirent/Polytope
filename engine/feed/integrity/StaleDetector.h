#pragma once

#include "feed/state/EntityStateStore.h"

#include <cstdint>
#include <string>

namespace trading_engine::feed {

enum class StaleLevel {
    Ok = 0,
    SourceStale,
    EntityStale
};

struct StaleResult {
    StaleLevel level{StaleLevel::Ok};
    std::string entity_id;
    std::string reason;
    std::uint64_t age_ns{0};

    [[nodiscard]] bool ok() const noexcept {
        return level == StaleLevel::Ok;
    }
};

class StaleDetector {
public:
    explicit StaleDetector(
        std::uint64_t source_stale_timeout_ns = 30'000'000'000ULL,
        std::uint64_t entity_stale_timeout_ns = 300'000'000'000ULL
    ) noexcept;

    void configure_source_timeout(std::uint64_t timeout_ns) noexcept;
    void configure_entity_timeout(std::uint64_t timeout_ns) noexcept;

    [[nodiscard]] StaleResult check_source(
        std::uint64_t now_ns,
        std::uint64_t last_message_received_ns
    ) const noexcept;

    [[nodiscard]] StaleResult check_entity(
        std::uint64_t now_ns,
        const EntityState& entity
    ) const noexcept;

    [[nodiscard]] std::uint64_t source_stale_timeout_ns() const noexcept;
    [[nodiscard]] std::uint64_t entity_stale_timeout_ns() const noexcept;

private:
    std::uint64_t source_stale_timeout_ns_{30'000'000'000ULL};
    std::uint64_t entity_stale_timeout_ns_{300'000'000'000ULL};
};

[[nodiscard]] std::string to_string(StaleLevel level);

}  // namespace trading_engine::feed

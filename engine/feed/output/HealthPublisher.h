#pragma once

#include "feed/state/EntityStateStore.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::feed {

struct ProcessHealth {
    std::uint64_t uptime_ms{0};
    std::uint64_t raw_packets_total{0};
    std::uint64_t decode_errors_total{0};
    std::uint64_t normalization_errors_total{0};
    std::uint64_t state_errors_total{0};
};

struct SourceHealth {
    bool connected{false};
    std::uint64_t connection_id{0};
    std::uint64_t reconnect_count{0};
    std::uint64_t last_message_age_ms{0};
    std::uint64_t ping_sent_count{0};
    std::uint64_t pong_received_count{0};
};

struct EntityHealth {
    std::string entity_id;
    std::string status;
    bool initialized{false};
    std::uint64_t snapshot_count{0};
    std::uint64_t delta_count{0};
    std::uint64_t error_count{0};
    std::optional<double> best_bid;
    std::optional<double> best_ask;
    std::uint64_t state_hash{0};
};

struct ReplayHealth {
    std::uint64_t packets_read{0};
    std::uint64_t events_normalized{0};
    std::uint64_t events_applied{0};
    std::uint64_t global_hash{0};
    bool deterministic_trace_written{false};
};

struct HealthSnapshot {
    ProcessHealth process;
    SourceHealth source;
    std::vector<EntityHealth> entities;
    ReplayHealth replay;
};

class HealthPublisher {
public:
    /**
     * @brief Serialize a health snapshot as compact JSON.
     */
    [[nodiscard]] static std::string to_json(const HealthSnapshot& snapshot);

    /**
     * @brief Write a health snapshot as one JSON document to stdout.
     */
    void publish(const HealthSnapshot& snapshot) const;

    /**
     * @brief Write a health snapshot as one JSON document to a caller-owned stream.
     */
    void publish(const HealthSnapshot& snapshot, std::ostream& out) const;
};

[[nodiscard]] EntityHealth make_entity_health(
    const EntityState& entity,
    std::uint64_t state_hash
);

}  // namespace trading_engine::feed

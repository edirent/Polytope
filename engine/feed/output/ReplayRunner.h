#pragma once

#include "decode/core/DecodePipeline.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/state/EntityStateStore.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

namespace trading_engine::feed {

struct ReplaySummary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t events_applied{0};
    std::uint64_t trace_rows{0};
    std::uint64_t global_hash{0};
    bool deterministic_trace_written{false};
};

class ReplayRunner {
public:
    explicit ReplayRunner(std::string raw_log_path = {});

    void load(std::string raw_log_path);
    void reset();

    /**
     * @brief Replay the raw log and write deterministic CSV trace rows.
     *
     * CSV columns:
     *
     * packet_id,event_index,event_type,raw_type,entity_id,apply_code,
     * entity_status,entity_hash,global_hash
     */
    [[nodiscard]] ReplaySummary replay_to_csv(const std::string& trace_path);

    /**
     * @brief Replay the raw log and write deterministic CSV trace rows.
     */
    [[nodiscard]] ReplaySummary replay_to_csv(std::ostream& out);

private:
    std::unique_ptr<RawLogReader> reader_;
    trading_engine::decode::DecodePipeline pipeline_;
    EntityStateStore state_store_;
};

}  // namespace trading_engine::feed

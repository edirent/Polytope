#include "feed/output/ReplayRunner.h"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace trading_engine::feed {

namespace {

std::string csv_escape(const std::string& value) {
    bool needs_quotes = false;

    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (char c : value) {
        if (c == '"') {
            escaped.push_back('"');
        }

        escaped.push_back(c);
    }

    escaped.push_back('"');
    return escaped;
}

void write_trace_header(std::ostream& out) {
    out
        << "packet_id,event_index,event_type,raw_type,entity_id,"
        << "apply_code,entity_status,entity_hash,global_hash\n";
}

void write_trace_row(
    std::ostream& out,
    const NormalizedEvent& event,
    std::uint64_t event_index,
    const StateApplyResult& apply_result,
    const EntityStateStore& state_store
) {
    std::string entity_status;

    if (!apply_result.entity_id.empty()) {
        entity_status = to_string(state_store.status(apply_result.entity_id));
    }

    out
        << event.packet_id << ','
        << event_index << ','
        << csv_escape(to_string(event.event_type)) << ','
        << csv_escape(event.raw_type) << ','
        << csv_escape(apply_result.entity_id) << ','
        << csv_escape(to_string(apply_result.code)) << ','
        << csv_escape(entity_status) << ','
        << apply_result.entity_hash << ','
        << apply_result.global_hash << '\n';
}

}  // namespace

ReplayRunner::ReplayRunner(std::string raw_log_path) {
    if (!raw_log_path.empty()) {
        load(std::move(raw_log_path));
    }
}

void ReplayRunner::load(std::string raw_log_path) {
    reader_ = std::make_unique<RawLogReader>(std::move(raw_log_path));
    state_store_.reset();
}

void ReplayRunner::reset() {
    if (reader_) {
        reader_->reset();
    }

    state_store_.reset();
}

ReplaySummary ReplayRunner::replay_to_csv(const std::string& trace_path) {
    std::ofstream out(trace_path, std::ios::out | std::ios::trunc);

    if (!out.is_open()) {
        throw std::runtime_error("ReplayRunner failed to open trace: " + trace_path);
    }

    return replay_to_csv(out);
}

ReplaySummary ReplayRunner::replay_to_csv(std::ostream& out) {
    if (!reader_) {
        throw std::runtime_error("ReplayRunner has no raw log loaded");
    }

    reset();

    ReplaySummary summary;
    write_trace_header(out);

    while (true) {
        auto packet = reader_->next();
        if (packet.eof()) {
            break;
        }

        if (!packet.ok()) {
            throw std::runtime_error("ReplayRunner raw read failed: " + packet.message);
        }

        ++summary.packets_read;

        const auto decoded = decoder_.decode(*packet.packet);
        NormalizationResult normalized;

        if (decoded.has_json_event_payload()) {
            normalized = normalizer_.normalize_json(*packet.packet, decoded.json);
        } else if (decoded.has_control_payload()) {
            normalized =
                normalizer_.normalize_control(
                    *packet.packet,
                    decoded.control_payload
                );
        } else {
            throw std::runtime_error("ReplayRunner decode failed: " + decoded.message);
        }

        if (!normalized.ok()) {
            throw std::runtime_error("ReplayRunner normalize failed: " + normalized.error);
        }

        std::uint64_t event_index = 0;

        for (const auto& event : normalized.events) {
            const StateApplyResult apply_result = state_store_.apply(event);

            write_trace_row(
                out,
                event,
                event_index,
                apply_result,
                state_store_
            );

            ++event_index;
            ++summary.normalized_events;
            ++summary.events_applied;
            ++summary.trace_rows;
        }
    }

    out.flush();

    if (!out) {
        throw std::runtime_error("ReplayRunner failed while writing trace");
    }

    summary.global_hash = state_store_.global_hash();
    summary.deterministic_trace_written = true;

    return summary;
}

}  // namespace trading_engine::feed

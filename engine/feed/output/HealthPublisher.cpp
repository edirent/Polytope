#include "feed/output/HealthPublisher.h"

#include <boost/json.hpp>

#include <iostream>
#include <ostream>

namespace trading_engine::feed {

namespace {

namespace json = boost::json;

json::object process_to_json(const ProcessHealth& health) {
    json::object object;

    object["uptime_ms"] = health.uptime_ms;
    object["raw_packets_total"] = health.raw_packets_total;
    object["decode_errors_total"] = health.decode_errors_total;
    object["normalization_errors_total"] =
        health.normalization_errors_total;
    object["state_errors_total"] = health.state_errors_total;

    return object;
}

json::object source_to_json(const SourceHealth& health) {
    json::object object;

    object["connected"] = health.connected;
    object["connection_id"] = health.connection_id;
    object["reconnect_count"] = health.reconnect_count;
    object["last_message_age_ms"] = health.last_message_age_ms;
    object["ping_sent_count"] = health.ping_sent_count;
    object["pong_received_count"] = health.pong_received_count;

    return object;
}

void set_optional_double(
    json::object& object,
    const char* key,
    const std::optional<double>& value
) {
    if (value) {
        object[key] = *value;
        return;
    }

    object[key] = nullptr;
}

json::object entity_to_json(const EntityHealth& health) {
    json::object object;

    object["entity_id"] = health.entity_id;
    object["status"] = health.status;
    object["initialized"] = health.initialized;
    object["snapshot_count"] = health.snapshot_count;
    object["delta_count"] = health.delta_count;
    object["error_count"] = health.error_count;
    set_optional_double(object, "best_bid", health.best_bid);
    set_optional_double(object, "best_ask", health.best_ask);
    object["state_hash"] = health.state_hash;

    return object;
}

json::array entities_to_json(const std::vector<EntityHealth>& entities) {
    json::array array;

    for (const auto& entity : entities) {
        array.emplace_back(entity_to_json(entity));
    }

    return array;
}

json::object replay_to_json(const ReplayHealth& health) {
    json::object object;

    object["packets_read"] = health.packets_read;
    object["events_normalized"] = health.events_normalized;
    object["events_applied"] = health.events_applied;
    object["global_hash"] = health.global_hash;
    object["deterministic_trace_written"] =
        health.deterministic_trace_written;

    return object;
}

json::object snapshot_to_json(const HealthSnapshot& snapshot) {
    json::object object;

    object["process"] = process_to_json(snapshot.process);
    object["source"] = source_to_json(snapshot.source);
    object["entities"] = entities_to_json(snapshot.entities);
    object["replay"] = replay_to_json(snapshot.replay);

    return object;
}

}  // namespace

std::string HealthPublisher::to_json(const HealthSnapshot& snapshot) {
    return json::serialize(snapshot_to_json(snapshot));
}

void HealthPublisher::publish(const HealthSnapshot& snapshot) const {
    publish(snapshot, std::cout);
}

void HealthPublisher::publish(
    const HealthSnapshot& snapshot,
    std::ostream& out
) const {
    out << to_json(snapshot) << '\n';
}

EntityHealth make_entity_health(
    const EntityState& entity,
    std::uint64_t state_hash
) {
    return EntityHealth{
        .entity_id = entity.entity_id,
        .status = to_string(entity.status),
        .initialized = entity.initialized,
        .snapshot_count = entity.snapshot_count,
        .delta_count = entity.delta_count,
        .error_count = entity.error_count,
        .best_bid = entity.book.best_bid,
        .best_ask = entity.book.best_ask,
        .state_hash = state_hash
    };
}

}  // namespace trading_engine::feed

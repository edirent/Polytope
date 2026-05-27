#include "feed/output/HealthPublisher.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace json = boost::json;

using trading_engine::feed::EntityState;
using trading_engine::feed::EntityStatus;
using trading_engine::feed::HealthPublisher;
using trading_engine::feed::HealthSnapshot;
using trading_engine::feed::make_entity_health;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_equal_u64(
    std::uint64_t actual,
    std::uint64_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

std::uint64_t json_u64(const json::value& value) {
    if (value.is_uint64()) {
        return value.as_uint64();
    }

    if (value.is_int64() && value.as_int64() >= 0) {
        return static_cast<std::uint64_t>(value.as_int64());
    }

    fail("value is not a non-negative integer");
}

void expect_equal_string(
    const json::string& actual,
    const std::string& expected,
    const std::string& field
) {
    const std::string actual_string(actual.data(), actual.size());

    if (actual_string != expected) {
        fail("mismatch: " + field);
    }
}

HealthSnapshot sample_snapshot() {
    HealthSnapshot snapshot;

    snapshot.process.uptime_ms = 500;
    snapshot.process.raw_packets_total = 39;
    snapshot.process.decode_errors_total = 0;
    snapshot.process.normalization_errors_total = 0;
    snapshot.process.state_errors_total = 0;

    snapshot.source.connected = true;
    snapshot.source.connection_id = 7;
    snapshot.source.reconnect_count = 2;
    snapshot.source.last_message_age_ms = 12;
    snapshot.source.ping_sent_count = 3;
    snapshot.source.pong_received_count = 3;

    snapshot.replay.packets_read = 39;
    snapshot.replay.events_normalized = 39;
    snapshot.replay.events_applied = 39;
    snapshot.replay.global_hash = 123456;
    snapshot.replay.deterministic_trace_written = true;

    EntityState entity;
    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Live;
    entity.initialized = true;
    entity.snapshot_count = 1;
    entity.delta_count = 35;
    entity.error_count = 0;
    entity.book.best_bid = 0.50;
    entity.book.best_ask = 0.51;

    snapshot.entities.push_back(make_entity_health(entity, 999));

    return snapshot;
}

json::object parse_object(const std::string& text) {
    return json::parse(text).as_object();
}

void SnapshotSerializesProcessAndReplay() {
    const auto root = parse_object(HealthPublisher::to_json(sample_snapshot()));

    const auto& process = root.at("process").as_object();
    expect_equal_u64(
        json_u64(process.at("raw_packets_total")),
        39,
        "raw packets"
    );
    expect_equal_u64(
        json_u64(process.at("decode_errors_total")),
        0,
        "decode errors"
    );
    expect_equal_u64(
        json_u64(process.at("state_errors_total")),
        0,
        "state errors"
    );

    const auto& replay = root.at("replay").as_object();
    expect_equal_u64(
        json_u64(replay.at("packets_read")),
        39,
        "packets read"
    );
    expect_equal_u64(
        json_u64(replay.at("events_normalized")),
        39,
        "events normalized"
    );
    expect_equal_u64(
        json_u64(replay.at("global_hash")),
        123456,
        "global hash"
    );
}

void SnapshotSerializesSourceAndEntity() {
    const auto root = parse_object(HealthPublisher::to_json(sample_snapshot()));

    const auto& source = root.at("source").as_object();
    expect_true(source.at("connected").as_bool(), "source connected");
    expect_equal_u64(
        json_u64(source.at("connection_id")),
        7,
        "connection id"
    );
    expect_equal_u64(
        json_u64(source.at("ping_sent_count")),
        3,
        "ping sent count"
    );

    const auto& entities = root.at("entities").as_array();
    expect_equal_u64(entities.size(), 1, "entity count");

    const auto& entity = entities.front().as_object();
    expect_equal_string(
        entity.at("entity_id").as_string(),
        "asset-a",
        "entity id"
    );
    expect_equal_string(entity.at("status").as_string(), "Live", "status");
    expect_equal_u64(
        json_u64(entity.at("snapshot_count")),
        1,
        "snapshot count"
    );
    expect_equal_u64(json_u64(entity.at("delta_count")), 35, "delta count");
    expect_equal_u64(json_u64(entity.at("state_hash")), 999, "state hash");
}

void PublishWritesJsonLine() {
    std::ostringstream out;
    HealthPublisher publisher;

    publisher.publish(sample_snapshot(), out);

    const auto text = out.str();
    expect_true(!text.empty(), "publish output");
    expect_true(text.back() == '\n', "publish newline");

    const auto root = parse_object(text);
    expect_equal_u64(
        json_u64(root.at("replay").as_object().at("events_applied")),
        39,
        "events applied"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SnapshotSerializesProcessAndReplay", &SnapshotSerializesProcessAndReplay},
        {"SnapshotSerializesSourceAndEntity", &SnapshotSerializesSourceAndEntity},
        {"PublishWritesJsonLine", &PublishWritesJsonLine}
    };

    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}

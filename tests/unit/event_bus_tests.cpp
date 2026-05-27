#include "feed/output/EventBus.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::feed::EventBus;
using trading_engine::feed::HealthSnapshot;
using trading_engine::feed::NormalizedEvent;
using trading_engine::feed::NormalizedEventType;
using trading_engine::feed::StateApplyCode;
using trading_engine::feed::StateApplyResult;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal_int(int actual, int expected, const std::string& field) {
    if (actual != expected) {
        fail("mismatch: " + field);
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

void expect_equal_string(
    const std::string& actual,
    const std::string& expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void PublishEventCallsSubscribersSynchronously() {
    EventBus bus;
    NormalizedEvent event;

    event.packet_id = 42;
    event.event_type = NormalizedEventType::Delta;
    event.entity_id = "asset-a";

    int calls = 0;
    std::uint64_t seen_packet_id = 0;
    std::string seen_entity_id;

    bus.subscribe_event([&](const NormalizedEvent& published) {
        ++calls;
        seen_packet_id = published.packet_id;
        seen_entity_id = published.entity_id;
    });

    bus.publish_event(event);

    expect_equal_int(calls, 1, "event calls");
    expect_equal_u64(seen_packet_id, 42, "event packet id");
    expect_equal_string(seen_entity_id, "asset-a", "event entity id");
}

void PublishStateCallsStateSubscribersOnly() {
    EventBus bus;
    StateApplyResult result;

    result.code = StateApplyCode::Applied;
    result.entity_id = "asset-a";
    result.global_hash = 123;

    int event_calls = 0;
    int state_calls = 0;
    std::uint64_t seen_global_hash = 0;

    bus.subscribe_event([&](const NormalizedEvent&) {
        ++event_calls;
    });

    bus.subscribe_state([&](const StateApplyResult& published) {
        ++state_calls;
        seen_global_hash = published.global_hash;
    });

    bus.publish_state(result);

    expect_equal_int(event_calls, 0, "event calls");
    expect_equal_int(state_calls, 1, "state calls");
    expect_equal_u64(seen_global_hash, 123, "global hash");
}

void PublishHealthCallsHealthSubscribers() {
    EventBus bus;
    HealthSnapshot snapshot;

    snapshot.process.raw_packets_total = 39;
    snapshot.replay.events_normalized = 39;
    snapshot.replay.global_hash = 999;

    int calls = 0;
    std::uint64_t seen_packets = 0;
    std::uint64_t seen_events = 0;
    std::uint64_t seen_hash = 0;

    bus.subscribe_health([&](const HealthSnapshot& published) {
        ++calls;
        seen_packets = published.process.raw_packets_total;
        seen_events = published.replay.events_normalized;
        seen_hash = published.replay.global_hash;
    });

    bus.publish_health(snapshot);

    expect_equal_int(calls, 1, "health calls");
    expect_equal_u64(seen_packets, 39, "raw packets");
    expect_equal_u64(seen_events, 39, "events normalized");
    expect_equal_u64(seen_hash, 999, "global hash");
}

void PublishOrderIsSubscriptionOrder() {
    EventBus bus;
    std::vector<int> order;

    bus.subscribe_event([&](const NormalizedEvent&) {
        order.push_back(1);
    });

    bus.subscribe_event([&](const NormalizedEvent&) {
        order.push_back(2);
    });

    bus.publish_event(NormalizedEvent{});

    expect_equal_int(static_cast<int>(order.size()), 2, "order size");
    expect_equal_int(order[0], 1, "first handler");
    expect_equal_int(order[1], 2, "second handler");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"PublishEventCallsSubscribersSynchronously", &PublishEventCallsSubscribersSynchronously},
        {"PublishStateCallsStateSubscribersOnly", &PublishStateCallsStateSubscribersOnly},
        {"PublishHealthCallsHealthSubscribers", &PublishHealthCallsHealthSubscribers},
        {"PublishOrderIsSubscriptionOrder", &PublishOrderIsSubscriptionOrder}
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

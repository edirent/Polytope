#include "feed/integrity/StaleDetector.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::feed::EntityState;
using trading_engine::feed::EntityStatus;
using trading_engine::feed::StaleDetector;
using trading_engine::feed::StaleLevel;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal(
    StaleLevel actual,
    StaleLevel expected,
    const std::string& field
) {
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

EntityState live_entity(std::uint64_t last_update_ns) {
    EntityState entity;

    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Live;
    entity.initialized = true;
    entity.last_update_monotonic_ns = last_update_ns;

    return entity;
}

void LastMessageWithinTimeoutOk() {
    StaleDetector detector(100, 1000);

    const auto result = detector.check_source(1'000, 950);

    expect_equal(result.level, StaleLevel::Ok, "source within timeout");
    expect_equal_u64(result.age_ns, 50, "source age");
}

void SourceNoMessageBeyondThreshold() {
    StaleDetector detector(100, 1000);

    const auto result = detector.check_source(1'000, 899);

    expect_equal(result.level, StaleLevel::SourceStale, "source stale");
    expect_equal_u64(result.age_ns, 101, "source stale age");
}

void EntityNoUpdateBeyondThreshold() {
    StaleDetector detector(100, 100);
    const auto entity = live_entity(899);

    const auto result = detector.check_entity(1'000, entity);

    expect_equal(result.level, StaleLevel::EntityStale, "entity stale");
    expect_equal_u64(result.age_ns, 101, "entity stale age");
}

void ClosedEntityIgnored() {
    StaleDetector detector(100, 100);
    auto entity = live_entity(1);

    entity.closed = true;
    entity.status = EntityStatus::Closed;

    const auto result = detector.check_entity(1'000, entity);

    expect_equal(result.level, StaleLevel::Ok, "closed entity ignored");
    expect_equal_u64(result.age_ns, 0, "closed entity age");
}

void UninitializedEntityIgnored() {
    StaleDetector detector(100, 100);

    EntityState entity;
    entity.entity_id = "asset-a";
    entity.status = EntityStatus::Uninitialized;
    entity.initialized = false;
    entity.last_update_monotonic_ns = 1;

    const auto result = detector.check_entity(1'000, entity);

    expect_equal(result.level, StaleLevel::Ok, "uninitialized entity ignored");
    expect_equal_u64(result.age_ns, 0, "uninitialized entity age");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"LastMessageWithinTimeoutOk", &LastMessageWithinTimeoutOk},
        {"SourceNoMessageBeyondThreshold", &SourceNoMessageBeyondThreshold},
        {"EntityNoUpdateBeyondThreshold", &EntityNoUpdateBeyondThreshold},
        {"ClosedEntityIgnored", &ClosedEntityIgnored},
        {"UninitializedEntityIgnored", &UninitializedEntityIgnored}
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

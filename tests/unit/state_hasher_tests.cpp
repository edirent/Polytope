#include "feed/state/StateHasher.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::feed::EntityState;
using trading_engine::feed::EntityStatus;
using trading_engine::feed::StateHasher;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& message) {
    if (!value) {
        fail(message);
    }
}

void expect_equal(
    std::uint64_t actual,
    std::uint64_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void expect_not_equal(
    std::uint64_t actual,
    std::uint64_t expected,
    const std::string& field
) {
    if (actual == expected) {
        fail("unexpected equal hash: " + field);
    }
}

EntityState sample_entity(std::string entity_id) {
    EntityState entity;

    entity.entity_id = std::move(entity_id);
    entity.status = EntityStatus::Live;
    entity.initialized = true;
    entity.snapshot_count = 1;
    entity.delta_count = 2;

    entity.first_packet_id = 10;
    entity.last_packet_id = 12;
    entity.last_snapshot_packet_id = 10;
    entity.last_update_monotonic_ns = 123456789;

    entity.book.bids[0.50] = 100.0;
    entity.book.bids[0.49] = 200.0;
    entity.book.asks[0.51] = 150.0;
    entity.book.asks[0.52] = 250.0;

    entity.book.best_bid = 0.50;
    entity.book.best_ask = 0.51;
    entity.book.tick_size = 0.01;

    return entity;
}

void SameEntitySameHash() {
    const EntityState entity = sample_entity("asset-a");

    expect_equal(
        StateHasher::hash_entity(entity),
        StateHasher::hash_entity(entity),
        "same entity hash"
    );
}

void DifferentBookDifferentHash() {
    EntityState left = sample_entity("asset-a");
    EntityState right = sample_entity("asset-a");

    right.book.bids[0.50] = 101.0;

    expect_not_equal(
        StateHasher::hash_entity(left),
        StateHasher::hash_entity(right),
        "different book"
    );
}

void DifferentStatusDifferentHash() {
    EntityState left = sample_entity("asset-a");
    EntityState right = sample_entity("asset-a");

    right.status = EntityStatus::Closed;
    right.closed = true;

    expect_not_equal(
        StateHasher::hash_entity(left),
        StateHasher::hash_entity(right),
        "different status"
    );
}

void TimestampDoesNotChangeHash() {
    EntityState left = sample_entity("asset-a");
    EntityState right = sample_entity("asset-a");

    right.last_update_monotonic_ns = left.last_update_monotonic_ns + 999999;

    expect_equal(
        StateHasher::hash_entity(left),
        StateHasher::hash_entity(right),
        "timestamp-independent hash"
    );
}

void MapInsertionOrderDoesNotChangeHash() {
    const EntityState asset_a = sample_entity("asset-a");
    const EntityState asset_b = sample_entity("asset-b");

    std::map<std::string, EntityState> first;
    first.emplace(asset_a.entity_id, asset_a);
    first.emplace(asset_b.entity_id, asset_b);

    std::map<std::string, EntityState> second;
    second.emplace(asset_b.entity_id, asset_b);
    second.emplace(asset_a.entity_id, asset_a);

    expect_equal(
        StateHasher::hash_entity_map(first),
        StateHasher::hash_entity_map(second),
        "map insertion order"
    );
}

void GlobalHashStableForSameEntities() {
    std::map<std::string, EntityState> first{
        {"asset-a", sample_entity("asset-a")},
        {"asset-b", sample_entity("asset-b")}
    };

    std::map<std::string, EntityState> second{
        {"asset-a", sample_entity("asset-a")},
        {"asset-b", sample_entity("asset-b")}
    };

    expect_equal(
        StateHasher::hash_entity_map(first),
        StateHasher::hash_entity_map(second),
        "global hash stable"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SameEntitySameHash", &SameEntitySameHash},
        {"DifferentBookDifferentHash", &DifferentBookDifferentHash},
        {"DifferentStatusDifferentHash", &DifferentStatusDifferentHash},
        {"TimestampDoesNotChangeHash", &TimestampDoesNotChangeHash},
        {"MapInsertionOrderDoesNotChangeHash", &MapInsertionOrderDoesNotChangeHash},
        {"GlobalHashStableForSameEntities", &GlobalHashStableForSameEntities}
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

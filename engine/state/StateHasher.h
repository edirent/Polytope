#pragma once

#include "state/EntityStateStore.h"

#include <cstdint>
#include <map>
#include <string>

namespace trading_engine::state {

/**
 * @brief Deterministic hasher for entity/order-book state.
 *
 * StateHasher is used by:
 *
 * - EntityStateStore::state_hash()
 * - EntityStateStore::global_hash()
 * - ReplayRunner trace generation
 * - state determinism tests
 *
 * Design rules:
 *
 * 1. Do not hash wall-clock time.
 * 2. Do not hash monotonic time.
 * 3. Do not hash pointer addresses.
 * 4. Do not depend on unordered_map iteration order.
 * 5. Do hash semantic state:
 *    - entity status
 *    - counters
 *    - order book levels
 *    - BBO fields
 *    - resolution fields
 *
 * The goal is:
 *
 *     same event sequence -> same state hash
 */
class StateHasher {
public:
    /**
     * @brief Hash one complete entity state.
     */
    [[nodiscard]] static std::uint64_t hash_entity(
        const EntityState& entity
    ) noexcept;

    /**
     * @brief Hash one order book state.
     */
    [[nodiscard]] static std::uint64_t hash_order_book(
        const OrderBookState& book
    ) noexcept;

    /**
     * @brief Hash all entities in deterministic map order.
     *
     * std::map iteration order is deterministic by key.
     */
    [[nodiscard]] static std::uint64_t hash_entity_map(
        const std::map<std::string, EntityState>& entities
    ) noexcept;
};

}  // namespace trading_engine::state
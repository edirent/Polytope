#include "feed/state/StateHasher.h"

#include <cstring>
#include <optional>
#include <type_traits>

namespace trading_engine::feed {

namespace {

/**
 * @brief FNV-1a 64-bit constants.
 *
 * This is not cryptographic. It is just a simple deterministic hash suitable
 * for replay traces and state comparison.
 */
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

/**
 * @brief Mix one byte into FNV-1a hash.
 */
void hash_byte(std::uint64_t& h, std::uint8_t b) noexcept {
    h ^= b;
    h *= kFnvPrime;
}

/**
 * @brief Hash uint64 in deterministic little-endian order.
 */
void hash_u64(std::uint64_t& h, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        const auto b = static_cast<std::uint8_t>(
            (value >> (i * 8)) & 0xFF
        );

        hash_byte(h, b);
    }
}

/**
 * @brief Hash uint32.
 */
void hash_u32(std::uint64_t& h, std::uint32_t value) noexcept {
    hash_u64(h, static_cast<std::uint64_t>(value));
}

/**
 * @brief Hash bool as one byte.
 */
void hash_bool(std::uint64_t& h, bool value) noexcept {
    hash_byte(h, value ? 1 : 0);
}

/**
 * @brief Hash string with length prefix.
 *
 * Length prefix avoids collisions such as:
 *
 *     ["ab", "c"] vs ["a", "bc"]
 */
void hash_string(std::uint64_t& h, const std::string& value) noexcept {
    hash_u64(h, static_cast<std::uint64_t>(value.size()));

    for (unsigned char c : value) {
        hash_byte(h, static_cast<std::uint8_t>(c));
    }
}

/**
 * @brief Hash double by IEEE-754 bit pattern.
 *
 * This gives deterministic behavior for the same parsed double values.
 *
 * We canonicalize -0.0 to +0.0 so the two equivalent numeric values hash the
 * same.
 */
void hash_double(std::uint64_t& h, double value) noexcept {
    if (value == 0.0) {
        value = 0.0;  // canonicalize -0.0 to +0.0
    }

    std::uint64_t bits = 0;

    static_assert(
        sizeof(bits) == sizeof(value),
        "double must be 64-bit for StateHasher"
    );

    std::memcpy(&bits, &value, sizeof(double));

    hash_u64(h, bits);
}

/**
 * @brief Hash optional<double>.
 */
void hash_optional_double(
    std::uint64_t& h,
    const std::optional<double>& value
) noexcept {
    hash_bool(h, value.has_value());

    if (value.has_value()) {
        hash_double(h, *value);
    }
}

/**
 * @brief Hash deterministic price level map.
 *
 * Works for:
 *
 * - bids: std::map<double, double, std::greater<double>>
 * - asks: std::map<double, double>
 */
template <typename MapT>
void hash_price_levels(std::uint64_t& h, const MapT& levels) noexcept {
    hash_u64(h, static_cast<std::uint64_t>(levels.size()));

    for (const auto& [price, size] : levels) {
        hash_double(h, price);
        hash_double(h, size);
    }
}

}  // namespace

std::uint64_t StateHasher::hash_order_book(
    const OrderBookState& book
) noexcept {
    std::uint64_t h = kFnvOffset;

    /**
     * Hash derived BBO fields.
     *
     * These are redundant with book levels, but useful because they catch bugs
     * where BBO fields are stale relative to levels.
     */
    hash_optional_double(h, book.best_bid);
    hash_optional_double(h, book.best_ask);

    /**
     * Hash external BBO reference fields.
     *
     * These come from best_bid_ask events and are not the same as local book
     * levels.
     */
    hash_optional_double(h, book.external_best_bid);
    hash_optional_double(h, book.external_best_ask);

    /**
     * Hash tick size if known.
     */
    hash_optional_double(h, book.tick_size);

    /**
     * Hash state flags.
     */
    hash_bool(h, book.crossed);
    hash_bool(h, book.external_bbo_diverged);
    hash_bool(h, book.resolved);

    /**
     * Hash resolution payload.
     */
    hash_string(h, book.winning_asset_id);

    /**
     * Hash actual order book levels.
     *
     * Map iteration order is deterministic:
     *
     * - bids use descending price order
     * - asks use ascending price order
     */
    hash_price_levels(h, book.bids);
    hash_price_levels(h, book.asks);

    return h;
}

std::uint64_t StateHasher::hash_entity(
    const EntityState& entity
) noexcept {
    std::uint64_t h = kFnvOffset;

    /**
     * Entity identity.
     *
    hash_string(h, entity.entity_id);

    /**
     * Entity lifecycle/status.
     */
    hash_u32(h, static_cast<std::uint32_t>(entity.status));

    hash_bool(h, entity.initialized);
    hash_bool(h, entity.recovering);
    hash_bool(h, entity.inconsistent);
    hash_bool(h, entity.closed);

    /**
     * Deterministic semantic counters.
     *
     * These are included because two books may look identical but have reached
     * that state through different event histories. For replay diagnostics,
     * that distinction is useful.
     */
    hash_u64(h, entity.snapshot_count);
    hash_u64(h, entity.delta_count);
    hash_u64(h, entity.status_change_count);
    hash_u64(h, entity.lifecycle_count);
    hash_u64(h, entity.ignored_count);
    hash_u64(h, entity.error_count);

    /**
     * Do not hash:
     *
     * - first_packet_id
     * - last_packet_id
     * - last_snapshot_packet_id
     * - last_update_monotonic_ns
     *
     * These are useful diagnostics, but they are not core market state.
     * The replay trace already records packet_id separately.
     */
    const std::uint64_t book_hash = hash_order_book(entity.book);
    hash_u64(h, book_hash);

    return h;
}

std::uint64_t StateHasher::hash_entity_map(
    const std::map<std::string, EntityState>& entities
) noexcept {
    std::uint64_t h = kFnvOffset;

    hash_u64(h, static_cast<std::uint64_t>(entities.size()));

    for (const auto& [entity_id, entity] : entities) {
        hash_string(h, entity_id);
        hash_u64(h, hash_entity(entity));
    }

    return h;
}

}  // namespace trading_engine::feed
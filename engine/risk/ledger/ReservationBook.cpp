#include "engine/risk/ledger/ReservationBook.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] ReservationResult reject(
    RiskRejectReason reason,
    std::string detail
) {
    ReservationResult result;
    result.ok = false;
    result.reject_reason = reason;
    result.detail = std::move(detail);
    return result;
}

[[nodiscard]] ReservationResult accept(std::uint64_t reservation_id) {
    ReservationResult result;
    result.ok = true;
    result.reject_reason = RiskRejectReason::None;
    result.reservation_id = reservation_id;
    return result;
}

[[nodiscard]] bool is_active(const RiskReservation& reservation) noexcept {
    return reservation.status == ReservationStatus::Active;
}

[[nodiscard]] std::uint64_t hash_string(std::string_view value) noexcept {
    auto hash = 14695981039346656037ULL;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::uint32_t compact_index(std::string_view value) noexcept {
    const auto hash = hash_string(value);
    return static_cast<std::uint32_t>(
        (hash >> 32U) ^ (hash & 0xffff'ffffULL)
    );
}

[[nodiscard]] ReservationKey reservation_key_for(
    const signal::OpportunityIntent& intent
) noexcept {
    return ReservationKey{
        .idempotency_hash = intent.idempotency_hash != 0
            ? intent.idempotency_hash
            : hash_string(intent.idempotency_key),
        .bundle_id = intent.bundle_id
    };
}

[[nodiscard]] RiskReservation* find_reservation(
    std::vector<RiskReservation>* reservations,
    std::uint64_t reservation_id
) noexcept {
    if (reservations == nullptr || reservation_id == 0 ||
        reservation_id > reservations->size()) {
        return nullptr;
    }

    auto& reservation = (*reservations)[reservation_id - 1];
    return reservation.reservation_id == reservation_id ? &reservation :
                                                          nullptr;
}

void add_asset_lot(
    RiskReservation* reservation,
    const std::string* asset_id,
    std::int64_t lots
) {
    if (reservation == nullptr || asset_id == nullptr || asset_id->empty() ||
        lots == 0) {
        return;
    }

    for (std::uint8_t i = 0; i < reservation->asset_count; ++i) {
        auto& asset = reservation->assets[i];
        if (asset.asset_id != nullptr && *asset.asset_id == *asset_id) {
            asset.lots += lots;
            return;
        }
    }

    if (reservation->asset_count >= kMaxReservedLegs) {
        return;
    }

    auto& asset = reservation->assets[reservation->asset_count++];
    asset.asset_index = compact_index(*asset_id);
    asset.asset_id = asset_id;
    asset.lots = lots;
}

void add_market_exposure(
    RiskReservation* reservation,
    const std::string* market_id,
    std::int64_t exposure_tick
) {
    if (reservation == nullptr || market_id == nullptr || market_id->empty() ||
        exposure_tick == 0) {
        return;
    }

    for (std::uint8_t i = 0; i < reservation->market_count; ++i) {
        auto& market = reservation->markets[i];
        if (market.market_id != nullptr && *market.market_id == *market_id) {
            market.exposure_tick += exposure_tick;
            return;
        }
    }

    if (reservation->market_count >= kMaxReservedLegs) {
        return;
    }

    auto& market = reservation->markets[reservation->market_count++];
    market.market_index = compact_index(*market_id);
    market.market_id = market_id;
    market.exposure_tick = exposure_tick;
}

}  // namespace

ReservationBook::ReservationBook() {
    reservations_.reserve(4096);
    active_reservation_keys_.reserve(4096);
    interned_strings_.reserve(4096);
}

const std::string* ReservationBook::intern_string(const std::string& value) {
    if (value.empty()) {
        return nullptr;
    }
    const auto [it, _] = interned_strings_.insert(value);
    return &*it;
}

ReservationResult ReservationBook::try_reserve(
    const signal::OpportunityIntent& intent,
    const RiskDecision& decision,
    std::uint64_t now_ns,
    bool track_ledger_detail
) {
    if (!decision.approved()) {
        return reject(
            RiskRejectReason::NotEvaluated,
            "reservation requires approved risk decision"
        );
    }
    if (intent.idempotency_hash == 0 && intent.idempotency_key.empty()) {
        return reject(
            RiskRejectReason::MissingEvidence,
            "reservation requires idempotency evidence"
        );
    }
    const auto key = reservation_key_for(intent);
    if (key.idempotency_hash == 0) {
        return reject(
            RiskRejectReason::MissingEvidence,
            "reservation requires idempotency_hash"
        );
    }
    if (intent.expires_at_ns <= now_ns) {
        return reject(RiskRejectReason::ExpiredIntent, "intent expired");
    }

    const auto reservation_id = next_reservation_id_;
    const auto [_, inserted] =
        active_reservation_keys_.emplace(key, reservation_id);
    if (!inserted) {
        return reject(
            RiskRejectReason::DuplicateReservation,
            "active reservation already exists for idempotency_hash"
        );
    }

    ++next_reservation_id_;

    reservations_.emplace_back();
    auto& reservation = reservations_.back();
    build_reservation(intent, now_ns, &reservation, track_ledger_detail);
    reservation.idempotency_hash = key.idempotency_hash;
    reservation.reservation_id = reservation_id;

    ledger_.reserve(reservation);
    reservation.ledger_reserved = true;

    return accept(reservation_id);
}

void ReservationBook::release(std::uint64_t reservation_id) {
    auto* reservation = find_reservation(&reservations_, reservation_id);
    if (reservation == nullptr || !is_active(*reservation)) {
        return;
    }

    if (reservation->ledger_reserved) {
        ledger_.release(*reservation);
        reservation->ledger_reserved = false;
    }
    active_reservation_keys_.erase(
        ReservationKey{
            .idempotency_hash = reservation->idempotency_hash,
            .bundle_id = reservation->bundle_id
        }
    );
    reservation->status = ReservationStatus::Released;
}

void ReservationBook::expire_old(std::uint64_t now_ns) {
    for (auto& reservation : reservations_) {
        if (!is_active(reservation) || reservation.expires_at_ns > now_ns) {
            continue;
        }

        if (reservation.ledger_reserved) {
            ledger_.release(reservation);
            reservation.ledger_reserved = false;
        }
        active_reservation_keys_.erase(
            ReservationKey{
                .idempotency_hash = reservation.idempotency_hash,
                .bundle_id = reservation.bundle_id
            }
        );
        reservation.status = ReservationStatus::Expired;
    }
}

RiskLedgerSnapshot ReservationBook::snapshot() const {
    auto out = ledger_.snapshot();

    for (const auto& reservation : reservations_) {
        switch (reservation.status) {
            case ReservationStatus::Active:
                ++out.active_reservations;
                break;
            case ReservationStatus::Released:
                ++out.released_reservations;
                break;
            case ReservationStatus::Expired:
                ++out.expired_reservations;
                break;
            case ReservationStatus::Consumed:
                ++out.consumed_reservations;
                break;
        }
    }

    return out;
}

void ReservationBook::build_reservation(
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns,
    RiskReservation* reservation,
    bool track_ledger_detail
) {
    if (reservation == nullptr) {
        return;
    }

    reservation->intent_id = intent.intent_id;
    reservation->bundle_id = intent.bundle_id;
    reservation->reserved_cost_tick = intent.estimated_cost_tick;
    reservation->reserved_exposure_tick = std::max<std::int64_t>(
        intent.estimated_cost_tick,
        0
    );
    reservation->created_ts_ns = now_ns;
    reservation->expires_at_ns = intent.expires_at_ns;
    reservation->idempotency_key = intent.idempotency_key;
    reservation->idempotency_hash = intent.idempotency_hash != 0
        ? intent.idempotency_hash
        : hash_string(intent.idempotency_key);

    if (!track_ledger_detail) {
        return;
    }

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        const auto* asset_id = intern_string(leg.asset_id);
        const auto* market_id = intern_string(leg.market_id);
        add_asset_lot(reservation, asset_id, leg.quantity_lots);
        add_market_exposure(
            reservation,
            market_id,
            std::max<std::int64_t>(leg.estimated_cost_tick, 0)
        );
    }
}

}  // namespace trading_engine::risk

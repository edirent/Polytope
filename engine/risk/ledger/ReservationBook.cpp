#include "engine/risk/ledger/ReservationBook.h"

#include <algorithm>
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

}  // namespace

ReservationResult ReservationBook::try_reserve(
    const signal::OpportunityIntent& intent,
    const RiskDecision& decision,
    std::uint64_t now_ns
) {
    if (!decision.approved()) {
        return reject(
            RiskRejectReason::NotEvaluated,
            "reservation requires approved risk decision"
        );
    }
    if (intent.idempotency_key.empty()) {
        return reject(
            RiskRejectReason::MissingEvidence,
            "reservation requires idempotency_key"
        );
    }
    if (intent.expires_at_ns <= now_ns) {
        return reject(RiskRejectReason::ExpiredIntent, "intent expired");
    }
    if (active_idempotency_keys_.contains(intent.idempotency_key)) {
        return reject(
            RiskRejectReason::DuplicateReservation,
            "active reservation already exists for idempotency_key"
        );
    }

    auto reservation = build_reservation(intent, now_ns);
    reservation.reservation_id = next_reservation_id_++;

    ledger_.reserve(reservation);
    active_idempotency_keys_[reservation.idempotency_key] =
        reservation.reservation_id;
    reservations_.emplace(reservation.reservation_id, std::move(reservation));

    return accept(next_reservation_id_ - 1);
}

void ReservationBook::release(std::uint64_t reservation_id) {
    auto it = reservations_.find(reservation_id);
    if (it == reservations_.end() || !is_active(it->second)) {
        return;
    }

    ledger_.release(it->second);
    active_idempotency_keys_.erase(it->second.idempotency_key);
    it->second.status = ReservationStatus::Released;
}

void ReservationBook::expire_old(std::uint64_t now_ns) {
    for (auto& [_, reservation] : reservations_) {
        if (!is_active(reservation) || reservation.expires_at_ns > now_ns) {
            continue;
        }

        ledger_.release(reservation);
        active_idempotency_keys_.erase(reservation.idempotency_key);
        reservation.status = ReservationStatus::Expired;
    }
}

RiskLedgerSnapshot ReservationBook::snapshot() const {
    auto out = ledger_.snapshot();

    for (const auto& [_, reservation] : reservations_) {
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

RiskReservation ReservationBook::build_reservation(
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns
) const {
    RiskReservation reservation;
    reservation.intent_id = intent.intent_id;
    reservation.bundle_id = intent.bundle_id;
    reservation.reserved_cost_tick = intent.estimated_cost_tick;
    reservation.reserved_exposure_tick = std::max<std::int64_t>(
        intent.estimated_cost_tick,
        0
    );
    reservation.created_ts_ns = now_ns;
    reservation.expires_at_ns = intent.expires_at_ns;
    reservation.idempotency_key = intent.idempotency_key;

    for (std::uint16_t i = 0; i < intent.leg_count; ++i) {
        const auto& leg = intent.legs[i];
        reservation.reserved_asset_lots[leg.asset_id] += leg.quantity_lots;
        reservation.reserved_market_exposure_tick[leg.market_id] +=
            std::max<std::int64_t>(leg.estimated_cost_tick, 0);
    }

    return reservation;
}

}  // namespace trading_engine::risk

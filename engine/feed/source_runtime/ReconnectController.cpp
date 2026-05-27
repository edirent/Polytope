#include "feed/source_runtime/ReconnectController.h"

#include <algorithm>

namespace trading_engine::feed {

/**
 * @brief Convert milliseconds to nanoseconds.
 *
 * The rest of the feed runtime uses monotonic timestamps in nanoseconds.
 */
std::uint64_t ReconnectController::ms_to_ns(std::uint64_t ms) noexcept {
    return ms * 1'000'000ULL;
}

/**
 * @brief Construct reconnect controller.
 *
 * Example:
 *
 *     ReconnectController reconnect(
 *         500,     // initial delay: 500 ms
 *         5'000,   // max delay: 5 seconds
 *         0        // unlimited attempts
 *     );
 *
 * If max_attempts == 0, reconnect attempts are unlimited.
 */
ReconnectController::ReconnectController(
    std::uint64_t initial_delay_ms,
    std::uint64_t max_delay_ms,
    std::uint32_t max_attempts
)
    : initial_delay_ns_(ms_to_ns(initial_delay_ms)),
      max_delay_ns_(ms_to_ns(max_delay_ms)),
      current_delay_ns_(ms_to_ns(initial_delay_ms)),
      max_attempts_(max_attempts) {
    if (max_delay_ns_ < initial_delay_ns_) {
        max_delay_ns_ = initial_delay_ns_;
    }
}

/**
 * @brief Record successful connection open.
 *
 * This should be called after:
 *
 * - first successful connect;
 * - successful reconnect.
 *
 * It increments connection_id because every successful connection represents
 * a new logical transport session.
 *
 * RawPacket should later store this connection_id so replay/debug can identify
 * whether packets came before or after reconnect.
 */
void ReconnectController::on_connection_open(std::uint64_t now_ns) noexcept {
    ++connection_id_;

    reconnect_requested_ = false;
    reconnecting_ = false;
    disabled_ = false;

    reason_ = ReconnectReason::None;

    attempt_count_ = 0;
    current_delay_ns_ = initial_delay_ns_;

    last_success_ns_ = now_ns;
}

/**
 * @brief Request reconnect.
 *
 * This function is idempotent:
 *
 * If reconnect is already requested, calling it again does not reset the
 * backoff timer or attempt count.
 *
 * This avoids a bug where repeated errors constantly push reconnect into the
 * future and prevent reconnect from ever happening.
 */
void ReconnectController::request_reconnect(
    std::uint64_t now_ns,
    ReconnectReason reason
) noexcept {
    if (disabled_) {
        return;
    }

    if (!reconnect_requested_ && !reconnecting_) {
        reconnect_requested_ = true;
        last_request_ns_ = now_ns;
        reason_ = reason;
    }
}

/**
 * @brief Decide whether runtime should attempt reconnect now.
 *
 * Rules:
 *
 * 1. If disabled, no reconnect.
 * 2. If no reconnect was requested, no reconnect.
 * 3. If already reconnecting, no new reconnect attempt.
 * 4. If max_attempts reached, no reconnect.
 * 5. If no attempt has happened yet, wait current_delay since request time.
 * 6. Otherwise wait current_delay since last attempt time.
 */
bool ReconnectController::should_reconnect(std::uint64_t now_ns) const noexcept {
    if (disabled_) {
        return false;
    }

    if (!reconnect_requested_) {
        return false;
    }

    if (reconnecting_) {
        return false;
    }

    if (max_attempts_ != 0 && attempt_count_ >= max_attempts_) {
        return false;
    }

    const std::uint64_t base_time =
        last_attempt_ns_ == 0 ? last_request_ns_ : last_attempt_ns_;

    if (now_ns < base_time) {
        return false;
    }

    return now_ns - base_time >= current_delay_ns_;
}

/**
 * @brief Mark reconnect attempt started.
 *
 * This does not know whether the attempt will succeed.
 * The runtime must later call either:
 *
 * - on_reconnect_success()
 * - on_reconnect_failure()
 */
void ReconnectController::on_reconnect_attempt(std::uint64_t now_ns) noexcept {
    if (disabled_) {
        return;
    }

    reconnecting_ = true;
    last_attempt_ns_ = now_ns;
    ++attempt_count_;
}

/**
 * @brief Mark reconnect success.
 *
 * Success means:
 *
 * - runtime has established WebSocket connection;
 * - TLS/WebSocket handshake succeeded;
 * - connection is usable.
 *
 * This increments reconnect_count and then calls on_connection_open() to create
 * a new connection_id and reset backoff state.
 */
void ReconnectController::on_reconnect_success(std::uint64_t now_ns) noexcept {
    ++reconnect_count_;
    on_connection_open(now_ns);
}

/**
 * @brief Mark reconnect failure and increase backoff.
 *
 * Failure means the attempted reconnect did not produce a usable connection.
 *
 * This puts the controller back into requested state so a future attempt can
 * happen after current_delay_ns_ elapses.
 *
 * Backoff rule:
 *
 *     current_delay = min(current_delay * 2, max_delay)
 *
 * If max_attempts is configured and reached, reconnect becomes disabled.
 */
void ReconnectController::on_reconnect_failure(std::uint64_t /*now_ns*/) noexcept {
    reconnecting_ = false;

    if (max_attempts_ != 0 && attempt_count_ >= max_attempts_) {
        disabled_ = true;
        reconnect_requested_ = false;
        return;
    }

    reconnect_requested_ = true;

    const std::uint64_t doubled =
        current_delay_ns_ > (max_delay_ns_ / 2)
            ? max_delay_ns_
            : current_delay_ns_ * 2;

    current_delay_ns_ = std::min(doubled, max_delay_ns_);
}

/**
 * @brief Disable reconnect attempts.
 *
 * Use this for clean shutdown or fatal unrecoverable errors.
 */
void ReconnectController::disable() noexcept {
    disabled_ = true;
    reconnect_requested_ = false;
    reconnecting_ = false;
}

/**
 * @brief Reset reconnect state.
 *
 * This clears reconnect request and backoff.
 *
 * It intentionally does not reset connection_id because connection_id is a
 * historical session counter.
 */
void ReconnectController::reset() noexcept {
    reconnect_requested_ = false;
    reconnecting_ = false;
    disabled_ = false;

    reason_ = ReconnectReason::None;

    attempt_count_ = 0;
    current_delay_ns_ = initial_delay_ns_;

    last_request_ns_ = 0;
    last_attempt_ns_ = 0;
}

/**
 * @brief Return current reconnect status.
 */
ReconnectStatus ReconnectController::status() const noexcept {
    if (disabled_) {
        return ReconnectStatus::Disabled;
    }

    if (reconnecting_) {
        return ReconnectStatus::Reconnecting;
    }

    if (reconnect_requested_) {
        return ReconnectStatus::Requested;
    }

    return ReconnectStatus::Idle;
}

ReconnectReason ReconnectController::reason() const noexcept {
    return reason_;
}

bool ReconnectController::reconnect_requested() const noexcept {
    return reconnect_requested_;
}

bool ReconnectController::disabled() const noexcept {
    return disabled_;
}

std::uint64_t ReconnectController::connection_id() const noexcept {
    return connection_id_;
}

std::uint64_t ReconnectController::reconnect_count() const noexcept {
    return reconnect_count_;
}

std::uint32_t ReconnectController::attempt_count() const noexcept {
    return attempt_count_;
}

std::uint64_t ReconnectController::current_delay_ns() const noexcept {
    return current_delay_ns_;
}

std::uint64_t ReconnectController::last_request_ns() const noexcept {
    return last_request_ns_;
}

std::uint64_t ReconnectController::last_attempt_ns() const noexcept {
    return last_attempt_ns_;
}

std::uint64_t ReconnectController::last_success_ns() const noexcept {
    return last_success_ns_;
}

}  // namespace trading_engine::feed
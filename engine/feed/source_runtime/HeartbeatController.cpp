#include "feed/source_runtime/HeartbeatController.h"

namespace trading_engine::feed {

/**
 * @brief Convert milliseconds to nanoseconds.
 *
 * The feed engine uses monotonic timestamps in nanoseconds internally.
 */
std::uint64_t HeartbeatController::ms_to_ns(std::uint64_t ms) noexcept {
    return ms * 1'000'000ULL;
}

/**
 * @brief Construct heartbeat controller with millisecond config.
 *
 * Example for Polymarket Market/User:
 *
 *     HeartbeatController heartbeat(
 *         10'000,  // ping every 10 seconds
 *         5'000,   // expect pong within 5 seconds
 *         30'000   // no message for 30 seconds => stale
 *     );
 */
HeartbeatController::HeartbeatController(
    std::uint64_t ping_interval_ms,
    std::uint64_t pong_timeout_ms,
    std::uint64_t stale_timeout_ms
)
    : ping_interval_ns_(ms_to_ns(ping_interval_ms)),
      pong_timeout_ns_(ms_to_ns(pong_timeout_ms)),
      stale_timeout_ns_(ms_to_ns(stale_timeout_ms)) {}

/**
 * @brief Reset state after a new connection opens.
 *
 * We intentionally set last_ping_sent_ns_ to 0 so should_send_ping() returns
 * true immediately after connection open.
 *
 * That lets the runtime send an initial PING quickly.
 */
void HeartbeatController::on_connection_open(std::uint64_t now_ns) noexcept {
    connection_open_ = true;
    waiting_for_pong_ = false;

    last_ping_sent_ns_ = 0;
    last_pong_received_ns_ = 0;
    last_message_received_ns_ = now_ns;
}

/**
 * @brief Mark connection closed.
 *
 * Once closed, should_send_ping() returns false until the next
 * on_connection_open().
 */
void HeartbeatController::on_connection_closed() noexcept {
    connection_open_ = false;
    waiting_for_pong_ = false;
}

/**
 * @brief Decide whether caller should send PING now.
 *
 * Rules:
 *
 * 1. If connection is not open, do not send PING.
 * 2. If no PING has been sent on this connection, send immediately.
 * 3. Otherwise send when ping_interval has elapsed.
 *
 * This function does not mutate state.
 * Caller must call on_ping_sent(now_ns) after actually sending PING.
 */
bool HeartbeatController::should_send_ping(std::uint64_t now_ns) const noexcept {
    if (!connection_open_) {
        return false;
    }

    if (last_ping_sent_ns_ == 0) {
        return true;
    }

    return now_ns >= last_ping_sent_ns_ &&
           now_ns - last_ping_sent_ns_ >= ping_interval_ns_;
}

/**
 * @brief Record that a PING was sent.
 *
 * After this call, the controller expects a PONG.
 */
void HeartbeatController::on_ping_sent(std::uint64_t now_ns) noexcept {
    last_ping_sent_ns_ = now_ns;
    waiting_for_pong_ = true;
}

/**
 * @brief Record that PONG was received.
 *
 * This clears waiting_for_pong_.
 *
 * Also updates last_message_received_ns_ because PONG is still an incoming
 * message from the source.
 */
void HeartbeatController::on_pong_received(std::uint64_t now_ns) noexcept {
    last_pong_received_ns_ = now_ns;
    last_message_received_ns_ = now_ns;
    waiting_for_pong_ = false;
}

/**
 * @brief Record any inbound message.
 *
 * This should be called for every WebSocket message:
 *
 * - book
 * - price_change
 * - best_bid_ask
 * - PONG
 * - unknown messages
 *
 * It prevents false stale detection when market data is flowing even if PONG
 * messages are sparse or formatted unexpectedly.
 */
void HeartbeatController::on_message_received(std::uint64_t now_ns) noexcept {
    last_message_received_ns_ = now_ns;
}

/**
 * @brief Return current heartbeat status.
 *
 * Priority:
 *
 * 1. If no message has arrived for stale_timeout, status is Stale.
 * 2. If waiting for PONG and timeout elapsed, status is PongTimeout.
 * 3. If waiting for PONG but still inside timeout, status is WaitingPong.
 * 4. Otherwise status is Ok.
 *
 * Stale is checked before PongTimeout because silent stalls are more severe:
 * the whole source may be dead even if the socket object still exists.
 */
HeartbeatStatus HeartbeatController::status(std::uint64_t now_ns) const noexcept {
    if (!connection_open_) {
        return HeartbeatStatus::Stale;
    }

    if (last_message_received_ns_ != 0 &&
        now_ns >= last_message_received_ns_ &&
        now_ns - last_message_received_ns_ >= stale_timeout_ns_) {
        return HeartbeatStatus::Stale;
    }

    if (waiting_for_pong_) {
        if (now_ns >= last_ping_sent_ns_ &&
            now_ns - last_ping_sent_ns_ >= pong_timeout_ns_) {
            return HeartbeatStatus::PongTimeout;
        }

        return HeartbeatStatus::WaitingPong;
    }

    return HeartbeatStatus::Ok;
}

/**
 * @brief Return true if heartbeat state is bad enough to require action.
 *
 * Usually the runtime should reconnect if this returns true.
 */
bool HeartbeatController::unhealthy(std::uint64_t now_ns) const noexcept {
    const auto s = status(now_ns);
    return s == HeartbeatStatus::PongTimeout ||
           s == HeartbeatStatus::Stale;
}

bool HeartbeatController::waiting_for_pong() const noexcept {
    return waiting_for_pong_;
}

std::uint64_t HeartbeatController::last_ping_sent_ns() const noexcept {
    return last_ping_sent_ns_;
}

std::uint64_t HeartbeatController::last_pong_received_ns() const noexcept {
    return last_pong_received_ns_;
}

std::uint64_t HeartbeatController::last_message_received_ns() const noexcept {
    return last_message_received_ns_;
}

std::uint64_t HeartbeatController::ping_interval_ns() const noexcept {
    return ping_interval_ns_;
}

std::uint64_t HeartbeatController::pong_timeout_ns() const noexcept {
    return pong_timeout_ns_;
}

std::uint64_t HeartbeatController::stale_timeout_ns() const noexcept {
    return stale_timeout_ns_;
}

}  // namespace trading_engine::feed
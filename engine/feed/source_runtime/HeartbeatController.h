#pragma once

#include <cstdint>

namespace trading_engine::feed {

/**
 * @brief Heartbeat health state for one live source connection.
 *
 * This status is about connection liveness only.
 * It does not say whether market data state is correct.
 */
enum class HeartbeatStatus {
    /**
     * @brief Connection heartbeat appears healthy.
     */
    Ok = 0,

    /**
     * @brief A PING has been sent and the controller is waiting for PONG.
     */
    WaitingPong,

    /**
     * @brief PONG did not arrive within timeout.
     */
    PongTimeout,

    /**
     * @brief No message has been received for too long.
     *
     * This catches silent stalls where the TCP/WebSocket connection may still
     * appear open, but the feed has stopped producing data.
     */
    Stale
};

/**
 * @brief Tracks heartbeat timing for a WebSocket source.
 *
 * For Polymarket Market/User channels, the client sends PING periodically
 * and expects PONG from the server.
 *
 * This class intentionally does not send network messages itself.
 * The outer runtime should do:
 *
 *     if (heartbeat.should_send_ping(now_ns)) {
 *         websocket.send("PING");
 *         heartbeat.on_ping_sent(now_ns);
 *     }
 *
 * And when receiving a message:
 *
 *     heartbeat.on_message_received(now_ns);
 *
 *     if (payload == "PONG" || payload == "pong") {
 *         heartbeat.on_pong_received(now_ns);
 *     }
 *
 * Design rule:
 *
 * All methods accept now_ns from the caller instead of calling clock functions
 * internally. This makes the controller deterministic and easy to unit test.
 */
class HeartbeatController {
public:
    /**
     * @brief Construct heartbeat controller.
     *
     * @param ping_interval_ms How often the client should send PING.
     * @param pong_timeout_ms Maximum allowed delay between PING and PONG.
     * @param stale_timeout_ms Maximum allowed delay since any message.
     */
    HeartbeatController(
        std::uint64_t ping_interval_ms,
        std::uint64_t pong_timeout_ms,
        std::uint64_t stale_timeout_ms
    );

    /**
     * @brief Reset heartbeat state after a new connection opens.
     *
     * Call this after WebSocket connection succeeds.
     */
    void on_connection_open(std::uint64_t now_ns) noexcept;

    /**
     * @brief Mark heartbeat as disconnected/closed.
     *
     * Call this after WebSocket close or before reconnect.
     */
    void on_connection_closed() noexcept;

    /**
     * @brief Return whether a PING should be sent now.
     *
     * For client-driven heartbeat:
     *
     * - send immediately after connection open;
     * - then send every ping_interval.
     */
    [[nodiscard]] bool should_send_ping(std::uint64_t now_ns) const noexcept;

    /**
     * @brief Record that PING was sent.
     */
    void on_ping_sent(std::uint64_t now_ns) noexcept;

    /**
     * @brief Record that PONG was received.
     */
    void on_pong_received(std::uint64_t now_ns) noexcept;

    /**
     * @brief Record that any message was received.
     *
     * This should be called for all incoming messages, not only PONG.
     *
     * It lets the controller detect silent feed stalls.
     */
    void on_message_received(std::uint64_t now_ns) noexcept;

    /**
     * @brief Return current heartbeat status.
     */
    [[nodiscard]] HeartbeatStatus status(std::uint64_t now_ns) const noexcept;

    /**
     * @brief Convenience check for bad heartbeat state.
     */
    [[nodiscard]] bool unhealthy(std::uint64_t now_ns) const noexcept;

    /**
     * @brief Return true if a PING has been sent and PONG is still missing.
     */
    [[nodiscard]] bool waiting_for_pong() const noexcept;

    [[nodiscard]] std::uint64_t last_ping_sent_ns() const noexcept;
    [[nodiscard]] std::uint64_t last_pong_received_ns() const noexcept;
    [[nodiscard]] std::uint64_t last_message_received_ns() const noexcept;

    [[nodiscard]] std::uint64_t ping_interval_ns() const noexcept;
    [[nodiscard]] std::uint64_t pong_timeout_ns() const noexcept;
    [[nodiscard]] std::uint64_t stale_timeout_ns() const noexcept;

private:
    static std::uint64_t ms_to_ns(std::uint64_t ms) noexcept;

    std::uint64_t ping_interval_ns_{0};
    std::uint64_t pong_timeout_ns_{0};
    std::uint64_t stale_timeout_ns_{0};

    bool connection_open_{false};
    bool waiting_for_pong_{false};

    std::uint64_t last_ping_sent_ns_{0};
    std::uint64_t last_pong_received_ns_{0};
    std::uint64_t last_message_received_ns_{0};
};

}  // namespace trading_engine::feed
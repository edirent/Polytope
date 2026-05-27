#pragma once

#include <cstdint>

namespace trading_engine::feed {

/**
 * @brief Current reconnect controller state.
 *
 * This describes the reconnect state machine, not the WebSocket state itself.
 */
enum class ReconnectStatus {
    /**
     * @brief No reconnect is currently needed.
     */
    Idle = 0,

    /**
     * @brief Reconnect has been requested, but no attempt is running now.
     */
    Requested,

    /**
     * @brief A reconnect attempt is currently in progress.
     */
    Reconnecting,

    /**
     * @brief Reconnect is disabled because max attempt limit was reached.
     */
    Disabled
};

/**
 * @brief Reason why reconnect was requested.
 *
 * This is useful for health output and debugging.
 */
enum class ReconnectReason {
    None = 0,

    /**
     * @brief WebSocket connection closed normally or unexpectedly.
     */
    ConnectionClosed,

    /**
     * @brief Transport-level error from WebSocketClient.
     */
    TransportError,

    /**
     * @brief HeartbeatController detected PONG timeout.
     */
    HeartbeatTimeout,

    /**
     * @brief No messages arrived for stale timeout.
     */
    StaleFeed,

    /**
     * @brief Manual reconnect request.
     */
    Manual
};

/**
 * @brief Pure reconnect decision controller.
 *
 * This class decides when reconnect should happen.
 *
 * It does not own WebSocketClient.
 * It does not call connect().
 * It does not resubscribe.
 *
 * The outer runtime should use it like this:
 *
 *     if (reconnect.should_reconnect(now_ns)) {
 *         reconnect.on_reconnect_attempt(now_ns);
 *
 *         try {
 *             client.connect();
 *             reconnect.on_reconnect_success(now_ns);
 *         } catch (...) {
 *             reconnect.on_reconnect_failure(now_ns);
 *         }
 *     }
 *
 * Design rule:
 *
 * All timing uses caller-provided monotonic nanoseconds. This keeps the class
 * deterministic and easy to unit test.
 */
class ReconnectController {
public:
    /**
     * @brief Construct reconnect controller.
     *
     * @param initial_delay_ms Delay before first/next reconnect attempt.
     * @param max_delay_ms Maximum exponential backoff delay.
     * @param max_attempts Maximum attempts before disabling reconnect.
     *                     0 means unlimited attempts.
     */
    ReconnectController(
        std::uint64_t initial_delay_ms,
        std::uint64_t max_delay_ms,
        std::uint32_t max_attempts = 0
    );

    /**
     * @brief Record that a connection opened successfully.
     *
     * This should be called after both initial connect and reconnect success.
     *
     * It increments connection_id.
     */
    void on_connection_open(std::uint64_t now_ns) noexcept;

    /**
     * @brief Request reconnect.
     *
     * This does not reconnect immediately.
     * It only marks reconnect as needed.
     */
    void request_reconnect(
        std::uint64_t now_ns,
        ReconnectReason reason
    ) noexcept;

    /**
     * @brief Return whether runtime should attempt reconnect now.
     */
    [[nodiscard]] bool should_reconnect(std::uint64_t now_ns) const noexcept;

    /**
     * @brief Mark that a reconnect attempt is starting now.
     */
    void on_reconnect_attempt(std::uint64_t now_ns) noexcept;

    /**
     * @brief Mark reconnect success.
     *
     * This resets backoff and clears reconnect request.
     */
    void on_reconnect_success(std::uint64_t now_ns) noexcept;

    /**
     * @brief Mark reconnect failure.
     *
     * This increases attempt count and exponential backoff delay.
     */
    void on_reconnect_failure(std::uint64_t now_ns) noexcept;

    /**
     * @brief Disable reconnect attempts.
     *
     * Useful for clean shutdown or fatal errors.
     */
    void disable() noexcept;

    /**
     * @brief Reset controller to idle state.
     *
     * Does not reset connection_id.
     */
    void reset() noexcept;

    [[nodiscard]] ReconnectStatus status() const noexcept;
    [[nodiscard]] ReconnectReason reason() const noexcept;

    [[nodiscard]] bool reconnect_requested() const noexcept;
    [[nodiscard]] bool disabled() const noexcept;

    [[nodiscard]] std::uint64_t connection_id() const noexcept;
    [[nodiscard]] std::uint64_t reconnect_count() const noexcept;
    [[nodiscard]] std::uint32_t attempt_count() const noexcept;

    [[nodiscard]] std::uint64_t current_delay_ns() const noexcept;
    [[nodiscard]] std::uint64_t last_request_ns() const noexcept;
    [[nodiscard]] std::uint64_t last_attempt_ns() const noexcept;
    [[nodiscard]] std::uint64_t last_success_ns() const noexcept;

private:
    static std::uint64_t ms_to_ns(std::uint64_t ms) noexcept;

    std::uint64_t initial_delay_ns_{0};
    std::uint64_t max_delay_ns_{0};
    std::uint64_t current_delay_ns_{0};

    std::uint32_t max_attempts_{0};
    std::uint32_t attempt_count_{0};

    bool reconnect_requested_{false};
    bool reconnecting_{false};
    bool disabled_{false};

    ReconnectReason reason_{ReconnectReason::None};

    std::uint64_t connection_id_{0};
    std::uint64_t reconnect_count_{0};

    std::uint64_t last_request_ns_{0};
    std::uint64_t last_attempt_ns_{0};
    std::uint64_t last_success_ns_{0};
};

}  // namespace trading_engine::feed
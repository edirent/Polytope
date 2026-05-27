#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace trading_engine::feed {

/**
 * @brief Thin WebSocket transport wrapper.
 *
 * This class only handles transport-level behavior:
 *
 * - connect
 * - send
 * - receive
 * - close
 * - callbacks
 *
 * It should NOT know about:
 *
 * - Polymarket asset ids
 * - RawPacket
 * - order book state
 * - recovery policy
 */
class WebSocketClient {
public:
    using OpenCallback = std::function<void()>;
    using MessageCallback = std::function<void(const std::string&)>;
    using CloseCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit WebSocketClient(std::string endpoint = {});
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    WebSocketClient(WebSocketClient&&) noexcept;
    WebSocketClient& operator=(WebSocketClient&&) noexcept;

    void set_endpoint(std::string endpoint);

    void set_on_open(OpenCallback cb);
    void set_on_message(MessageCallback cb);
    void set_on_close(CloseCallback cb);
    void set_on_error(ErrorCallback cb);

    /**
     * @brief Establish WebSocket connection.
     *
     * For wss:// endpoints this performs:
     *
     * - TCP connect
     * - TLS handshake
     * - WebSocket handshake
     */
    void connect();

    /**
     * @brief Blocking receive loop.
     *
     * This should usually run in its own thread.
     */
    void run();

    /**
     * @brief Send text message.
     *
     * Used for:
     *
     * - subscription
     * - PING
     */
    void send(const std::string& message);

    /**
     * @brief Close connection.
     */
    void disconnect() noexcept;

    [[nodiscard]] const std::string& endpoint() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace trading_engine::feed
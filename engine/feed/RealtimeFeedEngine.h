#pragma once

#include "feed/raw_ingest/RawLogWriter.h"
#include "feed/raw_ingest/RawPacket.h"
#include "feed/source_runtime/HeartbeatController.h"
#include "feed/source_runtime/ReconnectController.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading_engine::feed {

class WebSocketClient;

/**
 * @brief Runtime configuration for the first live raw-capture engine.
 *
 * This config is intentionally narrow.
 *
 * The first engine does not parse market data.
 * It only connects to a source, subscribes, receives raw messages, and writes
 * those messages to raw logs.
 */
struct RealtimeFeedConfig {
    /**
     * @brief WebSocket endpoint.
     *
     * For Polymarket Market channel:
     *
     *     wss://ws-subscriptions-clob.polymarket.com/ws/market
     */
    std::string endpoint{
        "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    };

    /**
     * @brief Logical source id used in RawPacketHeader.
     */
    SourceId source_id{SourceId::PolymarketMarket};

    /**
     * @brief Asset ids used by Polymarket Market subscription.
     *
     * Market channel uses asset ids, not condition ids.
     */
    std::vector<std::string> asset_ids;

    /**
     * @brief Whether to enable Polymarket custom feature events.
     *
     * Needed for events such as best_bid_ask, new_market, market_resolved.
     */
    bool custom_feature_enabled{true};

    /**
     * @brief Binary raw log output path.
     *
     * This file is used by RawLogReader / ReplayRunner.
     */
    std::string raw_log_path{"logs/market.raw"};

    /**
     * @brief Human-readable JSONL sample output path.
     *
     * This is not the authoritative replay format.
     * It is only for schema inspection and debugging.
     */
    std::string jsonl_log_path{"logs/market.jsonl"};

    /**
     * @brief Client-driven heartbeat interval.
     *
     * Polymarket Market/User channels expect client PING roughly every 10s.
     */
    std::uint64_t ping_interval_ms{10'000};

    /**
     * @brief Maximum time allowed after PING before PONG is considered missing.
     */
    std::uint64_t pong_timeout_ms{5'000};

    /**
     * @brief Maximum time allowed without any inbound message.
     *
     * This detects silent stalls.
     */
    std::uint64_t stale_timeout_ms{30'000};

    /**
     * @brief Initial reconnect delay.
     */
    std::uint64_t reconnect_initial_delay_ms{500};

    /**
     * @brief Maximum reconnect backoff delay.
     */
    std::uint64_t reconnect_max_delay_ms{5'000};

    /**
     * @brief Maximum reconnect attempts.
     *
     * 0 means unlimited.
     */
    std::uint32_t reconnect_max_attempts{0};

    /**
     * @brief Flush raw/jsonl logs every N packets.
     *
     * This is not fsync. It only flushes stream buffers.
     */
    std::uint64_t flush_every_n_packets{100};

    /**
     * @brief Main runtime polling interval.
     *
     * This controls how often heartbeat/reconnect checks run.
     */
    std::uint64_t runtime_tick_ms{100};
};

/**
 * @brief Runtime statistics for live raw capture.
 *
 * These counters are intentionally transport/raw-capture oriented.
 * They do not include decoded event counts because decoding is not part of
 * this phase.
 */
struct RealtimeFeedStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};

    std::uint64_t packets_captured{0};
    std::uint64_t heartbeat_packets_captured{0};

    std::uint64_t ping_sent_count{0};
    std::uint64_t pong_received_count{0};

    std::uint64_t transport_error_count{0};
    std::uint64_t raw_write_error_count{0};

    std::uint64_t reconnect_count{0};
    std::uint64_t connection_id{0};

    bool running{false};
};

/**
 * @brief First-stage live feed engine.
 *
 * This class glues together:
 *
 * - WebSocketClient
 * - HeartbeatController
 * - ReconnectController
 * - RawLogWriter
 *
 * Its only job is to turn a live WebSocket source into raw captured data:
 *
 *     Polymarket WebSocket
 *         -> WebSocketClient
 *         -> raw payload
 *         -> make_raw_packet()
 *         -> RawLogWriter
 *         -> market.raw
 *
 * It also writes market.jsonl for human-readable schema inspection.
 *
 * Design boundary:
 *
 * RealtimeFeedEngine does NOT decode JSON.
 * RealtimeFeedEngine does NOT update order books.
 * RealtimeFeedEngine does NOT produce trading signals.
 */
class RealtimeFeedEngine {
public:
    explicit RealtimeFeedEngine(RealtimeFeedConfig config);
    ~RealtimeFeedEngine();

    RealtimeFeedEngine(const RealtimeFeedEngine&) = delete;
    RealtimeFeedEngine& operator=(const RealtimeFeedEngine&) = delete;

    /**
     * @brief Blocking runtime loop.
     *
     * This starts the connection, then continuously checks:
     *
     * - heartbeat ping timing,
     * - heartbeat stale/timeout,
     * - reconnect timing.
     *
     * Call stop() from another thread to terminate.
     */
    void run();

    /**
     * @brief Request runtime shutdown.
     *
     * This disconnects the WebSocket and joins the receive thread.
     */
    void stop() noexcept;

    /**
     * @brief Return a snapshot of runtime counters.
     */
    [[nodiscard]] RealtimeFeedStats stats() const noexcept;

    /**
     * @brief Return current config.
     */
    [[nodiscard]] const RealtimeFeedConfig& config() const noexcept;

private:
    /**
     * @brief Create a new WebSocketClient and attach callbacks.
     *
     * We recreate the client after reconnect because many WebSocket stream
     * implementations are not safely reusable after close/error.
     */
    void create_client();

    /**
     * @brief Start connection and receive thread.
     *
     * @param is_reconnect_attempt true if called during reconnect flow.
     *
     * @return true if connection succeeded; false otherwise.
     */
    bool start_connection(bool is_reconnect_attempt);

    /**
     * @brief Start blocking WebSocket read loop in a background thread.
     */
    void start_receive_thread();

    /**
     * @brief Disconnect current client and join receive thread.
     */
    void stop_client_thread() noexcept;

    /**
     * @brief WebSocket open callback.
     *
     * Sends subscription and opens heartbeat state.
     */
    void handle_open();

    /**
     * @brief WebSocket message callback.
     *
     * Captures raw payload into market.raw and market.jsonl.
     */
    void handle_message(const std::string& payload);

    /**
     * @brief WebSocket close callback.
     *
     * Requests reconnect unless shutdown is in progress.
     */
    void handle_close();

    /**
     * @brief WebSocket error callback.
     *
     * Records error and requests reconnect.
     */
    void handle_error(const std::string& error);

    /**
     * @brief Send PING if heartbeat says it is time.
     */
    void maybe_send_ping(std::uint64_t now_ns);

    /**
     * @brief Check heartbeat status and request reconnect if unhealthy.
     */
    void maybe_request_reconnect_from_heartbeat(std::uint64_t now_ns);

    /**
     * @brief Attempt reconnect if ReconnectController says it is time.
     */
    void maybe_reconnect(std::uint64_t now_ns);

    /**
     * @brief Send Polymarket market subscription.
     */
    void send_subscription();

    /**
     * @brief Build Polymarket Market channel subscription JSON.
     */
    [[nodiscard]] std::string make_subscription_message() const;

    /**
     * @brief Return true if payload is a PONG heartbeat response.
     */
    [[nodiscard]] static bool is_pong_payload(const std::string& payload);

    /**
     * @brief Escape a string for minimal JSON generation.
     */
    [[nodiscard]] static std::string json_escape(const std::string& value);

    /**
     * @brief Sleep for runtime_tick_ms.
     */
    void sleep_runtime_tick() const;

private:
    RealtimeFeedConfig config_;

    RawLogWriter raw_writer_;
    std::ofstream jsonl_out_;

    HeartbeatController heartbeat_;
    ReconnectController reconnect_;

    std::unique_ptr<WebSocketClient> client_;
    std::thread receive_thread_;

    mutable std::mutex log_mutex_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> fatal_error_{false};

    std::atomic<std::uint64_t> next_packet_id_{1};

    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};

    std::atomic<std::uint64_t> packets_captured_{0};
    std::atomic<std::uint64_t> heartbeat_packets_captured_{0};

    std::atomic<std::uint64_t> ping_sent_count_{0};
    std::atomic<std::uint64_t> pong_received_count_{0};

    std::atomic<std::uint64_t> transport_error_count_{0};
    std::atomic<std::uint64_t> raw_write_error_count_{0};
};

}  // namespace trading_engine::feed
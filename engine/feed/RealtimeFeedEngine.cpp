#include "feed/RealtimeFeedEngine.h"

#include "feed/source_runtime/WebSocketClient.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace trading_engine::feed {

namespace {

/**
 * @brief Convert milliseconds to std::chrono duration.
 */
std::chrono::milliseconds ms_duration(std::uint64_t ms) {
    return std::chrono::milliseconds(ms);
}

/**
 * @brief Return reconnect reason based on heartbeat status.
 */
ReconnectReason reconnect_reason_from_heartbeat(HeartbeatStatus status) {
    if (status == HeartbeatStatus::PongTimeout) {
        return ReconnectReason::HeartbeatTimeout;
    }

    if (status == HeartbeatStatus::Stale) {
        return ReconnectReason::StaleFeed;
    }

    return ReconnectReason::None;
}

}  // namespace

/**
 * @brief Construct live raw-capture engine.
 *
 * Important:
 *
 * RawLogWriter opens config.raw_log_path immediately.
 * jsonl_out_ opens config.jsonl_log_path immediately.
 *
 * The caller must ensure parent directories such as logs/ already exist.
 */
RealtimeFeedEngine::RealtimeFeedEngine(RealtimeFeedConfig config)
    : config_(std::move(config)),
      raw_writer_(config_.raw_log_path),
      heartbeat_(
          config_.ping_interval_ms,
          config_.pong_timeout_ms,
          config_.stale_timeout_ms
      ),
      reconnect_(
          config_.reconnect_initial_delay_ms,
          config_.reconnect_max_delay_ms,
          config_.reconnect_max_attempts
      ) {
    jsonl_out_.open(
        config_.jsonl_log_path,
        std::ios::out | std::ios::app
    );

    if (!jsonl_out_.is_open()) {
        throw std::runtime_error(
            "RealtimeFeedEngine failed to open jsonl log: " +
            config_.jsonl_log_path
        );
    }
}

/**
 * @brief Destructor requests clean shutdown.
 */
RealtimeFeedEngine::~RealtimeFeedEngine() {
    stop();
}

/**
 * @brief Main blocking runtime loop.
 *
 * Runtime structure:
 *
 * 1. Attempt initial connection.
 * 2. Start WebSocket receive thread.
 * 3. Periodically:
 *      - send heartbeat PING if due;
 *      - detect heartbeat unhealthy state;
 *      - perform reconnect if due.
 *
 * This loop does not decode messages.
 * Message decoding belongs to the next phase.
 */
void RealtimeFeedEngine::run() {
    bool expected = false;

    if (!running_.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("RealtimeFeedEngine is already running");
    }

    stop_requested_.store(false);
    fatal_error_.store(false);

    // Initial connection.
    //
    // If this fails, request reconnect and let the main loop retry with backoff.
    if (!start_connection(false)) {
        reconnect_.request_reconnect(
            now_monotonic_ns(),
            ReconnectReason::TransportError
        );
    }

    while (!stop_requested_.load() && !fatal_error_.load()) {
        const auto now = now_monotonic_ns();

        maybe_send_ping(now);
        maybe_request_reconnect_from_heartbeat(now);
        maybe_reconnect(now);

        sleep_runtime_tick();
    }

    stop();
}

/**
 * @brief Stop engine and close current connection.
 *
 * This method is noexcept because it may be called from destructor.
 */
void RealtimeFeedEngine::stop() noexcept {
    stop_requested_.store(true);
    running_.store(false);

    try {
        stop_client_thread();

        {
            std::lock_guard<std::mutex> lock(log_mutex_);

            try {
                raw_writer_.flush();
            } catch (...) {
                // Destructors/shutdown should not throw.
            }

            if (jsonl_out_.is_open()) {
                jsonl_out_.flush();
            }
        }
    } catch (...) {
        // noexcept: suppress all shutdown exceptions.
    }
}

/**
 * @brief Return runtime statistics snapshot.
 */
RealtimeFeedStats RealtimeFeedEngine::stats() const noexcept {
    RealtimeFeedStats s;

    s.messages_received = messages_received_.load();
    s.bytes_received = bytes_received_.load();

    s.packets_captured = packets_captured_.load();
    s.heartbeat_packets_captured = heartbeat_packets_captured_.load();

    s.ping_sent_count = ping_sent_count_.load();
    s.pong_received_count = pong_received_count_.load();

    s.transport_error_count = transport_error_count_.load();
    s.raw_write_error_count = raw_write_error_count_.load();

    s.reconnect_count = reconnect_.reconnect_count();
    s.connection_id = reconnect_.connection_id();

    s.running = running_.load();

    return s;
}

const RealtimeFeedConfig& RealtimeFeedEngine::config() const noexcept {
    return config_;
}

/**
 * @brief Create WebSocketClient and bind callbacks.
 *
 * The WebSocketClient is transport-only.
 * The callbacks here glue transport events into runtime behavior.
 */
void RealtimeFeedEngine::create_client() {
    auto client = std::make_unique<WebSocketClient>(config_.endpoint);

    client->set_on_open([this]() {
        handle_open();
    });

    client->set_on_message([this](const std::string& payload) {
        handle_message(payload);
    });

    client->set_on_close([this]() {
        handle_close();
    });

    client->set_on_error([this](const std::string& error) {
        handle_error(error);
    });

    client_ = std::move(client);
}

/**
 * @brief Create client, connect, update connection state, and start read loop.
 *
 * If connection succeeds:
 *
 * - WebSocketClient::connect() will invoke handle_open().
 * - Then this function updates reconnect/connection state.
 * - Then it starts the blocking receive loop in a background thread.
 *
 * Note:
 *
 * handle_open() sends subscription before connection_id is incremented here.
 * That is fine because packet capture begins only after run() starts reading.
 */
bool RealtimeFeedEngine::start_connection(bool is_reconnect_attempt) {
    try {
        create_client();

        client_->connect();

        const auto now = now_monotonic_ns();

        if (is_reconnect_attempt) {
            reconnect_.on_reconnect_success(now);
        } else {
            reconnect_.on_connection_open(now);
        }

        start_receive_thread();

        return true;
    } catch (const std::exception& e) {
        ++transport_error_count_;

        std::cerr << "[feed] connect failed: " << e.what() << '\n';

        if (client_) {
            client_->disconnect();
            client_.reset();
        }

        return false;
    }
}

/**
 * @brief Start WebSocket receive loop on background thread.
 *
 * WebSocketClient::run() is blocking, so it must not run on the engine loop
 * thread.
 */
void RealtimeFeedEngine::start_receive_thread() {
    if (!client_) {
        throw std::runtime_error("Cannot start receive thread without client");
    }

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    WebSocketClient* client_ptr = client_.get();

    receive_thread_ = std::thread([client_ptr]() {
        client_ptr->run();
    });
}

/**
 * @brief Disconnect client and join receive thread.
 */
void RealtimeFeedEngine::stop_client_thread() noexcept {
    try {
        if (client_) {
            client_->disconnect();
        }

        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }

        client_.reset();
    } catch (...) {
        // shutdown path should not throw
    }
}

/**
 * @brief Handle successful WebSocket open.
 *
 * This is called by WebSocketClient during connect().
 *
 * It:
 *
 * - initializes heartbeat state;
 * - sends Polymarket subscription.
 */
void RealtimeFeedEngine::handle_open() {
    const auto now = now_monotonic_ns();

    heartbeat_.on_connection_open(now);

    send_subscription();

    std::cerr << "[feed] websocket opened and subscription sent\n";
}

/**
 * @brief Handle every inbound WebSocket payload.
 *
 * This function is the raw-capture hot path.
 *
 * It does not decode JSON.
 * It treats payload as opaque raw source data.
 */
void RealtimeFeedEngine::handle_message(const std::string& payload) {
    const auto now = now_monotonic_ns();

    heartbeat_.on_message_received(now);

    ++messages_received_;
    bytes_received_ += static_cast<std::uint64_t>(payload.size());

    std::uint32_t flags = PacketNone;

    if (is_pong_payload(payload)) {
        heartbeat_.on_pong_received(now);
        ++pong_received_count_;
        flags |= PacketHeartbeat;
    }

    try {
        const auto packet_id = next_packet_id_.fetch_add(1);

        RawPacket packet = make_raw_packet(
            config_.source_id,
            reconnect_.connection_id(),
            packet_id,
            payload,
            Codec::None,
            flags
        );

        {
            std::lock_guard<std::mutex> lock(log_mutex_);

            raw_writer_.write_packet(packet);

            // JSONL is a debugging/schema-inspection side channel.
            // It is not the authoritative replay format.
            if (jsonl_out_.is_open()) {
                jsonl_out_ << payload << '\n';
            }

            ++packets_captured_;

            if (flags & PacketHeartbeat) {
                ++heartbeat_packets_captured_;
            }

            const auto captured = packets_captured_.load();

            if (config_.flush_every_n_packets > 0 &&
                captured % config_.flush_every_n_packets == 0) {
                raw_writer_.flush();

                if (jsonl_out_.is_open()) {
                    jsonl_out_.flush();
                }
            }
        }
    } catch (const std::exception& e) {
        ++raw_write_error_count_;

        std::cerr << "[feed] raw capture failed: " << e.what() << '\n';

        // A raw logging failure is severe. Continuing live capture without raw
        // logs destroys replayability, so stop the engine.
        fatal_error_.store(true);
    }
}

/**
 * @brief Handle WebSocket close.
 *
 * If shutdown is not in progress, request reconnect.
 */
void RealtimeFeedEngine::handle_close() {
    heartbeat_.on_connection_closed();

    if (stop_requested_.load()) {
        return;
    }

    reconnect_.request_reconnect(
        now_monotonic_ns(),
        ReconnectReason::ConnectionClosed
    );

    std::cerr << "[feed] websocket closed; reconnect requested\n";
}

/**
 * @brief Handle WebSocket transport error.
 */
void RealtimeFeedEngine::handle_error(const std::string& error) {
    ++transport_error_count_;

    if (!stop_requested_.load()) {
        reconnect_.request_reconnect(
            now_monotonic_ns(),
            ReconnectReason::TransportError
        );
    }

    std::cerr << "[feed] websocket error: " << error << '\n';
}

/**
 * @brief Send heartbeat PING if due.
 *
 * HeartbeatController only decides timing.
 * WebSocketClient actually sends the string.
 */
void RealtimeFeedEngine::maybe_send_ping(std::uint64_t now_ns) {
    if (!client_ || !client_->connected()) {
        return;
    }

    if (!heartbeat_.should_send_ping(now_ns)) {
        return;
    }

    try {
        client_->send("PING");
        heartbeat_.on_ping_sent(now_ns);
        ++ping_sent_count_;
    } catch (const std::exception& e) {
        ++transport_error_count_;

        std::cerr << "[feed] failed to send PING: " << e.what() << '\n';

        reconnect_.request_reconnect(
            now_ns,
            ReconnectReason::TransportError
        );
    }
}

/**
 * @brief Request reconnect when heartbeat becomes unhealthy.
 */
void RealtimeFeedEngine::maybe_request_reconnect_from_heartbeat(
    std::uint64_t now_ns
) {
    if (!heartbeat_.unhealthy(now_ns)) {
        return;
    }

    const auto status = heartbeat_.status(now_ns);
    const auto reason = reconnect_reason_from_heartbeat(status);

    if (reason == ReconnectReason::None) {
        return;
    }

    reconnect_.request_reconnect(now_ns, reason);
}

/**
 * @brief Execute reconnect if ReconnectController says it is time.
 */
void RealtimeFeedEngine::maybe_reconnect(std::uint64_t now_ns) {
    if (!reconnect_.should_reconnect(now_ns)) {
        return;
    }

    std::cerr << "[feed] attempting reconnect\n";

    stop_client_thread();

    reconnect_.on_reconnect_attempt(now_ns);

    if (!start_connection(true)) {
        reconnect_.on_reconnect_failure(now_monotonic_ns());
    }
}

/**
 * @brief Send subscription after WebSocket open.
 */
void RealtimeFeedEngine::send_subscription() {
    if (!client_ || !client_->connected()) {
        throw std::runtime_error("Cannot subscribe: websocket not connected");
    }

    client_->send(make_subscription_message());
}

/**
 * @brief Build Polymarket Market subscription JSON.
 *
 * This avoids pulling in a JSON library for the raw-capture spike.
 *
 * Later, config/serialization can be cleaned up.
 */
std::string RealtimeFeedEngine::make_subscription_message() const {
    std::string msg;

    msg += R"({"assets_ids":[)";

    for (std::size_t i = 0; i < config_.asset_ids.size(); ++i) {
        if (i > 0) {
            msg += ",";
        }

        msg += "\"";
        msg += json_escape(config_.asset_ids[i]);
        msg += "\"";
    }

    msg += R"(],"type":"market","custom_feature_enabled":)";
    msg += config_.custom_feature_enabled ? "true" : "false";
    msg += "}";

    return msg;
}

/**
 * @brief Detect simple PONG payloads.
 *
 * Polymarket documentation says client sends PING and server returns PONG.
 *
 * We keep detection deliberately conservative here.
 */
bool RealtimeFeedEngine::is_pong_payload(const std::string& payload) {
    return payload == "PONG" ||
           payload == "pong" ||
           payload == "\"PONG\"" ||
           payload == "\"pong\"";
}

/**
 * @brief Minimal JSON string escaping.
 *
 * This is enough for asset ids and endpoint-style config strings.
 */
std::string RealtimeFeedEngine::json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());

    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }

    return out;
}

/**
 * @brief Sleep between runtime health/reconnect ticks.
 */
void RealtimeFeedEngine::sleep_runtime_tick() const {
    std::this_thread::sleep_for(ms_duration(config_.runtime_tick_ms));
}

}  // namespace trading_engine::feed
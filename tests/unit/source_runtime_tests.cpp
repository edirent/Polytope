#include "feed/source_runtime/HeartbeatController.h"
#include "feed/source_runtime/ReconnectController.h"
#include "feed/source_runtime/WebSocketClient.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using trading_engine::feed::HeartbeatController;
using trading_engine::feed::HeartbeatStatus;
using trading_engine::feed::ReconnectController;
using trading_engine::feed::ReconnectReason;
using trading_engine::feed::ReconnectStatus;
using trading_engine::feed::WebSocketClient;

namespace beast = boost::beast;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using ServerWebSocket = websocket::stream<tcp::socket>;

constexpr std::string_view kSubscription =
    R"({"assets_ids":["A"],"type":"market","custom_feature_enabled":true})";

std::uint64_t ms(std::uint64_t value) {
    return value * 1'000'000ULL;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& message) {
    if (!value) {
        fail(message);
    }
}

void expect_false(bool value, const std::string& message) {
    if (value) {
        fail(message);
    }
}

template <typename T, typename U>
void expect_equal(const T& actual, const U& expected, const std::string& field) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

template <typename Fn>
void expect_throws(Fn&& fn, const std::string& message) {
    try {
        fn();
    } catch (...) {
        return;
    }

    fail(message);
}

template <typename Predicate>
void wait_until(Predicate&& predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return;
        }

        std::this_thread::sleep_for(5ms);
    }

    fail(message);
}

std::string read_text(ServerWebSocket& ws) {
    beast::flat_buffer buffer;
    ws.read(buffer);
    return beast::buffers_to_string(buffer.data());
}

void close_normal(ServerWebSocket& ws) {
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);

    if (ec && ec != websocket::error::closed) {
        throw beast::system_error(ec);
    }
}

void close_transport(ServerWebSocket& ws) {
    beast::error_code ec;
    beast::get_lowest_layer(ws).close(ec);

    if (ec) {
        throw beast::system_error(ec);
    }
}

class FakeWebSocketServer {
public:
    using SessionFn = std::function<void(ServerWebSocket&, std::size_t)>;

    explicit FakeWebSocketServer(SessionFn session)
        : FakeWebSocketServer(1, std::move(session)) {}

    FakeWebSocketServer(std::size_t session_count, SessionFn session)
        : acceptor_(
              ioc_,
              tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)
          ),
          session_count_(session_count),
          session_(std::move(session)) {}

    FakeWebSocketServer(const FakeWebSocketServer&) = delete;
    FakeWebSocketServer& operator=(const FakeWebSocketServer&) = delete;

    ~FakeWebSocketServer() {
        beast::error_code ec;
        acceptor_.close(ec);
        ioc_.stop();

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void start() {
        thread_ = std::thread([this]() { run(); });
    }

    [[nodiscard]] std::string endpoint() const {
        return "ws://127.0.0.1:" +
               std::to_string(acceptor_.local_endpoint().port()) +
               "/ws";
    }

    void wait_done() {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool finished = cv_.wait_for(lock, 2s, [this]() {
            return done_;
        });

        if (!finished) {
            fail("fake websocket server did not finish");
        }

        if (!error_.empty()) {
            fail("fake websocket server error: " + error_);
        }
    }

private:
    void run() {
        try {
            for (std::size_t i = 0; i < session_count_; ++i) {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);

                ServerWebSocket ws(std::move(socket));
                ws.accept();

                session_(ws, i);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = e.what();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }

        cv_.notify_all();
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::size_t session_count_{1};
    SessionFn session_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_{false};
    std::string error_;
};

std::uint16_t unused_local_port() {
    net::io_context ioc;
    tcp::acceptor acceptor(ioc);
    acceptor.open(tcp::v4());
    acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

void WebSocketEndpointSetting() {
    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        std::this_thread::sleep_for(100ms);
        close_normal(ws);
    });
    server.start();

    WebSocketClient client("wss://example.com/ws");
    expect_equal(client.endpoint(), std::string("wss://example.com/ws"), "endpoint");

    client.set_endpoint(server.endpoint());
    expect_equal(client.endpoint(), server.endpoint(), "updated endpoint");

    client.connect();
    expect_true(client.connected(), "client should be connected");

    expect_throws(
        [&]() { client.set_endpoint("wss://example.com/other"); },
        "set_endpoint should reject changes while connected"
    );

    client.disconnect();
    server.wait_done();
}

void WebSocketConnectCallsOnOpen() {
    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        std::this_thread::sleep_for(100ms);
        close_normal(ws);
    });
    server.start();

    int open_count = 0;
    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() { ++open_count; });

    client.connect();
    expect_true(client.connected(), "client should be connected after connect");
    expect_equal(open_count, 1, "on_open count");

    client.disconnect();
    server.wait_done();
}

void WebSocketReceivesRawMessage() {
    const std::string payload = R"({"type":"book","asset_id":"A"})";

    FakeWebSocketServer server([&](ServerWebSocket& ws, std::size_t) {
        ws.write(net::buffer(payload));
        close_normal(ws);
    });
    server.start();

    std::vector<std::string> messages;
    int close_count = 0;

    WebSocketClient client(server.endpoint());
    client.set_on_message([&](const std::string& msg) {
        messages.push_back(msg);
    });
    client.set_on_close([&]() { ++close_count; });

    client.connect();
    client.run();
    server.wait_done();

    expect_equal(messages.size(), std::size_t{1}, "message count");
    expect_equal(messages.front(), payload, "raw payload");
    expect_equal(close_count, 1, "on_close count");
    expect_false(client.connected(), "client should be disconnected after close");
}

void WebSocketSendPreservesPayload() {
    std::vector<std::string> received;
    std::mutex mutex;

    FakeWebSocketServer server([&](ServerWebSocket& ws, std::size_t) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            received.push_back(read_text(ws));
            received.push_back(read_text(ws));
        }

        close_transport(ws);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.connect();
    client.send("PING");
    client.send(std::string(kSubscription));

    server.wait_done();
    client.disconnect();

    expect_equal(received.size(), std::size_t{2}, "server received count");
    expect_equal(received[0], std::string("PING"), "PING payload");
    expect_equal(received[1], std::string(kSubscription), "subscription payload");
}

void WebSocketCloseCallback() {
    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        close_normal(ws);
    });
    server.start();

    int close_count = 0;
    int error_count = 0;

    WebSocketClient client(server.endpoint());
    client.set_on_close([&]() { ++close_count; });
    client.set_on_error([&](const std::string&) { ++error_count; });

    client.connect();
    client.run();
    server.wait_done();

    expect_false(client.connected(), "client should be disconnected after server close");
    expect_equal(close_count, 1, "on_close count");
    expect_equal(error_count, 0, "normal close should not call on_error");
}

void WebSocketErrorCallbackOnConnectFailure() {
    const auto port = unused_local_port();
    WebSocketClient client(
        "ws://127.0.0.1:" + std::to_string(port) + "/ws"
    );

    int error_count = 0;
    client.set_on_error([&](const std::string&) { ++error_count; });

    expect_throws([&]() { client.connect(); }, "connect should fail");
    expect_false(client.connected(), "failed connect should not leave client connected");
    expect_equal(error_count, 1, "on_error count");
}

void WebSocketErrorCallbackOnReadError() {
    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        beast::error_code ec;
        beast::get_lowest_layer(ws).close(ec);
    });
    server.start();

    int error_count = 0;
    int close_count = 0;

    WebSocketClient client(server.endpoint());
    client.set_on_error([&](const std::string&) { ++error_count; });
    client.set_on_close([&]() { ++close_count; });

    client.connect();
    client.run();
    server.wait_done();

    expect_false(client.connected(), "read error should disconnect client");
    expect_equal(error_count, 1, "on_error count");
    expect_equal(close_count, 1, "on_close count");
}

void HeartbeatOpenAllowsImmediatePing() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(1000);

    expect_true(heartbeat.should_send_ping(1000), "new connection should ping immediately");
}

void HeartbeatPingSentWaitingPong() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(0);
    heartbeat.on_ping_sent(1000);

    expect_equal(
        heartbeat.status(2000),
        HeartbeatStatus::WaitingPong,
        "heartbeat status"
    );
    expect_true(heartbeat.waiting_for_pong(), "should be waiting for PONG");
}

void HeartbeatPongRestoresOk() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(0);
    heartbeat.on_ping_sent(1000);
    heartbeat.on_pong_received(2000);

    expect_equal(heartbeat.status(3000), HeartbeatStatus::Ok, "heartbeat status");
    expect_false(heartbeat.waiting_for_pong(), "should not be waiting for PONG");
    expect_equal(heartbeat.last_pong_received_ns(), 2000ULL, "last_pong_received_ns");
}

void HeartbeatPongTimeout() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(0);
    heartbeat.on_ping_sent(1000);

    expect_equal(
        heartbeat.status(ms(7'000)),
        HeartbeatStatus::PongTimeout,
        "heartbeat status"
    );
    expect_true(heartbeat.unhealthy(ms(7'000)), "pong timeout should be unhealthy");
}

void HeartbeatStaleDetection() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(0);
    heartbeat.on_message_received(1000);

    expect_equal(
        heartbeat.status(32'000'000'000ULL),
        HeartbeatStatus::Stale,
        "heartbeat status"
    );
    expect_true(heartbeat.unhealthy(32'000'000'000ULL), "stale should be unhealthy");
}

void HeartbeatMarketMessageUpdatesLastMessage() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    heartbeat.on_connection_open(0);
    heartbeat.on_message_received(5000);

    expect_equal(
        heartbeat.last_message_received_ns(),
        5000ULL,
        "last_message_received_ns"
    );
}

void ReconnectRequestReconnect() {
    ReconnectController reconnect(500, 5'000, 0);
    reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);

    expect_true(reconnect.reconnect_requested(), "reconnect should be requested");
    expect_equal(reconnect.reason(), ReconnectReason::ConnectionClosed, "reason");
    expect_equal(reconnect.status(), ReconnectStatus::Requested, "status");
}

void ReconnectBackoffNotElapsed() {
    ReconnectController reconnect(500, 5'000, 0);
    reconnect.request_reconnect(ms(1'000), ReconnectReason::ConnectionClosed);

    expect_false(
        reconnect.should_reconnect(ms(1'200)),
        "reconnect should wait for backoff"
    );
}

void ReconnectBackoffElapsed() {
    ReconnectController reconnect(500, 5'000, 0);
    reconnect.request_reconnect(ms(1'000), ReconnectReason::ConnectionClosed);

    expect_true(
        reconnect.should_reconnect(ms(1'500) + 1),
        "reconnect should be allowed after backoff"
    );
}

void ReconnectAttemptState() {
    ReconnectController reconnect(500, 5'000, 0);
    reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);
    reconnect.on_reconnect_attempt(2000);

    expect_equal(reconnect.status(), ReconnectStatus::Reconnecting, "status");
    expect_equal(reconnect.attempt_count(), 1U, "attempt_count");
    expect_equal(reconnect.last_attempt_ns(), 2000ULL, "last_attempt_ns");
}

void ReconnectSuccessResetsBackoff() {
    ReconnectController reconnect(500, 5'000, 0);
    const auto initial_delay = reconnect.current_delay_ns();

    reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);
    reconnect.on_reconnect_attempt(2000);
    reconnect.on_reconnect_success(2500);

    expect_equal(reconnect.status(), ReconnectStatus::Idle, "status");
    expect_false(reconnect.reconnect_requested(), "request should be cleared");
    expect_equal(reconnect.reconnect_count(), 1ULL, "reconnect_count");
    expect_equal(reconnect.connection_id(), 1ULL, "connection_id");
    expect_equal(reconnect.attempt_count(), 0U, "attempt_count");
    expect_equal(reconnect.current_delay_ns(), initial_delay, "current_delay_ns");
}

void ReconnectFailureBackoff() {
    ReconnectController reconnect(500, 5'000, 0);
    const auto initial_delay = reconnect.current_delay_ns();

    reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);
    reconnect.on_reconnect_attempt(2000);
    reconnect.on_reconnect_failure(2500);

    expect_equal(reconnect.status(), ReconnectStatus::Requested, "status");
    expect_equal(reconnect.attempt_count(), 1U, "attempt_count");
    expect_equal(reconnect.current_delay_ns(), initial_delay * 2, "current_delay_ns");
}

void ReconnectMaxAttemptsDisables() {
    ReconnectController reconnect(500, 5'000, 3);
    reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);

    for (int i = 0; i < 3; ++i) {
        reconnect.on_reconnect_attempt(2000 + static_cast<std::uint64_t>(i));
        reconnect.on_reconnect_failure(2500 + static_cast<std::uint64_t>(i));
    }

    expect_equal(reconnect.status(), ReconnectStatus::Disabled, "status");
    expect_false(reconnect.should_reconnect(10'000), "disabled should not reconnect");
}

void ReconnectDuplicateRequestDoesNotRefreshTimer() {
    ReconnectController reconnect(500, 5'000, 0);
    reconnect.request_reconnect(ms(1'000), ReconnectReason::ConnectionClosed);
    reconnect.request_reconnect(ms(1'200), ReconnectReason::TransportError);

    expect_equal(reconnect.last_request_ns(), ms(1'000), "last_request_ns");
    expect_equal(
        reconnect.reason(),
        ReconnectReason::ConnectionClosed,
        "reason should remain first request"
    );
    expect_true(
        reconnect.should_reconnect(ms(1'500)),
        "duplicate request should not refresh backoff timer"
    );
}

void SourceRuntimeOpenSendsSubscription() {
    std::vector<std::string> received;

    FakeWebSocketServer server([&](ServerWebSocket& ws, std::size_t) {
        received.push_back(read_text(ws));
        close_transport(ws);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        client.send(std::string(kSubscription));
    });

    client.connect();
    server.wait_done();
    client.disconnect();

    expect_equal(received.size(), std::size_t{1}, "subscription count");
    expect_equal(received.front(), std::string(kSubscription), "subscription payload");
    expect_true(received.front().find("assets_ids") != std::string::npos, "asset_ids");
    expect_true(received.front().find(R"("type":"market")") != std::string::npos, "type");
    expect_true(
        received.front().find(R"("custom_feature_enabled":true)") != std::string::npos,
        "custom_feature_enabled"
    );
}

void SourceRuntimeRawMessageCapture() {
    const std::string payload = R"({"type":"book","asset_id":"A"})";
    std::vector<std::string> raw_payloads;
    HeartbeatController heartbeat(10'000, 5'000, 30'000);

    FakeWebSocketServer server([&](ServerWebSocket& ws, std::size_t) {
        ws.write(net::buffer(payload));
        close_normal(ws);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        heartbeat.on_connection_open(1);
    });
    client.set_on_message([&](const std::string& msg) {
        heartbeat.on_message_received(2);
        raw_payloads.push_back(msg);
    });

    client.connect();
    client.run();
    server.wait_done();

    expect_equal(raw_payloads.size(), std::size_t{1}, "raw payload count");
    expect_equal(raw_payloads.front(), payload, "raw payload");
    expect_equal(heartbeat.last_message_received_ns(), 2ULL, "last_message_received_ns");
}

void SourceRuntimePingPongOk() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);

    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        expect_equal(read_text(ws), std::string("PING"), "PING payload");
        ws.write(net::buffer(std::string("PONG")));
        close_normal(ws);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        heartbeat.on_connection_open(1);

        if (heartbeat.should_send_ping(1)) {
            client.send("PING");
            heartbeat.on_ping_sent(2);
        }
    });
    client.set_on_message([&](const std::string& msg) {
        heartbeat.on_message_received(3);

        if (msg == "PONG") {
            heartbeat.on_pong_received(3);
        }
    });

    client.connect();
    client.run();
    server.wait_done();

    expect_equal(heartbeat.status(4), HeartbeatStatus::Ok, "heartbeat status");
    expect_false(heartbeat.waiting_for_pong(), "should not be waiting for PONG");
}

void SourceRuntimePongTimeoutRequestsReconnect() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    ReconnectController reconnect(500, 5'000, 0);

    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        expect_equal(read_text(ws), std::string("PING"), "PING payload");
        std::this_thread::sleep_for(100ms);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        heartbeat.on_connection_open(1);
        client.send("PING");
        heartbeat.on_ping_sent(2);
    });

    client.connect();

    if (heartbeat.unhealthy(ms(6'000))) {
        reconnect.request_reconnect(ms(6'000), ReconnectReason::HeartbeatTimeout);
    }

    client.disconnect();
    server.wait_done();

    expect_equal(
        heartbeat.status(ms(6'000)),
        HeartbeatStatus::PongTimeout,
        "heartbeat status"
    );
    expect_true(reconnect.reconnect_requested(), "reconnect should be requested");
    expect_equal(reconnect.reason(), ReconnectReason::HeartbeatTimeout, "reason");
}

void SourceRuntimeStaleFeedRequestsReconnect() {
    HeartbeatController heartbeat(10'000, 5'000, 30'000);
    ReconnectController reconnect(500, 5'000, 0);

    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        std::this_thread::sleep_for(100ms);
        close_normal(ws);
    });
    server.start();

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        heartbeat.on_connection_open(1);
    });

    client.connect();

    if (heartbeat.unhealthy(ms(31'000))) {
        reconnect.request_reconnect(ms(31'000), ReconnectReason::StaleFeed);
    }

    client.disconnect();
    server.wait_done();

    expect_equal(
        heartbeat.status(ms(31'000)),
        HeartbeatStatus::Stale,
        "heartbeat status"
    );
    expect_true(reconnect.reconnect_requested(), "reconnect should be requested");
    expect_equal(reconnect.reason(), ReconnectReason::StaleFeed, "reason");
}

void SourceRuntimeServerCloseRequestsReconnect() {
    ReconnectController reconnect(500, 5'000, 0);

    FakeWebSocketServer server([](ServerWebSocket& ws, std::size_t) {
        close_normal(ws);
    });
    server.start();

    int close_count = 0;
    WebSocketClient client(server.endpoint());
    client.set_on_close([&]() {
        ++close_count;
        reconnect.request_reconnect(1000, ReconnectReason::ConnectionClosed);
    });

    client.connect();
    client.run();
    server.wait_done();

    expect_false(client.connected(), "server close should disconnect client");
    expect_equal(close_count, 1, "on_close count");
    expect_true(reconnect.reconnect_requested(), "reconnect should be requested");
    expect_equal(reconnect.reason(), ReconnectReason::ConnectionClosed, "reason");
}

void SourceRuntimeReconnectResubscribes() {
    std::vector<std::string> subscriptions;
    std::mutex mutex;

    FakeWebSocketServer server(2, [&](ServerWebSocket& ws, std::size_t) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            subscriptions.push_back(read_text(ws));
        }

        close_normal(ws);
    });
    server.start();

    ReconnectController reconnect(0, 1'000, 3);
    bool reconnecting = false;
    int open_count = 0;
    int close_count = 0;

    WebSocketClient client(server.endpoint());
    client.set_on_open([&]() {
        ++open_count;
        client.send(std::string(kSubscription));

        if (!reconnecting) {
            reconnect.on_connection_open(1000);
        }
    });
    client.set_on_close([&]() {
        ++close_count;

        if (close_count == 1) {
            reconnect.request_reconnect(2000, ReconnectReason::ConnectionClosed);
        }
    });

    client.connect();
    client.run();

    expect_true(reconnect.reconnect_requested(), "first close should request reconnect");
    expect_true(reconnect.should_reconnect(2000), "zero backoff should reconnect now");

    reconnect.on_reconnect_attempt(3000);
    reconnecting = true;
    client.connect();
    reconnect.on_reconnect_success(3500);
    reconnecting = false;
    client.run();

    server.wait_done();

    expect_equal(open_count, 2, "on_open count");
    expect_equal(close_count, 2, "on_close count");
    expect_equal(subscriptions.size(), std::size_t{2}, "subscription count");
    expect_equal(subscriptions[0], std::string(kSubscription), "first subscription");
    expect_equal(subscriptions[1], std::string(kSubscription), "second subscription");
    expect_equal(reconnect.reconnect_count(), 1ULL, "reconnect_count");
    expect_equal(reconnect.connection_id(), 2ULL, "connection_id");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"WebSocketEndpointSetting", &WebSocketEndpointSetting},
        {"WebSocketConnectCallsOnOpen", &WebSocketConnectCallsOnOpen},
        {"WebSocketReceivesRawMessage", &WebSocketReceivesRawMessage},
        {"WebSocketSendPreservesPayload", &WebSocketSendPreservesPayload},
        {"WebSocketCloseCallback", &WebSocketCloseCallback},
        {"WebSocketErrorCallbackOnConnectFailure", &WebSocketErrorCallbackOnConnectFailure},
        {"WebSocketErrorCallbackOnReadError", &WebSocketErrorCallbackOnReadError},
        {"HeartbeatOpenAllowsImmediatePing", &HeartbeatOpenAllowsImmediatePing},
        {"HeartbeatPingSentWaitingPong", &HeartbeatPingSentWaitingPong},
        {"HeartbeatPongRestoresOk", &HeartbeatPongRestoresOk},
        {"HeartbeatPongTimeout", &HeartbeatPongTimeout},
        {"HeartbeatStaleDetection", &HeartbeatStaleDetection},
        {"HeartbeatMarketMessageUpdatesLastMessage", &HeartbeatMarketMessageUpdatesLastMessage},
        {"ReconnectRequestReconnect", &ReconnectRequestReconnect},
        {"ReconnectBackoffNotElapsed", &ReconnectBackoffNotElapsed},
        {"ReconnectBackoffElapsed", &ReconnectBackoffElapsed},
        {"ReconnectAttemptState", &ReconnectAttemptState},
        {"ReconnectSuccessResetsBackoff", &ReconnectSuccessResetsBackoff},
        {"ReconnectFailureBackoff", &ReconnectFailureBackoff},
        {"ReconnectMaxAttemptsDisables", &ReconnectMaxAttemptsDisables},
        {
            "ReconnectDuplicateRequestDoesNotRefreshTimer",
            &ReconnectDuplicateRequestDoesNotRefreshTimer
        },
        {"SourceRuntimeOpenSendsSubscription", &SourceRuntimeOpenSendsSubscription},
        {"SourceRuntimeRawMessageCapture", &SourceRuntimeRawMessageCapture},
        {"SourceRuntimePingPongOk", &SourceRuntimePingPongOk},
        {
            "SourceRuntimePongTimeoutRequestsReconnect",
            &SourceRuntimePongTimeoutRequestsReconnect
        },
        {
            "SourceRuntimeStaleFeedRequestsReconnect",
            &SourceRuntimeStaleFeedRequestsReconnect
        },
        {
            "SourceRuntimeServerCloseRequestsReconnect",
            &SourceRuntimeServerCloseRequestsReconnect
        },
        {"SourceRuntimeReconnectResubscribes", &SourceRuntimeReconnectResubscribes}
    };

    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;

    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}

#include "feed/source_runtime/WebSocketClient.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace trading_engine::feed {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;

using tcp = net::ip::tcp;
using PlainWebSocketStream = websocket::stream<beast::tcp_stream>;
using TlsWebSocketStream = websocket::stream<
    beast::ssl_stream<beast::tcp_stream>
>;

namespace {

/**
 * @brief Parsed WebSocket endpoint.
 *
 * Example:
 *
 *   wss://ws-subscriptions-clob.polymarket.com/ws/market
 *
 * becomes:
 *
 *   scheme = "wss"
 *   host   = "ws-subscriptions-clob.polymarket.com"
 *   port   = "443"
 *   target = "/ws/market"
 */
struct ParsedEndpoint {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

/**
 * @brief Parse a minimal ws:// or wss:// endpoint.
 *
 * This parser is intentionally simple. It supports:
 *
 * - wss://host/path
 * - ws://host/path
 * - wss://host:port/path
 * - ws://host:port/path
 *
 * It does not support query rewriting, username/password, IPv6 literals,
 * or other complex URI features. That is acceptable for the MVP.
 */
ParsedEndpoint parse_endpoint(const std::string& endpoint) {
    ParsedEndpoint parsed;

    const auto scheme_end = endpoint.find("://");
    if (scheme_end == std::string::npos) {
        throw std::invalid_argument("WebSocket endpoint missing scheme: " + endpoint);
    }

    parsed.scheme = endpoint.substr(0, scheme_end);

    if (parsed.scheme != "wss" && parsed.scheme != "ws") {
        throw std::invalid_argument("Unsupported WebSocket scheme: " + parsed.scheme);
    }

    const auto host_start = scheme_end + 3;
    const auto path_start = endpoint.find('/', host_start);

    std::string host_port;

    if (path_start == std::string::npos) {
        host_port = endpoint.substr(host_start);
        parsed.target = "/";
    } else {
        host_port = endpoint.substr(host_start, path_start - host_start);
        parsed.target = endpoint.substr(path_start);
    }

    if (host_port.empty()) {
        throw std::invalid_argument("WebSocket endpoint missing host: " + endpoint);
    }

    const auto colon_pos = host_port.find(':');

    if (colon_pos == std::string::npos) {
        parsed.host = host_port;
        parsed.port = (parsed.scheme == "wss") ? "443" : "80";
    } else {
        parsed.host = host_port.substr(0, colon_pos);
        parsed.port = host_port.substr(colon_pos + 1);
    }

    if (parsed.host.empty() || parsed.port.empty()) {
        throw std::invalid_argument("Invalid WebSocket host/port: " + endpoint);
    }

    return parsed;
}

/**
 * @brief Return host header used for WebSocket handshake.
 *
 * If non-default port is used, include host:port.
 */
std::string make_host_header(const ParsedEndpoint& parsed) {
    const bool default_wss = parsed.scheme == "wss" && parsed.port == "443";
    const bool default_ws = parsed.scheme == "ws" && parsed.port == "80";

    if (default_wss || default_ws) {
        return parsed.host;
    }

    return parsed.host + ":" + parsed.port;
}

template <typename Stream>
void configure_websocket(Stream& ws) {
    ws.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client)
    );

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(
                boost::beast::http::field::user_agent,
                std::string("trading_engine_feed_mvp")
            );
        }
    ));
}

}  // namespace

/**
 * @brief Private implementation.
 *
 * We hide Boost.Beast/OpenSSL types inside Impl so WebSocketClient.h stays
 * clean and does not force the rest of the project to include Boost headers.
 *
 * This implementation is synchronous and blocking:
 *
 * - connect() blocks until handshake succeeds or fails.
 * - run() blocks while reading messages.
 * - send() writes synchronously.
 *
 * For the MVP spike, this is fine. Later, if you need high concurrency or
 * lower latency, replace this with an async implementation.
 */
struct WebSocketClient::Impl {
    explicit Impl(std::string endpoint_arg)
        : endpoint(std::move(endpoint_arg)),
          ssl_ctx(ssl::context::tls_client),
          resolver(ioc) {
        // Ask OpenSSL to use the system's default CA paths.
        //
        // If this fails on your machine, you may need to install CA certificates
        // or explicitly configure certificate paths.
        ssl_ctx.set_default_verify_paths();

        // Verify server certificate.
        //
        // Do not disable verification in production. For a quick local spike,
        // people sometimes use verify_none, but that defeats TLS identity checks.
        ssl_ctx.set_verify_mode(ssl::verify_peer);
    }

    std::string endpoint;

    net::io_context ioc;
    ssl::context ssl_ctx;
    tcp::resolver resolver;

    std::unique_ptr<PlainWebSocketStream> plain_ws;
    std::unique_ptr<TlsWebSocketStream> tls_ws;
    bool using_tls{false};

    std::atomic<bool> connected{false};
    std::atomic<bool> disconnect_requested{false};
    std::atomic<bool> read_loop_running{false};
    std::atomic<bool> close_callback_emitted{false};

    std::mutex send_mutex;

    OpenCallback on_open;
    MessageCallback on_message;
    CloseCallback on_close;
    ErrorCallback on_error;
};

struct ReadLoopGuard {
    std::atomic<bool>& running;

    explicit ReadLoopGuard(std::atomic<bool>& running_arg) noexcept
        : running(running_arg) {
        running.store(true);
    }

    ~ReadLoopGuard() {
        running.store(false);
    }
};

WebSocketClient::WebSocketClient(std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
WebSocketClient& WebSocketClient::operator=(WebSocketClient&&) noexcept = default;

void WebSocketClient::set_endpoint(std::string endpoint) {
    if (connected()) {
        throw std::runtime_error("Cannot change endpoint while WebSocket is connected");
    }

    impl_->endpoint = std::move(endpoint);
}

void WebSocketClient::set_on_open(OpenCallback cb) {
    impl_->on_open = std::move(cb);
}

void WebSocketClient::set_on_message(MessageCallback cb) {
    impl_->on_message = std::move(cb);
}

void WebSocketClient::set_on_close(CloseCallback cb) {
    impl_->on_close = std::move(cb);
}

void WebSocketClient::set_on_error(ErrorCallback cb) {
    impl_->on_error = std::move(cb);
}

void WebSocketClient::connect() {
    try {
        if (impl_->connected.load()) {
            throw std::runtime_error("WebSocket is already connected");
        }

        if (impl_->endpoint.empty()) {
            throw std::runtime_error("WebSocket endpoint is empty");
        }

        const ParsedEndpoint parsed = parse_endpoint(impl_->endpoint);
        const auto results = impl_->resolver.resolve(parsed.host, parsed.port);
        const std::string host_header = make_host_header(parsed);

        impl_->ioc.restart();
        impl_->plain_ws.reset();
        impl_->tls_ws.reset();
        impl_->disconnect_requested.store(false);
        impl_->close_callback_emitted.store(false);

        if (parsed.scheme == "wss") {
            impl_->using_tls = true;
            impl_->tls_ws = std::make_unique<TlsWebSocketStream>(
                impl_->ioc,
                impl_->ssl_ctx
            );

            auto& ws = *impl_->tls_ws;

            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(ws).connect(results);

            // Many TLS servers require SNI.
            if (!SSL_set_tlsext_host_name(
                    ws.next_layer().native_handle(),
                    parsed.host.c_str())) {
                beast::error_code ec{
                    static_cast<int>(::ERR_get_error()),
                    net::error::get_ssl_category()
                };
                throw beast::system_error{ec};
            }

            ws.next_layer().set_verify_callback(
                ssl::host_name_verification(parsed.host)
            );

            ws.next_layer().handshake(ssl::stream_base::client);
            configure_websocket(ws);
            ws.handshake(host_header, parsed.target);
        } else {
            impl_->using_tls = false;
            impl_->plain_ws = std::make_unique<PlainWebSocketStream>(
                impl_->ioc
            );

            auto& ws = *impl_->plain_ws;

            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(ws).connect(results);

            configure_websocket(ws);
            ws.handshake(host_header, parsed.target);
        }

        impl_->connected.store(true);

        if (impl_->on_open) {
            impl_->on_open();
        }
    } catch (const std::exception& e) {
        impl_->connected.store(false);
        impl_->plain_ws.reset();
        impl_->tls_ws.reset();

        if (impl_->on_error) {
            impl_->on_error(e.what());
        }

        throw;
    }
}

void WebSocketClient::run() {
    ReadLoopGuard guard(impl_->read_loop_running);

    while (impl_->connected.load()) {
        beast::flat_buffer buffer;

        try {
            if (impl_->using_tls) {
                if (!impl_->tls_ws) {
                    throw std::runtime_error("TLS WebSocket stream is not open");
                }

                impl_->tls_ws->read(buffer);
            } else {
                if (!impl_->plain_ws) {
                    throw std::runtime_error("plain WebSocket stream is not open");
                }

                impl_->plain_ws->read(buffer);
            }

            const auto payload = beast::buffers_to_string(buffer.data());

            if (impl_->on_message) {
                impl_->on_message(payload);
            }
        } catch (const beast::system_error& e) {
            impl_->connected.store(false);

            if (!impl_->disconnect_requested.load() &&
                e.code() != websocket::error::closed &&
                impl_->on_error) {
                impl_->on_error(e.what());
            }

            bool expected = false;
            if (impl_->close_callback_emitted.compare_exchange_strong(
                    expected,
                    true
                ) &&
                impl_->on_close) {
                impl_->on_close();
            }

            return;
        } catch (const std::exception& e) {
            impl_->connected.store(false);

            if (!impl_->disconnect_requested.load() && impl_->on_error) {
                impl_->on_error(e.what());
            }

            bool expected = false;
            if (impl_->close_callback_emitted.compare_exchange_strong(
                    expected,
                    true
                ) &&
                impl_->on_close) {
                impl_->on_close();
            }

            return;
        }
    }
}

void WebSocketClient::send(const std::string& message) {
    if (!impl_->connected.load()) {
        throw std::runtime_error("Cannot send WebSocket message: not connected");
    }

    std::lock_guard<std::mutex> lock(impl_->send_mutex);

    if (impl_->using_tls) {
        if (!impl_->tls_ws) {
            throw std::runtime_error("TLS WebSocket stream is not open");
        }

        impl_->tls_ws->text(true);
        impl_->tls_ws->write(net::buffer(message));
    } else {
        if (!impl_->plain_ws) {
            throw std::runtime_error("plain WebSocket stream is not open");
        }

        impl_->plain_ws->text(true);
        impl_->plain_ws->write(net::buffer(message));
    }
}

void WebSocketClient::disconnect() noexcept {
    if (!impl_ || !impl_->connected.exchange(false)) {
        return;
    }

    impl_->disconnect_requested.store(true);

    try {
        beast::error_code ec;

        if (impl_->read_loop_running.load()) {
            if (impl_->using_tls && impl_->tls_ws) {
                auto& socket = beast::get_lowest_layer(*impl_->tls_ws).socket();
                socket.shutdown(tcp::socket::shutdown_both, ec);
                ec.clear();
                socket.close(ec);
            } else if (impl_->plain_ws) {
                auto& socket = beast::get_lowest_layer(*impl_->plain_ws).socket();
                socket.shutdown(tcp::socket::shutdown_both, ec);
                ec.clear();
                socket.close(ec);
            }
        } else if (impl_->using_tls && impl_->tls_ws) {
            impl_->tls_ws->close(websocket::close_code::normal, ec);
        } else if (impl_->plain_ws) {
            impl_->plain_ws->close(websocket::close_code::normal, ec);
        }

        bool expected = false;
        if (impl_->close_callback_emitted.compare_exchange_strong(
                expected,
                true
            ) &&
            impl_->on_close) {
            impl_->on_close();
        }
    } catch (...) {
        // noexcept: do not allow exceptions to escape.
    }
}

const std::string& WebSocketClient::endpoint() const noexcept {
    return impl_->endpoint;
}

bool WebSocketClient::connected() const noexcept {
    return impl_->connected.load();
}

}  // namespace trading_engine::feed

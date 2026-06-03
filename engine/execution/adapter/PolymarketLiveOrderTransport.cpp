#include "engine/execution/adapter/LiveOrderBridge.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trading_engine::execution {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

std::string normalized_base64(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    const auto padding = (4 - (value.size() % 4)) % 4;
    value.append(padding, '=');
    return value;
}

std::vector<unsigned char> base64_decode_urlsafe(const std::string& value) {
    const auto normalized = normalized_base64(value);
    std::vector<unsigned char> decoded((normalized.size() * 3) / 4 + 4);

    const auto decoded_len = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(normalized.data()),
        static_cast<int>(normalized.size())
    );
    if (decoded_len < 0) {
        throw std::runtime_error("invalid polymarket API secret encoding");
    }

    auto padding = 0;
    for (auto it = normalized.rbegin();
         it != normalized.rend() && *it == '=';
         ++it) {
        ++padding;
    }

    decoded.resize(static_cast<std::size_t>(decoded_len - padding));
    return decoded;
}

std::string base64_encode_urlsafe(
    const unsigned char* data,
    std::size_t size
) {
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const auto len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        data,
        static_cast<int>(size)
    );
    if (len < 0) {
        throw std::runtime_error("failed to encode polymarket signature");
    }
    encoded.resize(static_cast<std::size_t>(len));
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::string hmac_sha256_urlsafe_base64(
    const std::string& secret,
    const std::string& message
) {
    const auto key = base64_decode_urlsafe(secret);
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;

    const auto* result = HMAC(
        EVP_sha256(),
        key.data(),
        static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        digest,
        &digest_len
    );
    if (result == nullptr) {
        throw std::runtime_error("failed to compute polymarket HMAC");
    }

    return base64_encode_urlsafe(digest, digest_len);
}

std::int64_t unix_time_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

http::verb verb_from_method(std::string_view method) {
    if (method == "POST") {
        return http::verb::post;
    }
    if (method == "DELETE") {
        return http::verb::delete_;
    }
    if (method == "GET") {
        return http::verb::get;
    }
    throw std::runtime_error("unsupported live HTTP method");
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    escaped += ' ';
                } else {
                    escaped += c;
                }
        }
    }
    return escaped;
}

const json::value* find_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &it->value();
}

std::string string_field(const json::object& object, const char* name) {
    const auto* value = find_field(object, name);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return json::value_to<std::string>(*value);
}

bool bool_field(const json::object& object, const char* name) {
    const auto* value = find_field(object, name);
    return value != nullptr && value->is_bool() && value->as_bool();
}

std::string error_field(const json::object& object) {
    auto error = string_field(object, "error");
    if (!error.empty()) {
        return error;
    }
    error = string_field(object, "errorMsg");
    if (!error.empty()) {
        return error;
    }
    return string_field(object, "message");
}

LiveTransportSubmitResult parse_submit_response(const std::string& raw) {
    boost::system::error_code parse_error;
    const auto value = json::parse(raw, parse_error);
    if (parse_error || !value.is_object()) {
        return {
            .ok = false,
            .raw_response = raw,
            .error = "invalid order response JSON"
        };
    }

    const auto& object = value.as_object();
    const auto order_id = string_field(object, "orderID");
    const auto status = string_field(object, "status");
    const auto success = bool_field(object, "success");
    if (!success || order_id.empty()) {
        auto error = error_field(object);
        if (error.empty()) {
            error = "polymarket order response did not contain success/orderID";
        }
        return {
            .ok = false,
            .venue_status = status,
            .raw_response = raw,
            .error = std::move(error)
        };
    }

    return {
        .ok = true,
        .venue_order_id = order_id,
        .venue_status = status,
        .raw_response = raw
    };
}

LiveTransportCancelResult parse_cancel_response(
    const std::string& raw,
    std::string_view venue_order_id
) {
    boost::system::error_code parse_error;
    const auto value = json::parse(raw, parse_error);
    if (parse_error || !value.is_object()) {
        return {
            .ok = false,
            .raw_response = raw,
            .error = "invalid cancel response JSON"
        };
    }

    const auto& object = value.as_object();
    const auto* canceled = find_field(object, "canceled");
    if (canceled != nullptr && canceled->is_array()) {
        for (const auto& id : canceled->as_array()) {
            if (id.is_string() &&
                json::value_to<std::string>(id) == venue_order_id) {
                return {
                    .ok = true,
                    .raw_response = raw
                };
            }
        }
    }

    std::string error = "venue order was not canceled";
    const auto* not_canceled = find_field(object, "not_canceled");
    if (not_canceled != nullptr && not_canceled->is_object()) {
        const auto& reasons = not_canceled->as_object();
        const auto it = reasons.find(std::string(venue_order_id));
        if (it != reasons.end() && it->value().is_string()) {
            error = json::value_to<std::string>(it->value());
        }
    }

    return {
        .ok = false,
        .raw_response = raw,
        .error = std::move(error)
    };
}

}  // namespace

PolymarketL2Authenticator::PolymarketL2Authenticator(
    PolymarketL2Credentials credentials
) : credentials_(std::move(credentials)) {}

PolymarketL2Headers PolymarketL2Authenticator::build_headers(
    std::string_view method,
    std::string_view request_path,
    std::string_view body,
    std::int64_t unix_timestamp_seconds
) const {
    if (!credentials_.complete()) {
        throw std::runtime_error("incomplete polymarket L2 credentials");
    }

    auto timestamp = std::to_string(unix_timestamp_seconds);
    std::string message;
    message.reserve(
        timestamp.size() +
        method.size() +
        request_path.size() +
        body.size()
    );
    message += timestamp;
    message += method;
    message += request_path;
    message += body;

    return {
        .address = credentials_.address,
        .api_key = credentials_.api_key,
        .passphrase = credentials_.passphrase,
        .timestamp = std::move(timestamp),
        .signature = hmac_sha256_urlsafe_base64(
            credentials_.secret,
            message
        )
    };
}

PolymarketHttpsOrderTransport::PolymarketHttpsOrderTransport(
    PolymarketL2Credentials credentials,
    std::string host,
    std::string port
) : authenticator_(std::move(credentials)),
    host_(std::move(host)),
    port_(std::move(port)) {}

LiveTransportSubmitResult PolymarketHttpsOrderTransport::submit_order(
    std::string_view request_body_json
) {
    try {
        const auto raw = authenticated_request(
            "POST",
            "/order",
            request_body_json
        );
        return parse_submit_response(raw);
    } catch (const std::exception& error) {
        return {
            .ok = false,
            .error = error.what()
        };
    }
}

LiveTransportCancelResult PolymarketHttpsOrderTransport::cancel_order(
    std::string_view venue_order_id
) {
    try {
        const auto body = std::string{"{\"orderID\":\""} +
            json_escape(venue_order_id) +
            "\"}";
        const auto raw = authenticated_request("DELETE", "/order", body);
        return parse_cancel_response(raw, venue_order_id);
    } catch (const std::exception& error) {
        return {
            .ok = false,
            .error = error.what()
        };
    }
}

std::string PolymarketHttpsOrderTransport::authenticated_request(
    std::string_view method,
    std::string_view request_path,
    std::string_view body
) const {
    const auto headers = authenticator_.build_headers(
        method,
        request_path,
        body,
        unix_time_seconds()
    );

    asio::io_context io;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(io);
    beast::ssl_stream<beast::tcp_stream> stream(io, tls);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
        throw beast::system_error(
            beast::error_code(
                static_cast<int>(::ERR_get_error()),
                asio::error::get_ssl_category()
            )
        );
    }

    const auto results = resolver.resolve(host_, port_);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> request{
        verb_from_method(method),
        std::string(request_path),
        11
    };
    request.set(http::field::host, host_);
    request.set(http::field::user_agent, "Polytope-live-execution");
    request.set(http::field::content_type, "application/json");
    request.set("POLY_ADDRESS", headers.address);
    request.set("POLY_API_KEY", headers.api_key);
    request.set("POLY_PASSPHRASE", headers.passphrase);
    request.set("POLY_TIMESTAMP", headers.timestamp);
    request.set("POLY_SIGNATURE", headers.signature);
    request.body() = std::string(body);
    request.prepare_payload();

    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);

    beast::error_code shutdown_error;
    stream.shutdown(shutdown_error);
    if (shutdown_error == asio::error::eof ||
        shutdown_error == ssl::error::stream_truncated) {
        shutdown_error = {};
    }
    if (shutdown_error) {
        throw beast::system_error(shutdown_error);
    }

    const auto status = response.result_int();
    if (status < 200 || status >= 300) {
        throw std::runtime_error(
            "polymarket HTTP " +
            std::to_string(status) +
            ": " +
            response.body()
        );
    }

    return response.body();
}

}  // namespace trading_engine::execution

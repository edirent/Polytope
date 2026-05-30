#include "oracle/ingestion/MarketApiClient.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <sstream>
#include <utility>

namespace trading_engine::oracle {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct ParsedHttpsUrl {
    std::string host;
    std::string port = "443";
    std::string target = "/";
};

[[nodiscard]] std::uint64_t now_wall_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

[[nodiscard]] bool parse_https_url(
    std::string_view url,
    ParsedHttpsUrl* out
) {
    constexpr std::string_view prefix = "https://";
    if (!url.starts_with(prefix) || !out) {
        return false;
    }

    auto rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    const auto authority = slash == std::string_view::npos
        ? rest
        : rest.substr(0, slash);
    out->target = slash == std::string_view::npos
        ? "/"
        : std::string{rest.substr(slash)};

    const auto colon = authority.find(':');
    if (colon == std::string_view::npos) {
        out->host = std::string{authority};
        out->port = "443";
    } else {
        out->host = std::string{authority.substr(0, colon)};
        out->port = std::string{authority.substr(colon + 1)};
    }

    return !out->host.empty() && !out->target.empty();
}

[[nodiscard]] std::string bool_text(bool value) {
    return value ? "true" : "false";
}

[[nodiscard]] std::string gamma_url(const MarketApiFetchOptions& options) {
    std::ostringstream url;
    url << options.endpoint;
    url << "?active=" << bool_text(options.active);
    url << "&closed=" << bool_text(options.closed);
    url << "&archived=" << bool_text(options.archived);
    url << "&limit=" << options.limit;
    url << "&offset=" << options.offset;
    return url.str();
}

[[nodiscard]] std::string get_https(
    const std::string& url_text,
    std::string* error
) {
    ParsedHttpsUrl url;
    if (!parse_https_url(url_text, &url)) {
        if (error) {
            *error = "unsupported URL: " + url_text;
        }
        return {};
    }

    try {
        asio::io_context io;
        ssl::context ssl_context(ssl::context::tls_client);
        ssl_context.set_default_verify_paths();

        tcp::resolver resolver(io);
        beast::ssl_stream<beast::tcp_stream> stream(io, ssl_context);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
            if (error) {
                *error = "failed to set TLS SNI";
            }
            return {};
        }

        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        const auto results = resolver.resolve(url.host, url.port);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::empty_body> request{
            http::verb::get,
            url.target,
            11
        };
        request.set(http::field::host, url.host);
        request.set(http::field::user_agent, "PolytopeOracle/0.1");
        request.set(http::field::accept, "application/json");

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
        if (shutdown_error && error) {
            *error = "TLS shutdown failed: " + shutdown_error.message();
        }

        const auto status = response.result_int();
        if (status < 200 || status >= 300) {
            if (error) {
                *error = "HTTP status " + std::to_string(status);
            }
            return {};
        }

        return response.body();
    } catch (const std::exception& ex) {
        if (error) {
            *error = std::string{"request failed: "} + ex.what();
        }
        return {};
    }
}

[[nodiscard]] std::string string_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return {};
    }
    if (it->value().is_string()) {
        return json::value_to<std::string>(it->value());
    }
    if (it->value().is_uint64()) {
        return std::to_string(it->value().as_uint64());
    }
    if (it->value().is_int64()) {
        return std::to_string(it->value().as_int64());
    }
    return {};
}

[[nodiscard]] bool bool_field(
    const json::object& object,
    const char* name,
    bool default_value
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_bool()) {
        return default_value;
    }
    return it->value().as_bool();
}

void append_string_value(
    const json::value& value,
    std::vector<std::string>* out
) {
    if (value.is_string()) {
        out->push_back(json::value_to<std::string>(value));
    } else if (value.is_uint64()) {
        out->push_back(std::to_string(value.as_uint64()));
    } else if (value.is_int64()) {
        out->push_back(std::to_string(value.as_int64()));
    }
}

[[nodiscard]] std::vector<std::string> array_from_value(
    const json::value& value
) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            append_string_value(item, &out);
        }
        return out;
    }

    if (!value.is_string()) {
        return out;
    }

    const auto text = json::value_to<std::string>(value);
    boost::json::error_code error;
    const auto parsed = json::parse(text, error);
    if (error || !parsed.is_array()) {
        return out;
    }

    for (const auto& item : parsed.as_array()) {
        append_string_value(item, &out);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> string_array_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return {};
    }
    return array_from_value(it->value());
}

[[nodiscard]] std::string first_event_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find("events");
    if (it == object.end() || !it->value().is_array() ||
        it->value().as_array().empty()) {
        return {};
    }

    const auto& first = it->value().as_array().front();
    if (!first.is_object()) {
        return {};
    }
    return string_field(first.as_object(), name);
}

[[nodiscard]] std::vector<std::string> tags_from_market(
    const json::object& object
) {
    std::vector<std::string> tags;
    const auto append_tag_object = [&tags](const json::object& tag_object) {
        for (const char* field : {"label", "name", "slug"}) {
            const auto value = string_field(tag_object, field);
            if (!value.empty()) {
                tags.push_back(value);
                return;
            }
        }
    };

    const auto parse_tags_value = [&](const json::value& value) {
        if (!value.is_array()) {
            return;
        }
        for (const auto& item : value.as_array()) {
            if (item.is_string()) {
                tags.push_back(json::value_to<std::string>(item));
            } else if (item.is_object()) {
                append_tag_object(item.as_object());
            }
        }
    };

    if (const auto it = object.find("tags"); it != object.end()) {
        parse_tags_value(it->value());
    }

    const auto events_it = object.find("events");
    if (events_it != object.end() && events_it->value().is_array() &&
        !events_it->value().as_array().empty()) {
        const auto& first = events_it->value().as_array().front();
        if (first.is_object()) {
            if (const auto tags_it = first.as_object().find("tags");
                tags_it != first.as_object().end()) {
                parse_tags_value(tags_it->value());
            }
        }
    }

    return tags;
}

[[nodiscard]] RawMarketRecord record_from_gamma_market(
    const json::object& object,
    std::uint64_t fetched_at_ns
) {
    RawMarketRecord record;
    const auto condition_id = string_field(object, "conditionId");
    const auto gamma_id = string_field(object, "id");

    record.market_id = condition_id.empty() ? gamma_id : condition_id;
    record.event_id = first_event_field(object, "id");
    if (record.event_id.empty()) {
        record.event_id = string_field(object, "event_id");
    }

    record.title = string_field(object, "question");
    if (record.title.empty()) {
        record.title = string_field(object, "title");
    }

    record.description = string_field(object, "description");
    const auto event_title = first_event_field(object, "title");
    const auto event_description = first_event_field(object, "description");
    if (!event_title.empty()) {
        record.description += "\n\nEvent title: " + event_title;
    }
    if (!event_description.empty()) {
        record.description += "\n\nEvent description: " + event_description;
    }

    record.outcomes = string_array_field(object, "outcomes");
    record.asset_ids = string_array_field(object, "clobTokenIds");
    if (record.asset_ids.empty()) {
        record.asset_ids = string_array_field(object, "asset_ids");
    }

    record.resolution_source = string_field(object, "resolutionSource");
    record.end_time = string_field(object, "endDate");
    if (record.end_time.empty()) {
        record.end_time = string_field(object, "endDateIso");
    }

    record.tags = tags_from_market(object);
    record.fetched_at_ns = fetched_at_ns;
    record.source = "polymarket_gamma";
    return record;
}

void validate_gamma_record(
    const RawMarketRecord& record,
    std::size_t index,
    MarketApiFetchResult* result
) {
    const auto prefix = "markets[" + std::to_string(index) + "]: ";
    if (record.market_id.empty()) {
        result->warnings.push_back(prefix + "missing conditionId/id");
    }
    if (record.outcomes.empty()) {
        result->warnings.push_back(prefix + "missing outcomes");
    }
    if (record.asset_ids.empty()) {
        result->warnings.push_back(prefix + "missing clobTokenIds");
    }
    if (!record.outcomes.empty() && !record.asset_ids.empty() &&
        record.outcomes.size() != record.asset_ids.size()) {
        result->warnings.push_back(
            prefix + "outcomes and clobTokenIds count mismatch"
        );
    }
}

}  // namespace

MarketApiFetchResult MarketApiClient::fetch_active_markets() const {
    return fetch_markets(MarketApiFetchOptions{});
}

MarketApiFetchResult MarketApiClient::fetch_markets(
    const MarketApiFetchOptions& options
) const {
    MarketApiFetchResult result;

    std::string error;
    const auto body = get_https(gamma_url(options), &error);
    if (!error.empty()) {
        result.errors.push_back(error);
        return result;
    }

    result = parse_polymarket_gamma_markets(body, now_wall_ns());
    if (!options.require_order_book) {
        return result;
    }

    std::vector<RawMarketRecord> filtered;
    for (auto& record : result.records) {
        if (record.outcomes.empty() || record.asset_ids.empty() ||
            record.outcomes.size() != record.asset_ids.size()) {
            result.warnings.push_back(
                "skipping market without usable outcome/token mapping: " +
                record.market_id
            );
            continue;
        }
        filtered.push_back(std::move(record));
    }
    result.records = std::move(filtered);
    return result;
}

MarketApiFetchResult parse_polymarket_gamma_markets(
    std::string_view response_body,
    std::uint64_t fetched_at_ns
) {
    MarketApiFetchResult result;

    boost::json::error_code error;
    const auto parsed = json::parse(response_body, error);
    if (error) {
        result.errors.push_back("malformed Polymarket Gamma response");
        return result;
    }

    const json::array* markets = nullptr;
    if (parsed.is_array()) {
        markets = &parsed.as_array();
    } else if (parsed.is_object()) {
        const auto& object = parsed.as_object();
        for (const char* field : {"markets", "data", "results"}) {
            const auto it = object.find(field);
            if (it != object.end() && it->value().is_array()) {
                markets = &it->value().as_array();
                break;
            }
        }
    }

    if (!markets) {
        result.errors.push_back("Polymarket Gamma response missing markets");
        return result;
    }

    result.response_count = static_cast<std::uint32_t>(markets->size());

    for (std::size_t i = 0; i < markets->size(); ++i) {
        const auto& value = (*markets)[i];
        if (!value.is_object()) {
            result.warnings.push_back(
                "markets[" + std::to_string(i) + "]: expected object"
            );
            continue;
        }

        const auto& object = value.as_object();
        if (!bool_field(object, "enableOrderBook", true)) {
            result.warnings.push_back(
                "markets[" + std::to_string(i) + "]: order book disabled"
            );
            continue;
        }

        auto record = record_from_gamma_market(object, fetched_at_ns);
        validate_gamma_record(record, i, &result);
        if (!record.market_id.empty() &&
            !record.outcomes.empty() &&
            !record.asset_ids.empty() &&
            record.outcomes.size() == record.asset_ids.size()) {
            result.records.push_back(std::move(record));
        }
    }

    return result;
}

}  // namespace trading_engine::oracle

#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using trading_engine::feed::PacketHeartbeat;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawPacket;

struct EventSchema {
    std::string type{"unknown"};
    bool has_event_type{false};
    bool has_type{false};
    std::set<std::string> fields;
};

struct InspectStats {
    std::uint64_t total_packets{0};
    std::uint64_t json_ok{0};
    std::uint64_t non_json{0};
    std::uint64_t decode_errors{0};
    std::uint64_t heartbeat_packets{0};
    std::uint64_t json_events{0};
    std::uint64_t array_wrapped_packets{0};
    std::uint64_t max_events_per_packet{0};

    std::map<std::string, std::uint64_t> type_counts;
    std::map<std::string, std::uint64_t> field_presence;
    std::map<std::string, std::uint64_t> unknown_types;
};

bool is_ws(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

void skip_ws(const std::string& input, std::size_t& pos) {
    while (pos < input.size() && is_ws(input[pos])) {
        ++pos;
    }
}

bool fail_parse(std::string& error, const std::string& message) {
    error = message;
    return false;
}

bool parse_string(
    const std::string& input,
    std::size_t& pos,
    std::string* out,
    std::string& error
) {
    if (pos >= input.size() || input[pos] != '"') {
        return fail_parse(error, "expected string");
    }

    ++pos;

    while (pos < input.size()) {
        const char c = input[pos++];

        if (c == '"') {
            return true;
        }

        if (c != '\\') {
            if (out) {
                out->push_back(c);
            }
            continue;
        }

        if (pos >= input.size()) {
            return fail_parse(error, "unterminated escape");
        }

        const char escaped = input[pos++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                if (out) {
                    out->push_back(escaped);
                }
                break;
            case 'b':
                if (out) {
                    out->push_back('\b');
                }
                break;
            case 'f':
                if (out) {
                    out->push_back('\f');
                }
                break;
            case 'n':
                if (out) {
                    out->push_back('\n');
                }
                break;
            case 'r':
                if (out) {
                    out->push_back('\r');
                }
                break;
            case 't':
                if (out) {
                    out->push_back('\t');
                }
                break;
            case 'u':
                if (pos + 4 > input.size()) {
                    return fail_parse(error, "truncated unicode escape");
                }
                pos += 4;
                if (out) {
                    out->push_back('?');
                }
                break;
            default:
                return fail_parse(error, "invalid escape");
        }
    }

    return fail_parse(error, "unterminated string");
}

bool skip_value(const std::string& input, std::size_t& pos, std::string& error);

bool skip_object(const std::string& input, std::size_t& pos, std::string& error) {
    if (pos >= input.size() || input[pos] != '{') {
        return fail_parse(error, "expected object");
    }

    ++pos;
    skip_ws(input, pos);

    if (pos < input.size() && input[pos] == '}') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        std::string key;
        if (!parse_string(input, pos, &key, error)) {
            return false;
        }

        skip_ws(input, pos);
        if (pos >= input.size() || input[pos] != ':') {
            return fail_parse(error, "expected object colon");
        }
        ++pos;

        if (!skip_value(input, pos, error)) {
            return false;
        }

        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_ws(input, pos);
            continue;
        }

        if (pos < input.size() && input[pos] == '}') {
            ++pos;
            return true;
        }

        return fail_parse(error, "expected object comma or close");
    }

    return fail_parse(error, "unterminated object");
}

bool skip_array(const std::string& input, std::size_t& pos, std::string& error) {
    if (pos >= input.size() || input[pos] != '[') {
        return fail_parse(error, "expected array");
    }

    ++pos;
    skip_ws(input, pos);

    if (pos < input.size() && input[pos] == ']') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        if (!skip_value(input, pos, error)) {
            return false;
        }

        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_ws(input, pos);
            continue;
        }

        if (pos < input.size() && input[pos] == ']') {
            ++pos;
            return true;
        }

        return fail_parse(error, "expected array comma or close");
    }

    return fail_parse(error, "unterminated array");
}

bool skip_scalar(const std::string& input, std::size_t& pos) {
    const std::size_t start = pos;
    while (pos < input.size() &&
           !is_ws(input[pos]) &&
           input[pos] != ',' &&
           input[pos] != ']' &&
           input[pos] != '}') {
        ++pos;
    }

    return pos > start;
}

bool skip_value(const std::string& input, std::size_t& pos, std::string& error) {
    skip_ws(input, pos);

    if (pos >= input.size()) {
        return fail_parse(error, "expected value");
    }

    if (input[pos] == '"') {
        return parse_string(input, pos, nullptr, error);
    }

    if (input[pos] == '{') {
        return skip_object(input, pos, error);
    }

    if (input[pos] == '[') {
        return skip_array(input, pos, error);
    }

    if (skip_scalar(input, pos)) {
        return true;
    }

    return fail_parse(error, "invalid value");
}

bool inspect_object(
    const std::string& input,
    std::size_t& pos,
    EventSchema& event,
    std::string& error
) {
    if (pos >= input.size() || input[pos] != '{') {
        return fail_parse(error, "expected event object");
    }

    ++pos;
    skip_ws(input, pos);

    if (pos < input.size() && input[pos] == '}') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        std::string key;
        if (!parse_string(input, pos, &key, error)) {
            return false;
        }

        event.fields.insert(key);

        skip_ws(input, pos);
        if (pos >= input.size() || input[pos] != ':') {
            return fail_parse(error, "expected event object colon");
        }
        ++pos;
        skip_ws(input, pos);

        if ((key == "event_type" || key == "type") &&
            pos < input.size() &&
            input[pos] == '"') {
            std::string value;
            if (!parse_string(input, pos, &value, error)) {
                return false;
            }

            if (key == "event_type") {
                event.type = value.empty() ? "unknown" : value;
                event.has_event_type = true;
            } else if (!event.has_event_type) {
                event.type = value.empty() ? "unknown" : value;
                event.has_type = true;
            }
        } else if (!skip_value(input, pos, error)) {
            return false;
        }

        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_ws(input, pos);
            continue;
        }

        if (pos < input.size() && input[pos] == '}') {
            ++pos;
            return true;
        }

        return fail_parse(error, "expected event object comma or close");
    }

    return fail_parse(error, "unterminated event object");
}

bool inspect_array(
    const std::string& input,
    std::size_t& pos,
    std::vector<EventSchema>& events,
    std::string& error
) {
    if (pos >= input.size() || input[pos] != '[') {
        return fail_parse(error, "expected top-level array");
    }

    ++pos;
    skip_ws(input, pos);

    if (pos < input.size() && input[pos] == ']') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        if (input[pos] == '{') {
            EventSchema event;
            if (!inspect_object(input, pos, event, error)) {
                return false;
            }
            events.push_back(std::move(event));
        } else if (!skip_value(input, pos, error)) {
            return false;
        }

        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_ws(input, pos);
            continue;
        }

        if (pos < input.size() && input[pos] == ']') {
            ++pos;
            return true;
        }

        return fail_parse(error, "expected top-level array comma or close");
    }

    return fail_parse(error, "unterminated top-level array");
}

bool inspect_json_payload(
    const std::string& payload,
    std::vector<EventSchema>& events,
    bool& array_wrapped,
    std::string& error
) {
    std::size_t pos = 0;
    skip_ws(payload, pos);

    if (pos >= payload.size()) {
        return fail_parse(error, "empty payload");
    }

    if (payload[pos] == '{') {
        EventSchema event;
        if (!inspect_object(payload, pos, event, error)) {
            return false;
        }
        events.push_back(std::move(event));
    } else if (payload[pos] == '[') {
        array_wrapped = true;
        if (!inspect_array(payload, pos, events, error)) {
            return false;
        }
    } else {
        return fail_parse(error, "not a JSON object or array");
    }

    skip_ws(payload, pos);
    if (pos != payload.size()) {
        return fail_parse(error, "trailing bytes after JSON payload");
    }

    return true;
}

bool is_pong_payload(const std::string& payload) {
    return payload == "PONG" ||
           payload == "pong" ||
           payload == "\"PONG\"" ||
           payload == "\"pong\"";
}

bool is_known_type(const std::string& type) {
    static const std::set<std::string> known{
        "book",
        "price_change",
        "best_bid_ask",
        "last_trade_price",
        "market_resolved"
    };

    return known.find(type) != known.end();
}

void initialize_type_counts(InspectStats& stats) {
    stats.type_counts["book"] = 0;
    stats.type_counts["price_change"] = 0;
    stats.type_counts["best_bid_ask"] = 0;
    stats.type_counts["last_trade_price"] = 0;
    stats.type_counts["market_resolved"] = 0;
    stats.type_counts["unknown"] = 0;
}

void count_event(const EventSchema& event, InspectStats& stats) {
    ++stats.json_events;

    const std::string type = event.type.empty() ? "unknown" : event.type;
    if (is_known_type(type)) {
        ++stats.type_counts[type];
    } else {
        ++stats.type_counts["unknown"];
        ++stats.unknown_types[type];
    }

    if (!event.has_event_type && !event.has_type) {
        ++stats.unknown_types["<missing>"];
    }

    for (const auto& field : event.fields) {
        ++stats.field_presence[field];
    }
}

int fail(const std::string& message) {
    std::cerr << "inspect_polymarket_payloads failed: " << message << '\n';
    return 1;
}

int inspect(const std::string& raw_path) {
    RawLogReader reader(raw_path);
    InspectStats stats;
    initialize_type_counts(stats);

    while (true) {
        RawLogReadResult result = reader.next();
        if (result.eof()) {
            break;
        }

        if (!result.ok()) {
            return fail("raw read failed: " + result.message);
        }

        const RawPacket& packet = *result.packet;
        ++stats.total_packets;

        if ((packet.header.flags & PacketHeartbeat) != 0 ||
            is_pong_payload(packet.payload)) {
            ++stats.heartbeat_packets;
        }

        std::size_t first = 0;
        skip_ws(packet.payload, first);
        if (first >= packet.payload.size() ||
            (packet.payload[first] != '{' && packet.payload[first] != '[')) {
            ++stats.non_json;
            continue;
        }

        std::vector<EventSchema> events;
        bool array_wrapped = false;
        std::string error;

        if (!inspect_json_payload(packet.payload, events, array_wrapped, error)) {
            ++stats.decode_errors;
            continue;
        }

        ++stats.json_ok;
        if (array_wrapped) {
            ++stats.array_wrapped_packets;
        }

        stats.max_events_per_packet = std::max<std::uint64_t>(
            stats.max_events_per_packet,
            static_cast<std::uint64_t>(events.size())
        );

        for (const auto& event : events) {
            count_event(event, stats);
        }
    }

    std::cout << "total_packets: " << stats.total_packets << '\n';
    std::cout << "json_ok: " << stats.json_ok << '\n';
    std::cout << "non_json: " << stats.non_json << '\n';
    std::cout << "decode_errors: " << stats.decode_errors << '\n';
    std::cout << "heartbeat_packets: " << stats.heartbeat_packets << '\n';
    std::cout << "json_events: " << stats.json_events << '\n';
    std::cout << "array_wrapped_packets: " << stats.array_wrapped_packets << '\n';
    std::cout << "max_events_per_packet: " << stats.max_events_per_packet << '\n';

    std::cout << "\ntype_counts:\n";
    for (const auto& [type, count] : stats.type_counts) {
        std::cout << "  " << type << ": " << count << '\n';
    }

    std::cout << "\nfield_presence:\n";
    for (const auto& [field, count] : stats.field_presence) {
        std::cout << "  " << field << ": " << count << '\n';
    }

    std::cout << "\nunknown_types:\n";
    if (stats.unknown_types.empty()) {
        std::cout << "  <none>: 0\n";
    } else {
        for (const auto& [type, count] : stats.unknown_types) {
            std::cout << "  " << type << ": " << count << '\n';
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string raw_path =
        argc > 1 ? argv[1] : "tests/fixtures/polymarket/market_39.raw";

    if (argc > 2) {
        return fail("usage: inspect_polymarket_payloads [raw_path]");
    }

    try {
        return inspect(raw_path);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}

#include "oracle/ingestion/MarketDescriptionLoader.h"

#include <boost/json.hpp>

#include <fstream>
#include <sstream>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

std::string string_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return {};
    }
    if (!it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

std::uint64_t u64_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return 0;
    }
    if (it->value().is_uint64()) {
        return it->value().as_uint64();
    }
    if (it->value().is_int64() && it->value().as_int64() >= 0) {
        return static_cast<std::uint64_t>(it->value().as_int64());
    }
    return 0;
}

std::vector<std::string> string_array_field(
    const json::object& object,
    const char* name
) {
    std::vector<std::string> out;
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_array()) {
        return out;
    }

    for (const auto& item : it->value().as_array()) {
        if (item.is_string()) {
            out.push_back(json::value_to<std::string>(item));
        }
    }
    return out;
}

RawMarketRecord parse_record(const json::object& object) {
    RawMarketRecord record;
    record.market_id = string_field(object, "market_id");
    record.event_id = string_field(object, "event_id");
    record.title = string_field(object, "title");
    record.description = string_field(object, "description");
    record.outcomes = string_array_field(object, "outcomes");
    record.asset_ids = string_array_field(object, "asset_ids");
    record.resolution_source = string_field(object, "resolution_source");
    record.end_time = string_field(object, "end_time");
    record.tags = string_array_field(object, "tags");
    record.fetched_at_ns = u64_field(object, "fetched_at_ns");
    record.source = string_field(object, "source");
    return record;
}

void validate_record(
    const RawMarketRecord& record,
    std::size_t line_number,
    MarketDescriptionLoadResult* result
) {
    const auto prefix = "line " + std::to_string(line_number) + ": ";

    if (record.market_id.empty()) {
        result->errors.push_back(prefix + "missing market_id");
    }
    if (record.outcomes.empty()) {
        result->warnings.push_back(prefix + "missing outcomes");
    }
    if (record.asset_ids.empty()) {
        result->warnings.push_back(prefix + "missing asset_ids");
    }
    if (record.outcomes.size() != record.asset_ids.size()) {
        result->errors.push_back(
            prefix + "outcomes and asset_ids count mismatch"
        );
    }
}

json::array to_json_array(const std::vector<std::string>& values) {
    json::array out;
    for (const auto& value : values) {
        out.push_back(json::value(value));
    }
    return out;
}

}  // namespace

MarketDescriptionLoadResult MarketDescriptionLoader::load_jsonl(
    const std::string& path
) const {
    MarketDescriptionLoadResult result;

    std::ifstream input(path);
    if (!input) {
        result.errors.push_back("failed to open market fixture: " + path);
        return result;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        boost::json::error_code error;
        const auto parsed = json::parse(line, error);
        if (error || !parsed.is_object()) {
            result.errors.push_back(
                "line " + std::to_string(line_number) + ": malformed JSON"
            );
            continue;
        }

        RawMarketRecord record = parse_record(parsed.as_object());
        validate_record(record, line_number, &result);
        result.records.push_back(std::move(record));
    }

    return result;
}

std::string to_jsonl_line(const RawMarketRecord& record) {
    json::object object;
    object["market_id"] = record.market_id;
    object["event_id"] = record.event_id;
    object["title"] = record.title;
    object["description"] = record.description;
    object["outcomes"] = to_json_array(record.outcomes);
    object["asset_ids"] = to_json_array(record.asset_ids);
    object["resolution_source"] = record.resolution_source;
    object["end_time"] = record.end_time;
    object["tags"] = to_json_array(record.tags);
    object["fetched_at_ns"] = record.fetched_at_ns;
    object["source"] = record.source;

    std::ostringstream out;
    out << json::serialize(object);
    return out.str();
}

}  // namespace trading_engine::oracle

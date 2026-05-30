#include "oracle/llm/OpenRouterRuleExtractor.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
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

constexpr bool default_llm_enabled() noexcept {
#if ORACLE_ENABLE_LLM
    return true;
#else
    return false;
#endif
}

struct ParsedHttpsUrl {
    std::string host;
    std::string port = "443";
    std::string target = "/";
};

[[nodiscard]] std::string env_or_default(
    const char* name,
    std::string default_value
) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return default_value;
    }
    return value;
}

[[nodiscard]] std::uint32_t env_u32_or_default(
    const char* name,
    std::uint32_t default_value
) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return default_value;
    }

    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(value, &consumed);
        if (consumed != std::string{value}.size() || parsed == 0) {
            return default_value;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return default_value;
    }
}

[[nodiscard]] std::string trim_copy(std::string_view input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return std::string{input.substr(first, last - first + 1)};
}

[[nodiscard]] std::string strip_markdown_fence(std::string_view input) {
    auto text = trim_copy(input);
    if (!text.starts_with("```")) {
        return text;
    }

    const auto first_newline = text.find('\n');
    if (first_newline == std::string::npos) {
        return text;
    }
    auto body = text.substr(first_newline + 1);
    const auto last_fence = body.rfind("```");
    if (last_fence != std::string::npos) {
        body = body.substr(0, last_fence);
    }
    return trim_copy(body);
}

[[nodiscard]] std::string extract_balanced_json(std::string_view input) {
    const auto start = input.find_first_of("{[");
    if (start == std::string_view::npos) {
        return {};
    }

    const char open = input[start];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = start; i < input.size(); ++i) {
        const char ch = input[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == open) {
            ++depth;
        } else if (ch == close) {
            --depth;
            if (depth == 0) {
                return std::string{input.substr(start, i - start + 1)};
            }
        }
    }

    return {};
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

[[nodiscard]] std::string truncate_for_diagnostic(
    std::string_view input,
    std::size_t max_size = 512
) {
    auto text = trim_copy(input);
    if (text.size() <= max_size) {
        return text;
    }
    text.resize(max_size);
    text += "...";
    return text;
}

[[nodiscard]] std::vector<std::string> string_array_field(
    const json::object& object,
    const char* name
) {
    std::vector<std::string> out;
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_array()) {
        return out;
    }
    for (const auto& value : it->value().as_array()) {
        if (value.is_string()) {
            out.push_back(json::value_to<std::string>(value));
        }
    }
    return out;
}

[[nodiscard]] std::string string_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

[[nodiscard]] std::string text_from_content_part(const json::value& value) {
    if (value.is_string()) {
        return json::value_to<std::string>(value);
    }
    if (!value.is_object()) {
        return {};
    }

    const auto& object = value.as_object();
    if (const auto it = object.find("text");
        it != object.end() && it->value().is_string()) {
        return json::value_to<std::string>(it->value());
    }
    if (const auto it = object.find("content");
        it != object.end() && it->value().is_string()) {
        return json::value_to<std::string>(it->value());
    }
    return {};
}

[[nodiscard]] std::string message_content(
    const json::object& message
) {
    const auto it = message.find("content");
    if (it == message.end()) {
        return {};
    }

    const auto& content = it->value();
    if (content.is_string()) {
        return json::value_to<std::string>(content);
    }
    if (!content.is_array()) {
        return {};
    }

    std::string out;
    for (const auto& part : content.as_array()) {
        out += text_from_content_part(part);
    }
    return out;
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

void append_json_string_array(
    json::array* out,
    const std::vector<std::string>& values
) {
    for (const auto& value : values) {
        out->push_back(json::value(value));
    }
}

[[nodiscard]] json::object make_request_body(
    const LLMRuleExtractionRequest& request,
    std::string_view model,
    std::uint32_t max_tokens
) {
    json::object system_message;
    system_message["role"] = "system";
    system_message["content"] =
        "You extract conservative market-logic rules for an oracle cold path. "
        "Return only JSON. Output can only be RuleDraft objects requiring "
        "manual review. Use only these rule types: MutuallyExclusive, "
        "ExactlyOne, AtMostOne, AtLeastOne, Implies. CRITICAL DIRECTIVE: do not generate trivial "
        "single-market YES/NO constraints. Generate only combinatorial "
        "constraints spanning multiple different market_id values. Look for "
        "markets sharing the same event_id or thematic context: tournament "
        "winners, election nominees, sentencing brackets, numeric ranges, or "
        "other mutually exclusive outcomes. Use the provided market_id:outcome "
        "keys as variable_ids; asset_ids are audit context only. Do not invent "
        "variables outside those keys. Keep output compact. Do not include "
        "prose or reasoning outside the JSON object.";

    json::object user_message;
    user_message["role"] = "user";
    user_message["content"] = build_openrouter_rule_extraction_prompt(request);

    json::array messages;
    messages.push_back(std::move(system_message));
    messages.push_back(std::move(user_message));

    json::object body;
    body["model"] = std::string{model};
    body["stream"] = false;
    body["temperature"] = 0;
    body["max_tokens"] = max_tokens;
    json::object reasoning;
    reasoning["enabled"] = false;
    reasoning["exclude"] = true;
    body["reasoning"] = std::move(reasoning);
    json::object response_format;
    response_format["type"] = "json_object";
    body["response_format"] = std::move(response_format);
    body["messages"] = std::move(messages);
    return body;
}

[[nodiscard]] std::string post_openrouter_chat_completion(
    const std::string& endpoint,
    const std::string& api_key,
    const std::string& request_body,
    std::string* error
) {
    ParsedHttpsUrl url;
    if (!parse_https_url(endpoint, &url)) {
        if (error) {
            *error = "unsupported OpenRouter endpoint URL";
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

        http::request<http::string_body> request{
            http::verb::post,
            url.target,
            11
        };
        request.set(http::field::host, url.host);
        request.set(http::field::user_agent, "PolytopeOracle/0.1");
        request.set(http::field::content_type, "application/json");
        request.set(http::field::authorization, "Bearer " + api_key);
        request.set("X-Title", "Polytope Oracle");
        request.body() = request_body;
        request.prepare_payload();

        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);

        beast::error_code shutdown_error;
        stream.shutdown(shutdown_error);
        if (shutdown_error == asio::error::eof) {
            shutdown_error = {};
        }
        if (shutdown_error == ssl::error::stream_truncated) {
            shutdown_error = {};
        }
        if (shutdown_error && error) {
            *error = "TLS shutdown failed: " + shutdown_error.message();
        }

        const auto status = response.result_int();
        if (status < 200 || status >= 300) {
            if (error) {
                *error = "OpenRouter HTTP status " + std::to_string(status);
                const auto retry_after = response.find(http::field::retry_after);
                if (retry_after != response.end()) {
                    *error += "; retry_after: ";
                    *error += retry_after->value();
                }
                const auto body = truncate_for_diagnostic(response.body());
                if (!body.empty()) {
                    *error += "; body: ";
                    *error += body;
                }
            }
            return {};
        }

        return response.body();
    } catch (const std::exception& ex) {
        if (error) {
            *error = std::string{"OpenRouter request failed: "} + ex.what();
        }
        return {};
    }
}

}  // namespace

OpenRouterRuleExtractor::OpenRouterRuleExtractor()
    : OpenRouterRuleExtractor(default_llm_enabled()) {}

OpenRouterRuleExtractor::OpenRouterRuleExtractor(
    bool enabled,
    std::string api_key_env_var,
    std::string model,
    std::string endpoint,
    std::uint32_t max_tokens
)
    : enabled_(enabled),
      api_key_env_var_(std::move(api_key_env_var)),
      model_(model == kDefaultModel
                 ? env_or_default(kModelEnvVar, std::move(model))
                 : std::move(model)),
      endpoint_(std::move(endpoint)),
      max_tokens_(max_tokens == kDefaultMaxTokens
                      ? env_u32_or_default(kMaxTokensEnvVar, max_tokens)
                      : max_tokens) {}

LLMRuleExtractionResult OpenRouterRuleExtractor::extract(
    const LLMRuleExtractionRequest& request
) {
    LLMRuleExtractionResult result;

    if (!enabled_) {
        result.status = LLMExtractionStatus::Disabled;
        result.diagnostic = "OpenRouter rule extractor disabled";
        return result;
    }

    const char* api_key = std::getenv(api_key_env_var_.c_str());
    if (!api_key || *api_key == '\0') {
        result.status = LLMExtractionStatus::MissingApiKey;
        result.diagnostic = "missing API key environment variable: " +
                            api_key_env_var_;
        return result;
    }

    std::string request_error;
    const auto body = json::serialize(make_request_body(
        request,
        model_,
        max_tokens_
    ));
    const auto response_body = post_openrouter_chat_completion(
        endpoint_,
        api_key,
        body,
        &request_error
    );
    if (!request_error.empty()) {
        result.status = LLMExtractionStatus::ProviderError;
        result.diagnostic = request_error;
        return result;
    }

    result = parse_openrouter_chat_response(response_body);
    if (result.status == LLMExtractionStatus::Ok) {
        result.diagnostic = "OpenRouter rule drafts extracted";
    }
    return result;
}

const std::string& OpenRouterRuleExtractor::model() const noexcept {
    return model_;
}

const std::string& OpenRouterRuleExtractor::endpoint() const noexcept {
    return endpoint_;
}

const std::string& OpenRouterRuleExtractor::api_key_env_var() const noexcept {
    return api_key_env_var_;
}

std::uint32_t OpenRouterRuleExtractor::max_tokens() const noexcept {
    return max_tokens_;
}

std::string build_openrouter_rule_extraction_prompt(
    const LLMRuleExtractionRequest& request
) {
    std::map<std::string, std::vector<const LLMMarketContext*>> by_event;
    for (const auto& market : request.markets) {
        by_event[market.event_id].push_back(&market);
    }

    std::ostringstream out;
    out << "Instruction: " << request.instruction << "\n\n";
    out << "CRITICAL DIRECTIVE:\n";
    out << "- DO NOT generate trivial intra-market constraints such as "
           "ExactlyOne(market:Yes, market:No).\n";
    out << "- Generate ONLY COMBINATORIAL CONSTRAINTS that span multiple "
           "different market_id values.\n";
    out << "- For markets under the same event_id, inspect whether their YES "
           "outcomes are mutually exclusive or collectively exhaustive.\n";
    out << "- Use MutuallyExclusive with coverage=ExhaustiveAndExclusive and "
           "exhaustive=true for complete winner/tournament/election/candidate "
           "sets where exactly one listed market must be true.\n";
    out << "- Use MutuallyExclusive with coverage=ExclusiveOnly and "
           "exhaustive=false when only at-most-one is proven.\n";
    out << "- Use AtMostOne only for legacy exclusive-only rules.\n";
    out << "- Use ExactlyOne only when the provided markets form a complete "
           "exhaustive bracket.\n";
    out << "- Use variable_ids exactly as market_id:outcome. Asset IDs are "
           "included only for audit and must not replace variable_ids.\n";
    out << "- If no cross-market rule is directly supported, return "
           "{\"drafts\":[]}.\n\n";
    out << "Return JSON only in this exact shape:\n";
    out << "{\"drafts\":[{\"rule_id\":\"draft_rule_id\",";
    out << "\"type\":\"MutuallyExclusive\",";
    out << "\"coverage\":\"ExhaustiveAndExclusive\",";
    out << "\"exhaustive\":true,";
    out << "\"variable_ids\":[\"market_id_1:Yes\",\"market_id_2:Yes\"],";
    out << "\"source_text\":\"short quote or paraphrase\",";
    out << "\"rationale\":\"why this rule follows\",";
    out << "\"requires_manual_review\":true}]}\n\n";

    out << "Event groups:\n";
    for (const auto& [event_id, markets] : by_event) {
        out << "event_id: " << event_id << '\n';
        out << "market_count: " << markets.size() << '\n';
        for (const auto* market : markets) {
            out << "- market_id: " << market->market_id << '\n';
            out << "  title: " << market->title << '\n';
            out << "  description: " << market->description << '\n';
            out << "  resolution_source: " << market->resolution_source
                << '\n';
            out << "  variables:\n";
            for (std::size_t i = 0; i < market->outcomes.size(); ++i) {
                out << "    - variable_id: " << market->market_id << ':'
                    << market->outcomes[i] << '\n';
                if (i < market->asset_ids.size()) {
                    out << "      asset_id: " << market->asset_ids[i] << '\n';
                }
            }
        }
        out << '\n';
    }
    return out.str();
}

LLMRuleExtractionResult parse_rule_drafts_json(
    std::string_view content
) {
    LLMRuleExtractionResult result;
    auto json_text = strip_markdown_fence(content);

    boost::json::error_code error;
    auto parsed = json::parse(json_text, error);
    if (error) {
        const auto embedded_json = extract_balanced_json(json_text);
        if (!embedded_json.empty()) {
            error = {};
            parsed = json::parse(embedded_json, error);
            if (!error) {
                json_text = embedded_json;
            }
        }
        if (error) {
            result.status = LLMExtractionStatus::InvalidResponse;
            result.diagnostic =
                "LLM draft content is not valid JSON; content: " +
                truncate_for_diagnostic(content);
            return result;
        }
    }

    const json::array* drafts = nullptr;
    if (parsed.is_object()) {
        const auto& object = parsed.as_object();
        const auto it = object.find("drafts");
        if (it != object.end() && it->value().is_array()) {
            drafts = &it->value().as_array();
        }
    } else if (parsed.is_array()) {
        drafts = &parsed.as_array();
    }

    if (!drafts) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "LLM draft JSON missing drafts array";
        return result;
    }

    for (std::size_t i = 0; i < drafts->size(); ++i) {
        const auto& value = (*drafts)[i];
        if (!value.is_object()) {
            result.status = LLMExtractionStatus::InvalidResponse;
            result.diagnostic =
                "LLM draft entry " + std::to_string(i) + " is not object";
            result.drafts.clear();
            return result;
        }

        const auto& object = value.as_object();
        RuleDraft draft;
        draft.rule_id = string_field(object, "rule_id");
        draft.variable_ids = string_array_field(object, "variable_ids");
        draft.source_text = string_field(object, "source_text");
        draft.rationale = string_field(object, "rationale");
        const auto coverage_name = string_field(object, "coverage");
        if (!coverage_name.empty() &&
            !rule_coverage_from_string(coverage_name, &draft.coverage)) {
            result.status = LLMExtractionStatus::InvalidResponse;
            result.diagnostic =
                "LLM draft entry " + std::to_string(i) +
                " has unknown rule coverage";
            result.drafts.clear();
            return result;
        }
        if (bool_field(object, "exhaustive", false)) {
            draft.coverage = RuleCoverage::ExhaustiveAndExclusive;
        }
        draft.requires_manual_review =
            bool_field(object, "requires_manual_review", true);
        draft.requires_manual_review = true;

        const auto type_name = string_field(object, "type");
        if (!rule_type_from_string(type_name, &draft.type)) {
            result.status = LLMExtractionStatus::InvalidResponse;
            result.diagnostic =
                "LLM draft entry " + std::to_string(i) +
                " has unknown rule type";
            result.drafts.clear();
            return result;
        }
        if (draft.rule_id.empty() || draft.variable_ids.empty()) {
            result.status = LLMExtractionStatus::InvalidResponse;
            result.diagnostic =
                "LLM draft entry " + std::to_string(i) +
                " missing rule_id or variable_ids";
            result.drafts.clear();
            return result;
        }

        result.drafts.push_back(std::move(draft));
    }

    result.status = LLMExtractionStatus::Ok;
    result.diagnostic = "parsed LLM rule drafts";
    return result;
}

LLMRuleExtractionResult parse_openrouter_chat_response(
    std::string_view response_body
) {
    LLMRuleExtractionResult result;

    boost::json::error_code error;
    const auto parsed = json::parse(response_body, error);
    if (error || !parsed.is_object()) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "OpenRouter response is not valid JSON object";
        return result;
    }

    const auto& object = parsed.as_object();
    const auto choices_it = object.find("choices");
    if (choices_it == object.end() || !choices_it->value().is_array() ||
        choices_it->value().as_array().empty()) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "OpenRouter response missing choices";
        return result;
    }

    const auto& first_choice = choices_it->value().as_array().front();
    if (!first_choice.is_object()) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "OpenRouter choice is not object";
        return result;
    }
    const auto& choice_object = first_choice.as_object();
    const auto message_it = choice_object.find("message");
    if (message_it == choice_object.end() ||
        !message_it->value().is_object()) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "OpenRouter choice missing message";
        return result;
    }

    const auto content = message_content(message_it->value().as_object());
    if (content.empty()) {
        result.status = LLMExtractionStatus::InvalidResponse;
        result.diagnostic = "OpenRouter message content is empty; choice: " +
                            truncate_for_diagnostic(
                                json::serialize(choice_object)
                            );
        return result;
    }

    return parse_rule_drafts_json(content);
}

}  // namespace trading_engine::oracle

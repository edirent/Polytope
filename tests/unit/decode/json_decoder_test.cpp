#include "decode/json/JsonDecoder.h"
#include "decode/public/DecodeError.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::decode::DecodeErrorCode;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::JsonDecoder;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_equal(
    JsonDecodeKind actual,
    JsonDecodeKind expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void expect_equal_error(
    DecodeErrorCode actual,
    DecodeErrorCode expected,
    const std::string& field
) {
    if (actual != expected) {
        fail("mismatch: " + field);
    }
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

void JsonDecoder_ObjectPayload() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload(R"({"event_type":"book"})");

    expect_equal(result.kind, JsonDecodeKind::JsonObject, "kind");
    expect_equal(result.status, JsonDecodeKind::JsonObject, "status");
    expect_equal_error(result.error, DecodeErrorCode::None, "error");
    expect_true(result.ok(), "ok");
    expect_true(result.has_json_event_payload(), "json payload");
    expect_true(result.json.is_object(), "object");
}

void JsonDecoder_ArrayPayload() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload(R"([{"event_type":"book"}])");

    expect_equal(result.kind, JsonDecodeKind::JsonArray, "kind");
    expect_equal(result.status, JsonDecodeKind::JsonArray, "status");
    expect_equal_error(result.error, DecodeErrorCode::None, "error");
    expect_true(result.ok(), "ok");
    expect_true(result.has_json_event_payload(), "json payload");
    expect_true(result.json.is_array(), "array");
}

void JsonDecoder_PongIsControl() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload("PONG");

    expect_equal(result.kind, JsonDecodeKind::NonJsonControl, "kind");
    expect_equal(result.status, JsonDecodeKind::NonJsonControl, "status");
    expect_equal_error(
        result.error,
        DecodeErrorCode::NonJsonControl,
        "error"
    );
    expect_true(result.ok(), "ok");
    expect_true(result.has_control_payload(), "control payload");
    expect_false(result.has_json_event_payload(), "json payload");
}

void JsonDecoder_ControlIsNotMalformed() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload("  ping  ");

    expect_equal(result.kind, JsonDecodeKind::NonJsonControl, "kind");
    expect_equal_error(
        result.error,
        DecodeErrorCode::NonJsonControl,
        "error"
    );
    expect_true(result.ok(), "ok");
}

void JsonDecoder_MalformedJson() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload(R"({"event_type":)");

    expect_equal(result.kind, JsonDecodeKind::MalformedJson, "kind");
    expect_equal_error(
        result.error,
        DecodeErrorCode::MalformedJson,
        "error"
    );
    expect_false(result.ok(), "ok");
}

void JsonDecoder_UnsupportedScalarJson() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload("123");

    expect_equal(result.kind, JsonDecodeKind::UnsupportedJson, "kind");
    expect_equal_error(
        result.error,
        DecodeErrorCode::UnsupportedJson,
        "error"
    );
    expect_false(result.ok(), "ok");
    expect_true(result.json.is_int64() || result.json.is_uint64(), "number");
}

void JsonDecoder_EmptyPayload() {
    JsonDecoder decoder;

    const auto result = decoder.decode_payload("   ");

    expect_equal(result.kind, JsonDecodeKind::MalformedJson, "kind");
    expect_equal_error(result.error, DecodeErrorCode::EmptyPayload, "error");
    expect_false(result.ok(), "ok");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"JsonDecoder_ObjectPayload", &JsonDecoder_ObjectPayload},
        {"JsonDecoder_ArrayPayload", &JsonDecoder_ArrayPayload},
        {"JsonDecoder_PongIsControl", &JsonDecoder_PongIsControl},
        {"JsonDecoder_ControlIsNotMalformed", &JsonDecoder_ControlIsNotMalformed},
        {"JsonDecoder_MalformedJson", &JsonDecoder_MalformedJson},
        {"JsonDecoder_UnsupportedScalarJson", &JsonDecoder_UnsupportedScalarJson},
        {"JsonDecoder_EmptyPayload", &JsonDecoder_EmptyPayload}
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

#include "engine/execution/adapter/LiveOrderBridge.h"

#include <openssl/evp.h>

#if HAVE_LIBSECP256K1
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace execution = trading_engine::execution;
using Clock = std::chrono::steady_clock;

struct Config {
    std::uint64_t iterations = 10'000;
    std::uint64_t warmup = 1'000;
    std::uint64_t threshold_us = 100;
    std::string private_key_env{"POLYMARKET_WALLET_PRIVATE_KEY"};
    std::string asset_id;
    std::filesystem::path out_json;
};

struct LatencyStats {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()
    );
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
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
                out.push_back(
                    static_cast<unsigned char>(c) < 0x20 ? ' ' : c
                );
                break;
        }
    }
    return out;
}

Config parse_args(int argc, char** argv) {
    Config config;
    if (const char* asset = std::getenv("POLYMARKET_ASSET_ID")) {
        config.asset_id = asset;
    }

    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto value = [&](const char* option) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + option);
            }
            return argv[++index];
        };

        if (arg == "--iterations") {
            config.iterations = std::stoull(value("--iterations"));
        } else if (arg == "--warmup") {
            config.warmup = std::stoull(value("--warmup"));
        } else if (arg == "--threshold-us") {
            config.threshold_us = std::stoull(value("--threshold-us"));
        } else if (arg == "--private-key-env") {
            config.private_key_env = value("--private-key-env");
        } else if (arg == "--asset-id") {
            config.asset_id = value("--asset-id");
        } else if (arg == "--out-json") {
            config.out_json = value("--out-json");
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: profile_live_order_signer "
                << "[--iterations 10000] [--warmup 1000] "
                << "[--threshold-us 100] "
                << "[--private-key-env POLYMARKET_WALLET_PRIVATE_KEY]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (config.iterations == 0) {
        fail("--iterations must be greater than zero");
    }
    return config;
}

std::optional<unsigned char> hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned char>(10 + c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned char>(10 + c - 'A');
    }
    return std::nullopt;
}

std::array<unsigned char, 32> parse_private_key(const std::string& raw) {
    std::string value = raw;
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value = value.substr(2);
    }
    if (value.size() != 64) {
        fail("private key must be 32-byte hex");
    }

    std::array<unsigned char, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto hi = hex_nibble(value[i * 2]);
        const auto lo = hex_nibble(value[i * 2 + 1]);
        if (!hi || !lo) {
            fail("private key contains non-hex characters");
        }
        out[i] = static_cast<unsigned char>((*hi << 4U) | *lo);
    }
    return out;
}

std::string hex_encode(const unsigned char* bytes, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4U]);
        out.push_back(kHex[bytes[i] & 0x0fU]);
    }
    return out;
}

std::array<unsigned char, 32> request_digest(
    const execution::LiveOrderRequest& request
) {
    std::string payload;
    payload.reserve(256 + request.client_order_id.size() +
                    request.market_id.size() + request.asset_id.size());
    payload += "PolytopeLiveOrderHash:";
    payload += request.client_order_id;
    payload += ':';
    payload += request.market_id;
    payload += ':';
    payload += request.asset_id;
    payload += ':';
    payload += std::to_string(static_cast<int>(request.side));
    payload += ':';
    payload += std::to_string(request.quantity_lots);
    payload += ':';
    payload += std::to_string(request.price_tick);
    payload += ':';
    payload += std::to_string(request.created_ts_ns);
    payload += ':';
    payload += std::to_string(request.expire_after_ns);

    std::array<unsigned char, 32> digest{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        fail("EVP_MD_CTX_new failed");
    }
    const auto cleanup = [&]() {
        EVP_MD_CTX_free(ctx);
    };

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, payload.data(), payload.size()) != 1) {
        cleanup();
        fail("EVP sha256 failed");
    }
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1 ||
        digest_len != digest.size()) {
        cleanup();
        fail("EVP sha256 final failed");
    }
    cleanup();
    return digest;
}

#if HAVE_LIBSECP256K1
class LibSecp256k1OrderSigner final : public execution::ILiveOrderSigner {
public:
    explicit LibSecp256k1OrderSigner(std::array<unsigned char, 32> private_key)
        : private_key_(private_key),
          context_(secp256k1_context_create(SECP256K1_CONTEXT_SIGN)) {
        if (context_ == nullptr) {
            fail("secp256k1_context_create failed");
        }
        if (secp256k1_ec_seckey_verify(context_, private_key_.data()) != 1) {
            fail("invalid secp256k1 private key");
        }
    }

    ~LibSecp256k1OrderSigner() override {
        if (context_ != nullptr) {
            secp256k1_context_destroy(context_);
        }
        private_key_.fill(0);
    }

    [[nodiscard]] execution::LiveOrderSignResult sign_order(
        const execution::LiveOrderRequest& request
    ) override {
        const auto digest = request_digest(request);
        secp256k1_ecdsa_recoverable_signature signature;
        if (secp256k1_ecdsa_sign_recoverable(
                context_,
                &signature,
                digest.data(),
                private_key_.data(),
                nullptr,
                nullptr
            ) != 1) {
            return {.ok = false, .error = "secp256k1_ecdsa_sign failed"};
        }

        std::array<unsigned char, 64> compact{};
        int recovery_id = 0;
        secp256k1_ecdsa_recoverable_signature_serialize_compact(
            context_,
            compact.data(),
            &recovery_id,
            &signature
        );

        const auto body =
            std::string{"{\"discarded\":true,\"client_order_id\":\""} +
            json_escape(request.client_order_id) +
            "\",\"digest\":\"0x" +
            hex_encode(digest.data(), digest.size()) +
            "\",\"signature\":\"0x" +
            hex_encode(compact.data(), compact.size()) +
            "\",\"recovery_id\":" +
            std::to_string(recovery_id) +
            "}";

        return {
            .ok = true,
            .order = execution::SignedLiveOrder{
                .request_body_json = body,
                .venue_order_id_hint = {}
            }
        };
    }

private:
    std::array<unsigned char, 32> private_key_{};
    secp256k1_context* context_ = nullptr;
};
#endif

LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();
    auto percentile = [&](double p) -> std::uint64_t {
        const auto index = static_cast<std::size_t>(
            p * static_cast<double>(values.size() - 1) + 0.5
        );
        return values[std::min(index, values.size() - 1)];
    };
    stats.p50 = percentile(0.50);
    stats.p90 = percentile(0.90);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    long double total = 0.0;
    for (const auto value : values) {
        total += static_cast<long double>(value);
    }
    stats.mean = static_cast<double>(total / values.size());
    return stats;
}

execution::LiveOrderRequest make_request(
    std::uint64_t index,
    const std::string& asset_id
) {
    execution::LiveOrderRequest request;
    request.parent_id = 1;
    request.child_id = index + 1;
    request.client_order_id = "profile-" + std::to_string(index + 1);
    request.market_id = "world-cup";
    request.asset_id = asset_id.empty() ? "asset" : asset_id;
    request.side = (index % 2 == 0)
        ? execution::OrderSide::Buy
        : execution::OrderSide::Sell;
    request.quantity_lots = 9;
    request.price_tick = (index % 2 == 0) ? 475'000 : 525'000;
    request.created_ts_ns = 1'000'000'000ULL + index;
    request.expire_after_ns = request.created_ts_ns + 5'000'000'000ULL;
    request.order_type = "GTC";
    request.post_only = true;
    return request;
}

void write_json_report(
    const Config& config,
    const LatencyStats& stats,
    bool passed
) {
    if (config.out_json.empty()) {
        return;
    }
    if (!config.out_json.parent_path().empty()) {
        std::filesystem::create_directories(config.out_json.parent_path());
    }
    std::ofstream out(config.out_json);
    if (!out) {
        fail("failed to open --out-json: " + config.out_json.string());
    }
    out << "{\n"
        << "  \"signer\": \"libsecp256k1_ecdsa_recoverable_compact\",\n"
        << "  \"full_eip712_order_struct_v2\": false,\n"
        << "  \"iterations\": " << config.iterations << ",\n"
        << "  \"warmup\": " << config.warmup << ",\n"
        << "  \"threshold_us\": " << config.threshold_us << ",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"latency_ns\": {\n"
        << "    \"count\": " << stats.count << ",\n"
        << "    \"min\": " << stats.min << ",\n"
        << "    \"p50\": " << stats.p50 << ",\n"
        << "    \"p90\": " << stats.p90 << ",\n"
        << "    \"p95\": " << stats.p95 << ",\n"
        << "    \"p99\": " << stats.p99 << ",\n"
        << "    \"max\": " << stats.max << ",\n"
        << "    \"mean\": " << std::fixed << std::setprecision(2)
        << stats.mean << "\n"
        << "  }\n"
        << "}\n";
}

int run(int argc, char** argv) {
    const auto config = parse_args(argc, argv);

#if !HAVE_LIBSECP256K1
    (void)config;
    std::cerr
        << "libsecp256k1 is not available at build time; "
        << "install secp256k1.h/libsecp256k1 and rerun cmake\n";
    return 2;
#else
    const char* raw_private_key = std::getenv(config.private_key_env.c_str());
    if (raw_private_key == nullptr || raw_private_key[0] == '\0') {
        std::cerr << "missing private key env: " << config.private_key_env
                  << '\n';
        return 2;
    }

    LibSecp256k1OrderSigner signer(parse_private_key(raw_private_key));
    std::vector<std::uint64_t> measured_ns;
    measured_ns.reserve(static_cast<std::size_t>(config.iterations));

    const auto total = config.warmup + config.iterations;
    for (std::uint64_t i = 0; i < total; ++i) {
        const auto request = make_request(i, config.asset_id);
        const auto started = Clock::now();
        const auto signed_order = signer.sign_order(request);
        const auto stopped = Clock::now();
        if (!signed_order.ok) {
            fail(signed_order.error.empty() ? "signing failed"
                                            : signed_order.error);
        }
        if (i >= config.warmup) {
            measured_ns.push_back(elapsed_ns(started, stopped));
        }
    }

    const auto stats = summarize(std::move(measured_ns));
    const auto passed = stats.p99 <= config.threshold_us * 1'000ULL;
    write_json_report(config, stats, passed);

    std::cout << "live_order_signer_profile:\n"
              << "  signer: libsecp256k1_ecdsa_recoverable_compact\n"
              << "  full_eip712_order_struct_v2: false\n"
              << "  iterations: " << config.iterations << "\n"
              << "  warmup: " << config.warmup << "\n"
              << "  threshold_us: " << config.threshold_us << "\n"
              << "  passed: " << (passed ? "true" : "false") << "\n"
              << "  latency_ns:\n"
              << "    count: " << stats.count << "\n"
              << "    min: " << stats.min << "\n"
              << "    p50: " << stats.p50 << "\n"
              << "    p90: " << stats.p90 << "\n"
              << "    p95: " << stats.p95 << "\n"
              << "    p99: " << stats.p99 << "\n"
              << "    max: " << stats.max << "\n"
              << "    mean: " << std::fixed << std::setprecision(2)
              << stats.mean << "\n";
    if (!config.out_json.empty()) {
        std::cout << "  out_json: " << config.out_json << "\n";
    }
    return passed ? 0 : 3;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "live_order_signer_profile_error: " << error.what()
                  << '\n';
        return 1;
    }
}

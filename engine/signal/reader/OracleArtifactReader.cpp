#include "engine/signal/reader/OracleArtifactReader.h"

#include "oracle/artifact/ArtifactLoader.h"
#include "oracle/artifact/ArtifactLayout.h"
#include "oracle/bundles/BundleHash.h"

#include <boost/json.hpp>

#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace trading_engine::signal {

namespace {

namespace json = boost::json;

constexpr std::uint32_t kSupportedArtifactVersion = 1;

[[nodiscard]] std::uint64_t parse_hex_u64(std::string_view text) noexcept {
    std::uint64_t value = 0;
    for (const char ch : text) {
        value <<= 4U;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<std::uint64_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<std::uint64_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<std::uint64_t>(ch - 'A' + 10);
        } else {
            return 0;
        }
    }
    return value;
}

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool read_u8(std::uint8_t* out) {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        *out = static_cast<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t* out) {
        if (offset_ + 4 > bytes_.size()) {
            return false;
        }
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                         static_cast<std::uint8_t>(bytes_[offset_++])
                     )
                     << shift;
        }
        *out = value;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t* out) {
        if (offset_ + 8 > bytes_.size()) {
            return false;
        }
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(
                         static_cast<std::uint8_t>(bytes_[offset_++])
                     )
                     << shift;
        }
        *out = value;
        return true;
    }

    [[nodiscard]] bool read_i64(std::int64_t* out) {
        std::uint64_t value = 0;
        if (!read_u64(&value)) {
            return false;
        }
        *out = static_cast<std::int64_t>(value);
        return true;
    }

    [[nodiscard]] bool read_string(std::string* out) {
        std::uint32_t size = 0;
        if (!read_u32(&size)) {
            return false;
        }
        if (offset_ + size > bytes_.size()) {
            return false;
        }
        out->assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            size
        );
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool consumed() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

[[nodiscard]] std::optional<CandidateBundle> read_binary_bundle(
    ByteReader* reader,
    std::string* error
) {
    CandidateBundle bundle;
    std::uint32_t leg_count = 0;

    if (!reader->read_u64(&bundle.bundle_id) ||
        !reader->read_u64(&bundle.required_true_mask) ||
        !reader->read_u64(&bundle.required_false_mask) ||
        !reader->read_u64(&bundle.invalid_mask) ||
        !reader->read_i64(&bundle.guaranteed_payout_tick) ||
        !reader->read_u32(&leg_count) ||
        !reader->read_i64(&bundle.min_edge_tick)) {
        *error = "truncated candidate_bundles.bin";
        return std::nullopt;
    }

    if (leg_count > trading_engine::oracle::kMaxBundleLegs ||
        leg_count > std::numeric_limits<std::uint16_t>::max()) {
        *error = "candidate bundle leg_count exceeds 16";
        return std::nullopt;
    }
    bundle.leg_count = static_cast<std::uint16_t>(leg_count);

    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        auto& leg = bundle.legs[i];
        std::uint8_t side = 0;
        if (!reader->read_string(&leg.market_id) ||
            !reader->read_string(&leg.asset_id) ||
            !reader->read_u8(&side) ||
            !reader->read_i64(&leg.quantity_lots) ||
            !reader->read_i64(&leg.max_price_tick)) {
            *error = "truncated candidate bundle leg";
            return std::nullopt;
        }
        if (side > static_cast<std::uint8_t>(trading_engine::oracle::Side::Sell)) {
            *error = "invalid candidate bundle side";
            return std::nullopt;
        }
        leg.side = static_cast<trading_engine::oracle::Side>(side);
    }

    return bundle;
}

[[nodiscard]] std::vector<CandidateBundle> parse_binary_bundles(
    std::span<const std::byte> bytes,
    std::string* error
) {
    ByteReader reader(bytes);
    std::uint32_t count = 0;
    if (!reader.read_u32(&count)) {
        *error = "truncated candidate_bundles.bin header";
        return {};
    }

    std::vector<CandidateBundle> bundles;
    bundles.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto bundle = read_binary_bundle(&reader, error);
        if (!bundle) {
            return {};
        }
        bundles.push_back(std::move(*bundle));
    }

    if (!reader.consumed()) {
        *error = "candidate_bundles.bin has trailing bytes";
        return {};
    }
    return bundles;
}

std::string string_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

std::uint64_t u64_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
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

std::int64_t i64_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0;
    }
    if (it->value().is_int64()) {
        return it->value().as_int64();
    }
    if (it->value().is_uint64() &&
        it->value().as_uint64() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(it->value().as_uint64());
    }
    return 0;
}

[[nodiscard]] std::optional<CandidateBundle> parse_json_bundle(
    const json::object& object,
    std::string* error
) {
    CandidateBundle bundle;
    bundle.bundle_id = u64_field(object, "bundle_id");
    bundle.required_true_mask = u64_field(object, "required_true_mask");
    bundle.required_false_mask = u64_field(object, "required_false_mask");
    bundle.invalid_mask = u64_field(object, "invalid_mask");
    bundle.guaranteed_payout_tick = i64_field(object, "guaranteed_payout_tick");
    bundle.min_edge_tick = i64_field(object, "min_edge_tick");

    const auto legs_it = object.find("legs");
    if (legs_it == object.end() || !legs_it->value().is_array()) {
        *error = "candidate bundle missing legs";
        return std::nullopt;
    }
    const auto& legs = legs_it->value().as_array();
    if (legs.size() > trading_engine::oracle::kMaxBundleLegs) {
        *error = "candidate bundle leg_count exceeds 16";
        return std::nullopt;
    }
    bundle.leg_count = static_cast<std::uint16_t>(legs.size());
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        if (!legs[i].is_object()) {
            *error = "candidate bundle leg is not object";
            return std::nullopt;
        }
        const auto& leg_object = legs[i].as_object();
        auto& leg = bundle.legs[i];
        leg.market_id = string_field(leg_object, "market_id");
        leg.asset_id = string_field(leg_object, "asset_id");
        leg.quantity_lots = i64_field(leg_object, "quantity_lots");
        leg.max_price_tick = i64_field(leg_object, "max_price_tick");

        const auto side_text = string_field(leg_object, "side");
        if (!trading_engine::oracle::side_from_string(side_text, &leg.side)) {
            *error = "candidate bundle has invalid side";
            return std::nullopt;
        }
    }

    return bundle;
}

[[nodiscard]] std::vector<CandidateBundle> parse_json_bundles(
    std::string_view content,
    std::string* error
) {
    boost::json::error_code parse_error;
    const auto parsed = json::parse(content, parse_error);
    if (parse_error || !parsed.is_object()) {
        *error = "malformed candidate bundle JSON";
        return {};
    }

    const auto& root = parsed.as_object();
    const auto bundles_it = root.find("bundles");
    if (bundles_it == root.end() || !bundles_it->value().is_array()) {
        *error = "candidate bundle JSON missing bundles";
        return {};
    }

    std::vector<CandidateBundle> bundles;
    for (const auto& value : bundles_it->value().as_array()) {
        if (!value.is_object()) {
            *error = "candidate bundle entry is not object";
            return {};
        }
        auto bundle = parse_json_bundle(value.as_object(), error);
        if (!bundle) {
            return {};
        }
        bundles.push_back(std::move(*bundle));
    }
    return bundles;
}

[[nodiscard]] std::vector<CandidateBundle> parse_bundles(
    const std::vector<std::byte>& bytes,
    std::string* error
) {
    if (bytes.empty()) {
        return {};
    }

    const auto first = static_cast<unsigned char>(bytes.front());
    if (first == '{' || first == '[') {
        return parse_json_bundles(
            std::string_view{
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()
            },
            error
        );
    }

    return parse_binary_bundles(bytes, error);
}

[[nodiscard]] bool validate_bundles(
    const std::vector<CandidateBundle>& bundles,
    std::string* error
) {
    std::unordered_set<std::uint64_t> seen_bundle_ids;
    for (const auto& bundle : bundles) {
        if (bundle.leg_count > trading_engine::oracle::kMaxBundleLegs) {
            *error = "candidate bundle leg_count exceeds 16";
            return false;
        }
        const auto [_, inserted] = seen_bundle_ids.insert(bundle.bundle_id);
        if (!inserted) {
            *error = "duplicate candidate bundle id";
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<BundleRuntimePlan> build_runtime_plans(
    const std::vector<CandidateBundle>& bundles,
    std::uint64_t artifact_hash,
    std::uint64_t constraint_hash
) {
    std::unordered_map<std::string, std::uint32_t> asset_indices;
    SideResolver side_resolver;
    std::vector<BundleRuntimePlan> plans;
    plans.reserve(bundles.size());

    for (const auto& bundle : bundles) {
        BundleRuntimePlan plan;
        plan.bundle = &bundle;
        plan.bundle_id = bundle.bundle_id;
        plan.bundle_hash =
            trading_engine::oracle::hash_candidate_bundle(bundle);
        plan.oracle_artifact_hash = artifact_hash;
        plan.constraint_hash = constraint_hash;
        plan.leg_count = std::min<std::uint16_t>(
            bundle.leg_count,
            kMaxIntentLegs
        );
        plan.guaranteed_payout_tick = bundle.guaranteed_payout_tick;
        plan.min_unit_edge_tick = bundle.min_edge_tick;
        plan.min_total_edge_tick = bundle.min_edge_tick;

        for (std::uint16_t i = 0; i < plan.leg_count; ++i) {
            const auto& leg = bundle.legs[i];
            plan.market_ids[i] = &leg.market_id;
            plan.asset_ids[i] = &leg.asset_id;
            plan.sides[i] = leg.side;
            plan.executable_sides[i] = side_resolver.resolve(leg.side);
            plan.ratio_qty_lots[i] = leg.quantity_lots;
            plan.max_price_ticks[i] = leg.max_price_tick;

            auto [it, inserted] = asset_indices.emplace(
                leg.asset_id,
                static_cast<std::uint32_t>(asset_indices.size())
            );
            plan.asset_indices[i] = it->second;

            bool already_unique = false;
            for (std::uint16_t j = 0; j < plan.unique_asset_count; ++j) {
                if (plan.unique_asset_indices[j] == it->second) {
                    already_unique = true;
                    break;
                }
            }
            if (!already_unique && plan.unique_asset_count < kMaxIntentLegs) {
                plan.unique_asset_ids[plan.unique_asset_count] = &leg.asset_id;
                plan.unique_asset_indices[plan.unique_asset_count] = it->second;
                ++plan.unique_asset_count;
            }
        }

        plans.push_back(plan);
    }

    return plans;
}

}  // namespace

OracleLoadResult OracleArtifactReader::load(
    const std::filesystem::path& artifact_dir
) {
    bundles_.clear();
    runtime_plans_.clear();
    artifact_version_ = 0;
    artifact_hash_ = 0;
    constraint_hash_ = 0;
    bundle_hash_ = 0;

    OracleLoadResult result;

    trading_engine::oracle::ArtifactLoader loader;
    const auto artifact = loader.load(artifact_dir);
    if (!artifact.ok()) {
        result.error = artifact.errors.empty()
                           ? "artifact checksum validation failed"
                           : artifact.errors.front();
        return result;
    }

    result.artifact_version = artifact.contents.manifest.artifact_version;
    if (artifact.contents.manifest.artifact_version != kSupportedArtifactVersion) {
        result.error = "unsupported oracle artifact version";
        return result;
    }

    std::string parse_error;
    auto parsed_bundles = parse_bundles(
        artifact.contents.candidate_bundles_bin,
        &parse_error
    );
    if (!parse_error.empty()) {
        result.error = parse_error;
        return result;
    }

    if (parsed_bundles.size() != artifact.contents.manifest.bundle_count) {
        result.error = "candidate bundle count does not match manifest";
        return result;
    }
    if (!validate_bundles(parsed_bundles, &result.error)) {
        return result;
    }

    bundles_ = std::move(parsed_bundles);
    artifact_version_ = artifact.contents.manifest.artifact_version;
    const auto manifest_checksum =
        artifact.checksums.find(std::string{trading_engine::oracle::kManifestFile});
    if (manifest_checksum != artifact.checksums.end()) {
        artifact_hash_ = parse_hex_u64(manifest_checksum->second);
    }
    constraint_hash_ =
        parse_hex_u64(artifact.contents.manifest.constraint_hash);
    bundle_hash_ = trading_engine::oracle::hash_candidate_bundles(bundles_);
    runtime_plans_ = build_runtime_plans(
        bundles_,
        artifact_hash_,
        constraint_hash_
    );

    result.ok = true;
    result.bundle_count = static_cast<std::uint64_t>(bundles_.size());
    return result;
}

std::span<const CandidateBundle> OracleArtifactReader::active_bundles() const {
    return bundles_;
}

std::span<const BundleRuntimePlan> OracleArtifactReader::active_runtime_plans(
) const {
    return runtime_plans_;
}

std::uint64_t OracleArtifactReader::artifact_version() const {
    return artifact_version_;
}

std::uint64_t OracleArtifactReader::artifact_hash() const {
    return artifact_hash_;
}

std::uint64_t OracleArtifactReader::constraint_hash() const {
    return constraint_hash_;
}

std::uint64_t OracleArtifactReader::bundle_hash() const {
    return bundle_hash_;
}

}  // namespace trading_engine::signal

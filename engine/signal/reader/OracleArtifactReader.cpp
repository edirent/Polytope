#include "engine/signal/reader/OracleArtifactReader.h"

#include "oracle/artifact/ArtifactLoader.h"
#include "oracle/bundles/BundleHash.h"

#include <boost/json.hpp>

#include <limits>
#include <optional>
#include <unordered_set>

namespace trading_engine::signal {

namespace {

namespace json = boost::json;

constexpr std::uint32_t kSupportedArtifactVersion = 1;

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

}  // namespace

OracleLoadResult OracleArtifactReader::load(
    const std::filesystem::path& artifact_dir
) {
    bundles_.clear();
    artifact_version_ = 0;
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
    bundle_hash_ = trading_engine::oracle::hash_candidate_bundles(bundles_);

    result.ok = true;
    result.bundle_count = static_cast<std::uint64_t>(bundles_.size());
    return result;
}

std::span<const CandidateBundle> OracleArtifactReader::active_bundles() const {
    return bundles_;
}

std::uint64_t OracleArtifactReader::artifact_version() const {
    return artifact_version_;
}

std::uint64_t OracleArtifactReader::bundle_hash() const {
    return bundle_hash_;
}

}  // namespace trading_engine::signal

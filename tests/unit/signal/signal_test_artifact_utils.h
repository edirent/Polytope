#pragma once

#include "oracle/artifact/ArtifactExporter.h"
#include "oracle/public/CandidateBundle.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace signal_test {

using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::OracleArtifactContents;
using trading_engine::oracle::Side;

inline void append_u8(std::vector<std::byte>* out, std::uint8_t value) {
    out->push_back(static_cast<std::byte>(value));
}

inline void append_u32(std::vector<std::byte>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void append_u64(std::vector<std::byte>* out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void append_i64(std::vector<std::byte>* out, std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

inline void append_string(std::vector<std::byte>* out, const std::string& value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) {
        append_u8(out, byte);
    }
}

inline CandidateBundle make_bundle(
    std::uint64_t bundle_id,
    const std::vector<std::string>& asset_ids
) {
    CandidateBundle out;
    out.bundle_id = bundle_id;
    out.required_true_mask = 1;
    out.guaranteed_payout_tick = 1'000'000;
    out.min_edge_tick = 0;
    out.leg_count = static_cast<std::uint16_t>(asset_ids.size());
    for (std::uint16_t i = 0; i < out.leg_count; ++i) {
        out.legs[i] = BundleLeg{
            .market_id = "m" + std::to_string(bundle_id),
            .asset_id = asset_ids[i],
            .side = Side::Buy,
            .quantity_lots = 1,
            .max_price_tick = 500000
        };
    }
    return out;
}

inline std::vector<std::byte> serialize_bundles(
    const std::vector<CandidateBundle>& bundles
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(bundles.size()));
    for (const auto& candidate : bundles) {
        append_u64(&out, candidate.bundle_id);
        append_u64(&out, candidate.required_true_mask);
        append_u64(&out, candidate.required_false_mask);
        append_u64(&out, candidate.invalid_mask);
        append_i64(&out, candidate.guaranteed_payout_tick);
        append_u32(&out, candidate.leg_count);
        append_i64(&out, candidate.min_edge_tick);
        for (std::uint16_t i = 0; i < candidate.leg_count; ++i) {
            const auto& leg = candidate.legs[i];
            append_string(&out, leg.market_id);
            append_string(&out, leg.asset_id);
            append_u8(&out, static_cast<std::uint8_t>(leg.side));
            append_i64(&out, leg.quantity_lots);
            append_i64(&out, leg.max_price_tick);
        }
    }
    return out;
}

inline std::filesystem::path test_root(const std::string& name) {
    const auto root =
        std::filesystem::temp_directory_path() / ("signal_scan_" + name);
    std::filesystem::remove_all(root);
    return root;
}

inline std::filesystem::path export_artifact(
    const std::string& name,
    const std::vector<CandidateBundle>& bundles
) {
    OracleArtifactContents contents;
    contents.manifest.artifact_version = 1;
    contents.manifest.created_at_ns = 1;
    contents.manifest.market_count = 2;
    contents.manifest.asset_count = 4;
    contents.manifest.variable_count = 4;
    contents.manifest.rule_count = 1;
    contents.manifest.constraint_count = 1;
    contents.manifest.feasible_state_count = 2;
    contents.manifest.bundle_count =
        static_cast<std::uint64_t>(bundles.size());
    contents.manifest.llm_provider = "none";
    contents.market_universe_json = "{\"markets\":[\"m1\",\"m2\"]}\n";
    contents.rulebook_json = "{\"rules\":[\"r1\"]}\n";
    contents.variables_bin = {std::byte{1}};
    contents.constraints_bin = {std::byte{2}};
    contents.feasible_states_bin = {std::byte{3}};
    contents.payoff_matrix_bin = {std::byte{4}};
    contents.candidate_bundles_bin = serialize_bundles(bundles);
    contents.market_dependency_graph_bin = {};
    contents.settlement_bitmask_bin = {};

    trading_engine::oracle::ArtifactExporter exporter;
    const auto result = exporter.export_artifact(
        test_root(name),
        "artifact",
        contents
    );
    if (!result.ok()) {
        return {};
    }
    return result.artifact_dir;
}

}  // namespace signal_test

#include "oracle/payoff/PayoffMatrixBuilder.h"

#include "oracle/payoff/PayoutRule.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace trading_engine::oracle {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct AssetColumn {
    std::uint32_t var_id = 0;
    std::string asset_id;
};

void hash_byte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

void hash_u32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (int shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value));
}

void hash_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (unsigned char c : value) {
        hash_byte(hash, c);
    }
    hash_byte(hash, 0xffU);
}

bool bit_is_set(
    const FeasibleState& state,
    std::uint32_t var_id
) noexcept {
    const std::uint32_t word_index = var_id / 64U;
    const std::uint32_t bit_index = var_id % 64U;
    if (word_index >= state.bitset_words.size()) {
        return false;
    }
    return ((state.bitset_words[word_index] >> bit_index) & 1ULL) != 0;
}

std::vector<AssetColumn> build_columns(
    const std::vector<BooleanVariable>& variables,
    std::vector<std::string>* errors
) {
    std::vector<AssetColumn> columns;
    std::unordered_set<std::string> seen_assets;

    for (const auto& variable : variables) {
        if (variable.asset_id.empty()) {
            errors->push_back(
                "variable " + variable.variable_key + " missing asset_id"
            );
            continue;
        }

        const auto [_, inserted] = seen_assets.insert(variable.asset_id);
        if (!inserted) {
            errors->push_back("duplicate asset_id: " + variable.asset_id);
            continue;
        }

        columns.push_back(AssetColumn{variable.var_id, variable.asset_id});
    }

    std::sort(
        columns.begin(),
        columns.end(),
        [](const AssetColumn& lhs, const AssetColumn& rhs) {
            return lhs.asset_id < rhs.asset_id;
        }
    );

    return columns;
}

template <typename T>
void write_pod(std::ofstream& output, const T& value) {
    output.write(
        reinterpret_cast<const char*>(&value),
        static_cast<std::streamsize>(sizeof(T))
    );
}

void write_string(std::ofstream& output, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    write_pod(output, size);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool write_payoff_bin(
    const std::filesystem::path& path,
    const PayoffMatrix& matrix,
    std::vector<std::string>* errors
) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        errors->push_back("failed to open payoff_matrix.bin");
        return false;
    }

    write_pod(output, matrix.row_count);
    write_pod(output, matrix.column_count);
    write_pod(output, matrix.payoff_hash);

    const auto asset_count = static_cast<std::uint32_t>(matrix.asset_ids.size());
    write_pod(output, asset_count);
    for (const auto& asset_id : matrix.asset_ids) {
        write_string(output, asset_id);
    }

    const auto state_count = static_cast<std::uint32_t>(matrix.state_ids.size());
    write_pod(output, state_count);
    for (const auto state_id : matrix.state_ids) {
        write_pod(output, state_id);
    }

    const auto entry_count = static_cast<std::uint64_t>(matrix.entries.size());
    write_pod(output, entry_count);
    for (const auto& entry : matrix.entries) {
        write_pod(output, entry.state_id);
        write_pod(output, entry.asset_index);
        write_pod(output, entry.payout_tick);
    }

    return output.good();
}

bool write_debug_json(
    const std::filesystem::path& path,
    const PayoffMatrix& matrix,
    std::vector<std::string>* errors
) {
    std::ofstream output(path);
    if (!output) {
        errors->push_back("failed to open payoff_matrix.debug.json");
        return false;
    }

    output << "{\n";
    output << "  \"row_count\": " << matrix.row_count << ",\n";
    output << "  \"column_count\": " << matrix.column_count << ",\n";
    output << "  \"payoff_hash\": " << matrix.payoff_hash << ",\n";
    output << "  \"asset_ids\": [";
    for (std::size_t i = 0; i < matrix.asset_ids.size(); ++i) {
        output << "\"" << matrix.asset_ids[i] << "\"";
        if (i + 1 != matrix.asset_ids.size()) {
            output << ", ";
        }
    }
    output << "],\n";
    output << "  \"entries\": [\n";
    for (std::size_t i = 0; i < matrix.entries.size(); ++i) {
        const auto& entry = matrix.entries[i];
        output << "    {\"state_id\": " << entry.state_id
               << ", \"asset_index\": " << entry.asset_index
               << ", \"payout_tick\": " << entry.payout_tick << "}";
        output << (i + 1 == matrix.entries.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";

    return output.good();
}

}  // namespace

PayoffMatrixBuildResult PayoffMatrixBuilder::build(
    const std::vector<BooleanVariable>& variables,
    const std::vector<FeasibleState>& feasible_states
) const {
    PayoffMatrixBuildResult result;
    const auto columns = build_columns(variables, &result.errors);
    if (!result.errors.empty()) {
        return result;
    }

    result.matrix.row_count =
        static_cast<std::uint32_t>(feasible_states.size());
    result.matrix.column_count = static_cast<std::uint32_t>(columns.size());

    for (const auto& column : columns) {
        result.matrix.asset_ids.push_back(column.asset_id);
    }

    for (const auto& state : feasible_states) {
        result.matrix.state_ids.push_back(state.state_id);
        for (std::uint32_t asset_index = 0;
             asset_index < columns.size();
             ++asset_index) {
            const auto& column = columns[asset_index];
            result.matrix.entries.push_back(PayoffEntry{
                .state_id = state.state_id,
                .asset_index = asset_index,
                .payout_tick = bit_is_set(state, column.var_id)
                                   ? PAYOUT_ONE_TICK
                                   : 0
            });
        }
    }

    result.matrix.payoff_hash = hash_payoff_matrix(result.matrix);
    return result;
}

bool PayoffMatrixBuilder::write(
    const PayoffMatrix& matrix,
    const std::string& directory,
    std::vector<std::string>* errors
) const {
    std::vector<std::string> local_errors;
    auto* out_errors = errors ? errors : &local_errors;

    const std::filesystem::path dir(directory);
    std::filesystem::create_directories(dir);

    bool ok = true;
    ok &= write_payoff_bin(dir / "payoff_matrix.bin", matrix, out_errors);
    ok &= write_debug_json(
        dir / "payoff_matrix.debug.json",
        matrix,
        out_errors
    );
    return ok;
}

std::uint64_t hash_payoff_matrix(const PayoffMatrix& matrix) noexcept {
    std::uint64_t hash = kFnvOffset;

    hash_u32(&hash, matrix.row_count);
    hash_u32(&hash, matrix.column_count);

    for (const auto& asset_id : matrix.asset_ids) {
        hash_string(&hash, asset_id);
    }
    for (const auto state_id : matrix.state_ids) {
        hash_u64(&hash, state_id);
    }
    for (const auto& entry : matrix.entries) {
        hash_u64(&hash, entry.state_id);
        hash_u32(&hash, entry.asset_index);
        hash_i64(&hash, entry.payout_tick);
    }

    return hash;
}

}  // namespace trading_engine::oracle

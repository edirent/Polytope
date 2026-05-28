#include "oracle/compiler/MatrixBuilder.h"

#include <filesystem>
#include <fstream>

namespace trading_engine::oracle {

namespace {

const char* op_to_string(ConstraintOp op) noexcept {
    switch (op) {
        case ConstraintOp::Equal:
            return "Equal";
        case ConstraintOp::LessEqual:
            return "LessEqual";
        case ConstraintOp::GreaterEqual:
            return "GreaterEqual";
    }
    return "Equal";
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

bool write_variables_bin(
    const std::filesystem::path& path,
    const std::vector<BooleanVariable>& variables,
    std::vector<std::string>* errors
) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        errors->push_back("failed to open variables.bin");
        return false;
    }

    const auto count = static_cast<std::uint32_t>(variables.size());
    write_pod(output, count);
    for (const auto& variable : variables) {
        write_pod(output, variable.var_id);
        write_string(output, variable.variable_key);
        write_string(output, variable.market_id);
        write_string(output, variable.outcome_id);
        write_string(output, variable.asset_id);
    }

    return output.good();
}

bool write_constraints_bin(
    const std::filesystem::path& path,
    const std::vector<LinearBooleanConstraint>& constraints,
    std::vector<std::string>* errors
) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        errors->push_back("failed to open constraints.bin");
        return false;
    }

    const auto count = static_cast<std::uint32_t>(constraints.size());
    write_pod(output, count);
    for (const auto& constraint : constraints) {
        const auto op = static_cast<std::uint8_t>(constraint.op);
        const auto terms = static_cast<std::uint32_t>(constraint.var_ids.size());
        write_pod(output, op);
        write_pod(output, constraint.rhs);
        write_pod(output, terms);
        for (std::size_t i = 0; i < constraint.var_ids.size(); ++i) {
            write_pod(output, constraint.var_ids[i]);
            write_pod(output, constraint.coeffs[i]);
        }
    }

    return output.good();
}

bool write_debug_json(
    const std::filesystem::path& variable_path,
    const std::filesystem::path& constraint_path,
    const CompiledConstraintSet& compiled,
    std::vector<std::string>* errors
) {
    std::ofstream variables(variable_path);
    if (!variables) {
        errors->push_back("failed to open variables.debug.json");
        return false;
    }

    variables << "{\n  \"variables\": [\n";
    for (std::size_t i = 0; i < compiled.variables.size(); ++i) {
        const auto& variable = compiled.variables[i];
        variables << "    {\"var_id\":" << variable.var_id
                  << ",\"variable_key\":\"" << variable.variable_key
                  << "\",\"market_id\":\"" << variable.market_id
                  << "\",\"outcome_id\":\"" << variable.outcome_id
                  << "\",\"asset_id\":\"" << variable.asset_id << "\"}";
        variables << (i + 1 == compiled.variables.size() ? "\n" : ",\n");
    }
    variables << "  ]\n}\n";

    std::ofstream constraints(constraint_path);
    if (!constraints) {
        errors->push_back("failed to open constraints.debug.json");
        return false;
    }

    constraints << "{\n  \"constraints\": [\n";
    for (std::size_t i = 0; i < compiled.constraints.size(); ++i) {
        const auto& constraint = compiled.constraints[i];
        constraints << "    {\"op\":\"" << op_to_string(constraint.op)
                    << "\",\"rhs\":" << constraint.rhs
                    << ",\"terms\":[";
        for (std::size_t j = 0; j < constraint.var_ids.size(); ++j) {
            constraints << "{\"var_id\":" << constraint.var_ids[j]
                        << ",\"coeff\":" << constraint.coeffs[j] << "}";
            if (j + 1 != constraint.var_ids.size()) {
                constraints << ",";
            }
        }
        constraints << "]}";
        constraints << (i + 1 == compiled.constraints.size() ? "\n" : ",\n");
    }
    constraints << "  ],\n";
    constraints << "  \"constraint_hash\": " << compiled.constraint_hash
                << "\n}\n";

    return variables.good() && constraints.good();
}

}  // namespace

bool MatrixBuilder::write(
    const CompiledConstraintSet& compiled,
    const std::string& directory,
    std::vector<std::string>* errors
) const {
    std::vector<std::string> local_errors;
    auto* out_errors = errors ? errors : &local_errors;

    const std::filesystem::path dir(directory);
    std::filesystem::create_directories(dir);

    bool ok = true;
    ok &= write_variables_bin(dir / "variables.bin", compiled.variables, out_errors);
    ok &= write_constraints_bin(
        dir / "constraints.bin",
        compiled.constraints,
        out_errors
    );
    ok &= write_debug_json(
        dir / "variables.debug.json",
        dir / "constraints.debug.json",
        compiled,
        out_errors
    );

    return ok;
}

}  // namespace trading_engine::oracle

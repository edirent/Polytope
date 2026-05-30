#include "oracle/compiler/ConstraintCompiler.h"

#include "oracle/compiler/BooleanConstraintBuilder.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace trading_engine::oracle {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

void hash_u32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (int shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_i32(std::uint64_t* hash, std::int32_t value) noexcept {
    hash_u32(hash, static_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (unsigned char c : value) {
        hash_byte(hash, c);
    }
    hash_byte(hash, 0xffU);
}

std::vector<BooleanVariable> normalize_variables(
    const std::vector<BooleanVariable>& variables,
    std::vector<std::string>* errors
) {
    std::vector<BooleanVariable> out = variables;
    std::sort(
        out.begin(),
        out.end(),
        [](const BooleanVariable& lhs, const BooleanVariable& rhs) {
            return lhs.variable_key < rhs.variable_key;
        }
    );

    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i].variable_key.empty()) {
            errors->push_back("boolean variable missing variable_key");
            continue;
        }

        const auto [_, inserted] = seen.insert(out[i].variable_key);
        if (!inserted) {
            errors->push_back(
                "duplicate boolean variable: " + out[i].variable_key
            );
        }

        out[i].var_id = static_cast<std::uint32_t>(i);
    }

    return out;
}

BooleanConstraintBuilder::VariableIndex index_variables(
    const std::vector<BooleanVariable>& variables
) {
    BooleanConstraintBuilder::VariableIndex out;
    for (const auto& variable : variables) {
        out.emplace(variable.variable_key, variable.var_id);
    }
    return out;
}

void append_rule_error(
    const ValidatedRule& rule,
    const std::string& message,
    std::vector<std::string>* errors
) {
    errors->push_back("rule " + rule.rule_id + ": " + message);
}

}  // namespace

ConstraintCompilationResult ConstraintCompiler::compile(
    const Rulebook& rulebook,
    const std::vector<BooleanVariable>& variables
) const {
    ConstraintCompilationResult result;
    result.compiled.variables = normalize_variables(variables, &result.errors);
    if (!result.errors.empty()) {
        return result;
    }

    const auto variable_index = index_variables(result.compiled.variables);
    BooleanConstraintBuilder builder;

    for (const auto& rule : rulebook.rules()) {
        if (!rule.approved) {
            append_rule_error(
                rule,
                "unapproved rule cannot be compiled",
                &result.errors
            );
            continue;
        }

        BooleanConstraintBuildResult built;
        switch (rule.type) {
            case RuleType::MutuallyExclusive:
                if (rule_is_exhaustive(rule.coverage)) {
                    built = builder.exactly_one(
                        rule.variable_ids,
                        variable_index
                    );
                } else {
                    built = builder.at_most_one(
                        rule.variable_ids,
                        variable_index
                    );
                }
                break;
            case RuleType::ExactlyOne:
                built = builder.exactly_one(
                    rule.variable_ids,
                    variable_index
                );
                break;
            case RuleType::AtMostOne:
                built = builder.at_most_one(
                    rule.variable_ids,
                    variable_index
                );
                break;
            case RuleType::AtLeastOne:
                built = builder.at_least_one(
                    rule.variable_ids,
                    variable_index
                );
                break;
            case RuleType::Implies:
                built = builder.implies(rule.variable_ids, variable_index);
                break;
            default:
                append_rule_error(
                    rule,
                    "unsupported rule type for constraint compiler",
                    &result.errors
                );
                continue;
        }

        if (!built.ok()) {
            for (const auto& error : built.errors) {
                append_rule_error(rule, error, &result.errors);
            }
            continue;
        }

        result.compiled.constraints.push_back(std::move(built.constraint));
    }

    result.compiled.constraint_hash =
        hash_compiled_constraints(result.compiled);
    return result;
}

std::uint64_t hash_compiled_constraints(
    const CompiledConstraintSet& compiled
) noexcept {
    std::uint64_t hash = kFnvOffset;

    for (const auto& variable : compiled.variables) {
        hash_u32(&hash, variable.var_id);
        hash_string(&hash, variable.variable_key);
        hash_string(&hash, variable.market_id);
        hash_string(&hash, variable.outcome_id);
        hash_string(&hash, variable.asset_id);
    }

    for (const auto& constraint : compiled.constraints) {
        hash_byte(&hash, static_cast<std::uint8_t>(constraint.op));
        hash_i32(&hash, constraint.rhs);
        hash_u32(
            &hash,
            static_cast<std::uint32_t>(constraint.var_ids.size())
        );

        for (const auto var_id : constraint.var_ids) {
            hash_u32(&hash, var_id);
        }
        for (const auto coeff : constraint.coeffs) {
            hash_i32(&hash, coeff);
        }
    }

    return hash;
}

}  // namespace trading_engine::oracle

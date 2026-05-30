#include "oracle/rules/Rulebook.h"

#include <boost/json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

const std::unordered_map<std::string, RuleType>& rule_type_map() {
    static const std::unordered_map<std::string, RuleType> values{
        {"MutuallyExclusive", RuleType::MutuallyExclusive},
        {"CollectivelyExhaustive", RuleType::CollectivelyExhaustive},
        {"AtMostOne", RuleType::AtMostOne},
        {"AtLeastOne", RuleType::AtLeastOne},
        {"ExactlyOne", RuleType::ExactlyOne},
        {"Implies", RuleType::Implies},
        {"Equivalent", RuleType::Equivalent},
        {"Disjoint", RuleType::Disjoint},
        {"Custom", RuleType::Custom}
    };
    return values;
}

const std::unordered_map<std::string, RuleCoverage>& rule_coverage_map() {
    static const std::unordered_map<std::string, RuleCoverage> values{
        {"ExclusiveOnly", RuleCoverage::ExclusiveOnly},
        {"ExhaustiveAndExclusive", RuleCoverage::ExhaustiveAndExclusive}
    };
    return values;
}

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

bool bool_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_bool()) {
        return false;
    }
    return it->value().as_bool();
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
    std::vector<std::string> values;
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_array()) {
        return values;
    }

    for (const auto& item : it->value().as_array()) {
        if (item.is_string()) {
            values.push_back(json::value_to<std::string>(item));
        }
    }

    return values;
}

json::array to_json_array(const std::vector<std::string>& values) {
    json::array out;
    for (const auto& value : values) {
        out.push_back(json::value(value));
    }
    return out;
}

ValidatedRule parse_rule(
    const json::object& object,
    std::size_t index,
    RulebookLoadResult* result
) {
    ValidatedRule rule;
    rule.rule_id = string_field(object, "rule_id");
    rule.variable_ids = string_array_field(object, "variable_ids");
    rule.approved = bool_field(object, "approved");
    rule.approved_by = string_field(object, "approved_by");
    rule.approved_at_ns = u64_field(object, "approved_at_ns");
    rule.source_rule_draft_id = string_field(object, "source_rule_draft_id");

    const std::string type_name = string_field(object, "type");
    if (!rule_type_from_string(type_name, &rule.type)) {
        result->errors.push_back(
            "rules[" + std::to_string(index) + "]: unknown rule type"
        );
    }

    const std::string coverage_name = string_field(object, "coverage");
    if (!coverage_name.empty() &&
        !rule_coverage_from_string(coverage_name, &rule.coverage)) {
        result->errors.push_back(
            "rules[" + std::to_string(index) + "]: unknown rule coverage"
        );
    }
    if (bool_field(object, "exhaustive")) {
        rule.coverage = RuleCoverage::ExhaustiveAndExclusive;
    }

    if (rule.rule_id.empty()) {
        result->errors.push_back(
            "rules[" + std::to_string(index) + "]: missing rule_id"
        );
    }

    return rule;
}

json::object to_json_object(const ValidatedRule& rule) {
    json::object object;
    object["rule_id"] = rule.rule_id;
    object["type"] = rule_type_to_string(rule.type);
    object["coverage"] = rule_coverage_to_string(rule.coverage);
    object["exhaustive"] = rule_is_exhaustive(rule.coverage);
    object["variable_ids"] = to_json_array(rule.variable_ids);
    object["approved"] = rule.approved;
    object["approved_by"] = rule.approved_by;
    object["approved_at_ns"] = rule.approved_at_ns;
    object["source_rule_draft_id"] = rule.source_rule_draft_id;
    return object;
}

}  // namespace

const char* rule_type_to_string(RuleType type) noexcept {
    switch (type) {
        case RuleType::MutuallyExclusive:
            return "MutuallyExclusive";
        case RuleType::CollectivelyExhaustive:
            return "CollectivelyExhaustive";
        case RuleType::AtMostOne:
            return "AtMostOne";
        case RuleType::AtLeastOne:
            return "AtLeastOne";
        case RuleType::ExactlyOne:
            return "ExactlyOne";
        case RuleType::Implies:
            return "Implies";
        case RuleType::Equivalent:
            return "Equivalent";
        case RuleType::Disjoint:
            return "Disjoint";
        case RuleType::Custom:
            return "Custom";
    }

    return "Custom";
}

bool rule_type_from_string(
    const std::string& value,
    RuleType* out
) noexcept {
    const auto it = rule_type_map().find(value);
    if (it == rule_type_map().end()) {
        return false;
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

const char* rule_coverage_to_string(RuleCoverage coverage) noexcept {
    switch (coverage) {
        case RuleCoverage::ExclusiveOnly:
            return "ExclusiveOnly";
        case RuleCoverage::ExhaustiveAndExclusive:
            return "ExhaustiveAndExclusive";
    }

    return "ExclusiveOnly";
}

bool rule_coverage_from_string(
    const std::string& value,
    RuleCoverage* out
) noexcept {
    const auto it = rule_coverage_map().find(value);
    if (it == rule_coverage_map().end()) {
        return false;
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

bool rule_is_exhaustive(RuleCoverage coverage) noexcept {
    return coverage == RuleCoverage::ExhaustiveAndExclusive;
}

void Rulebook::add_rule(const ValidatedRule& rule) {
    rules_.push_back(rule);
}

void Rulebook::clear() {
    rules_.clear();
}

const std::vector<ValidatedRule>& Rulebook::rules() const noexcept {
    return rules_;
}

std::vector<ValidatedRule> Rulebook::approved_rules() const {
    std::vector<ValidatedRule> out;
    for (const auto& rule : rules_) {
        if (rule.approved) {
            out.push_back(rule);
        }
    }
    return out;
}

std::vector<ValidatedRule> Rulebook::unapproved_rules() const {
    std::vector<ValidatedRule> out;
    for (const auto& rule : rules_) {
        if (!rule.approved) {
            out.push_back(rule);
        }
    }
    return out;
}

RulebookLoadResult Rulebook::load_json(const std::string& path) {
    RulebookLoadResult result;

    std::ifstream input(path);
    if (!input) {
        result.errors.push_back("failed to open rulebook: " + path);
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    boost::json::error_code error;
    const auto parsed = json::parse(buffer.str(), error);
    if (error || !parsed.is_object()) {
        result.errors.push_back("malformed rulebook JSON");
        return result;
    }

    const auto& object = parsed.as_object();
    const auto rules_it = object.find("rules");
    if (rules_it == object.end() || !rules_it->value().is_array()) {
        result.errors.push_back("missing rules array");
        return result;
    }

    const auto& rules = rules_it->value().as_array();
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!rules[i].is_object()) {
            result.errors.push_back(
                "rules[" + std::to_string(i) + "]: expected object"
            );
            continue;
        }
        result.rules.push_back(parse_rule(rules[i].as_object(), i, &result));
    }

    return result;
}

bool Rulebook::save_json(
    const std::string& path,
    std::vector<std::string>* errors
) const {
    json::array rules;
    for (const auto& rule : rules_) {
        rules.push_back(to_json_object(rule));
    }

    json::object root;
    root["rules"] = std::move(rules);

    std::ofstream output(path);
    if (!output) {
        if (errors) {
            errors->push_back("failed to open rulebook output: " + path);
        }
        return false;
    }

    output << json::serialize(root) << '\n';
    if (!output) {
        if (errors) {
            errors->push_back("failed to write rulebook output: " + path);
        }
        return false;
    }

    return true;
}

}  // namespace trading_engine::oracle

#include "oracle/rules/ManualRuleEditor.h"

#include <boost/json.hpp>

#include <fstream>
#include <sstream>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

std::string string_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

bool bool_field(
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

std::vector<std::string> string_array_field(
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

json::array to_json_array(const std::vector<std::string>& values) {
    json::array out;
    for (const auto& value : values) {
        out.push_back(json::value(value));
    }
    return out;
}

RuleDraft parse_draft(
    const json::object& object,
    std::size_t index,
    RuleDraftLoadResult* result
) {
    RuleDraft draft;
    draft.rule_id = string_field(object, "rule_id");
    draft.variable_ids = string_array_field(object, "variable_ids");
    draft.source_text = string_field(object, "source_text");
    draft.rationale = string_field(object, "rationale");
    draft.requires_manual_review =
        bool_field(object, "requires_manual_review", true);

    const auto type_name = string_field(object, "type");
    if (!rule_type_from_string(type_name, &draft.type)) {
        result->errors.push_back(
            "drafts[" + std::to_string(index) + "]: unknown rule type"
        );
    }
    const auto coverage_name = string_field(object, "coverage");
    if (!coverage_name.empty() &&
        !rule_coverage_from_string(coverage_name, &draft.coverage)) {
        result->errors.push_back(
            "drafts[" + std::to_string(index) + "]: unknown rule coverage"
        );
    }
    if (bool_field(object, "exhaustive", false)) {
        draft.coverage = RuleCoverage::ExhaustiveAndExclusive;
    }
    if (draft.rule_id.empty()) {
        result->errors.push_back(
            "drafts[" + std::to_string(index) + "]: missing rule_id"
        );
    }
    if (draft.variable_ids.empty()) {
        result->errors.push_back(
            "drafts[" + std::to_string(index) +
            "]: missing variable_ids"
        );
    }

    draft.requires_manual_review = true;
    return draft;
}

json::object to_json_object(const RuleDraft& draft) {
    json::object object;
    object["rule_id"] = draft.rule_id;
    object["type"] = rule_type_to_string(draft.type);
    object["coverage"] = rule_coverage_to_string(draft.coverage);
    object["exhaustive"] = rule_is_exhaustive(draft.coverage);
    object["variable_ids"] = to_json_array(draft.variable_ids);
    object["source_text"] = draft.source_text;
    object["rationale"] = draft.rationale;
    object["requires_manual_review"] = true;
    return object;
}

}  // namespace

RulebookLoadResult ManualRuleEditor::load_rulebook(
    const std::string& path
) const {
    return Rulebook::load_json(path);
}

bool ManualRuleEditor::write_approved_rulebook(
    const Rulebook& rulebook,
    const std::string& path,
    std::vector<std::string>* errors
) const {
    Rulebook approved;
    for (const auto& rule : rulebook.rules()) {
        if (rule.approved) {
            approved.add_rule(rule);
        }
    }
    return approved.save_json(path, errors);
}

std::vector<ValidatedRule> ManualRuleEditor::list_unapproved_rules(
    const Rulebook& rulebook
) const {
    return rulebook.unapproved_rules();
}

RuleDraftLoadResult ManualRuleEditor::load_rule_drafts(
    const std::string& path
) const {
    RuleDraftLoadResult result;

    std::ifstream input(path);
    if (!input) {
        result.errors.push_back("failed to open rule drafts: " + path);
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    boost::json::error_code error;
    const auto parsed = json::parse(buffer.str(), error);
    if (error || !parsed.is_object()) {
        result.errors.push_back("malformed rule drafts JSON");
        return result;
    }

    const auto& root = parsed.as_object();
    const auto drafts_it = root.find("drafts");
    if (drafts_it == root.end() || !drafts_it->value().is_array()) {
        result.errors.push_back("missing drafts array");
        return result;
    }

    const auto& drafts = drafts_it->value().as_array();
    for (std::size_t i = 0; i < drafts.size(); ++i) {
        if (!drafts[i].is_object()) {
            result.errors.push_back(
                "drafts[" + std::to_string(i) + "]: expected object"
            );
            continue;
        }
        result.drafts.push_back(parse_draft(
            drafts[i].as_object(),
            i,
            &result
        ));
    }

    return result;
}

bool ManualRuleEditor::write_rule_drafts(
    const std::vector<RuleDraft>& drafts,
    const std::string& path,
    std::vector<std::string>* errors
) const {
    json::array draft_values;
    for (const auto& draft : drafts) {
        draft_values.push_back(to_json_object(draft));
    }

    json::object root;
    root["drafts"] = std::move(draft_values);

    std::ofstream output(path);
    if (!output) {
        if (errors) {
            errors->push_back("failed to open rule drafts output: " + path);
        }
        return false;
    }

    output << json::serialize(root) << '\n';
    if (!output) {
        if (errors) {
            errors->push_back("failed to write rule drafts output: " + path);
        }
        return false;
    }
    return true;
}

ValidatedRule ManualRuleEditor::approve_draft(
    const RuleDraft& draft,
    const std::string& approved_by,
    std::uint64_t approved_at_ns
) const {
    ValidatedRule rule;
    rule.rule_id = draft.rule_id;
    rule.type = draft.type;
    rule.coverage = draft.coverage;
    rule.variable_ids = draft.variable_ids;
    rule.approved = true;
    rule.approved_by = approved_by;
    rule.approved_at_ns = approved_at_ns;
    rule.source_rule_draft_id = draft.rule_id;
    return rule;
}

Rulebook ManualRuleEditor::approve_drafts(
    const std::vector<RuleDraft>& drafts,
    const std::string& approved_by,
    std::uint64_t approved_at_ns
) const {
    Rulebook rulebook;
    for (const auto& draft : drafts) {
        rulebook.add_rule(approve_draft(
            draft,
            approved_by,
            approved_at_ns
        ));
    }
    return rulebook;
}

}  // namespace trading_engine::oracle

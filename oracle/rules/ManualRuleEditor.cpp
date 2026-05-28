#include "oracle/rules/ManualRuleEditor.h"

namespace trading_engine::oracle {

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

}  // namespace trading_engine::oracle

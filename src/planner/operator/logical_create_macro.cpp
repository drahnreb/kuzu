#include "planner/operator/logical_create_macro.h"

namespace kuzu {
namespace planner {

std::string LogicalCreateMacroPrintInfo::toString() const {
    std::string result = "Macro: ";
    result += macroName;
    return result;
}

void LogicalCreateMacro::computeFlatSchema() {
    createEmptySchema();
    schema->createGroup();
    schema->insertToGroupAndScope(outputExpression, 0);
}

void LogicalCreateMacro::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(outputExpression, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

} // namespace planner
} // namespace kuzu

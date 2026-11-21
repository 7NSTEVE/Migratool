#pragma once
#include "migratool/gateway.hpp"
#include <string>

namespace mt {

void docs_hub_schema(Database& db);

Json tool_docs_hub(const Project& p, Database& db, const Json& args);

} // namespace mt

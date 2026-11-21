#pragma once
#include "migratool/gateway.hpp"
#include <map>
#include <optional>
#include <string>

namespace mt {

fs::path env_registry_db_path();

Json env_lookup_tool(const std::string& query_or_key, bool verify_live = false);

Json env_resolve_project_profile(const Project& p);

std::map<std::string, std::string> env_materialize_recipe(const std::string& recipe_name);

Json tool_env_tools(const std::optional<Project>& p, const Json& args);

} // namespace mt

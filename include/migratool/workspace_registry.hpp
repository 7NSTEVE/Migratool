#pragma once
#include "migratool/gateway.hpp"

namespace mt {

// Default root directory for external workspace metadata/catalogs (%LOCALAPPDATA%\Migratool\workspaces)
fs::path default_workspaces_root();

// Activate or auto-create a workspace/archive/migration project from ANY folder or JSON path (zero root pollution!)
Json tool_activate_workspace(const Json& args, fs::path* out_project_file = nullptr, Log log = {}, Cancel* cancel = nullptr);

// List all registered projects/workspaces with live mode, entry count, catalog size, and session status
Json tool_list_workspaces(const Json& args = Json::object());

// Execute a read-only tool against another registered workspace (cross-project federated query)
Json tool_query_workspace(const Json& args, Log log = {}, Cancel* cancel = nullptr);

// Automatic workspace orientation (architecture, languages, build/test markers, top dirs -> persists _onboarding memory)
Json tool_onboard_workspace(const Project& p, Database& db, const Json& args = Json::object());

} // namespace mt

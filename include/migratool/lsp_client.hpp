#pragma once
#include "migratool/gateway.hpp"
#include <string>
#include <vector>

namespace mt {

// Discover already-installed language servers on PATH / standard toolchain env vars / project config (never installs software)
Json lsp_discover_servers(const Project& p);

// Semantic LSP tools with transparent fallback to Migratool's native AST / Roslyn / libclang index:
Json tool_get_diagnostics(const Project& p, Database& db, const Json& args);
Json tool_find_implementations(const Project& p, Database& db, const Json& args);
Json tool_find_declaration(const Project& p, Database& db, const Json& args);
Json tool_lsp_rename_symbol(const Project& p, Database& db, const Json& args);
Json tool_restart_language_server(const Project& p, const Json& args = Json::object());

// Shut down all active LSP child processes cleanly (called on process/server shutdown)
void lsp_shutdown_all();

} // namespace mt

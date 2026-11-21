#pragma once
#include "migratool/gateway.hpp"

namespace mt {

// Additive SQLite schema for structured project memories + FTS5 + markdown mirror in state_dir/memories/
void memories_schema(Database& db);

// Unified MCP/CLI entry point for project memories:
// action: "write" | "read" | "edit" | "list" | "search" | "rename" | "delete"
Json memory_request(const Project& p, Database& db, const Json& args);

} // namespace mt

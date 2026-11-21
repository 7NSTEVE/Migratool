#pragma once
#include "migratool/gateway.hpp"

namespace mt {

// Read-only housekeeping health report (catalog size, WAL size, freelist pages,
// telemetry/log row counts, expired claims, dead connections/watchers, stale temp files, registry stats).
Json housekeep_status(const Project& p, Database& db, const Json& args = Json::object());

// Execute automatic or on-demand self-housekeeping across catalog.sqlite, state_dir temp files,
// FTS5 trigram segments, WAL checkpoint, incremental_vacuum / optional full VACUUM, and global registry.
// When background=true, uses non-blocking WAL PASSIVE checkpoint and bounded incremental work.
Json housekeep_run(const Project& p, Database& db, const Json& args = Json::object(),
                   bool background = false, Log log = {});

// Unified MCP/CLI entry point for the "housekeep" tool (action = "status" | "run" | "vacuum").
Json tool_housekeep(const Project& p, Database& db, const Json& args = Json::object(), Log log = {});

} // namespace mt

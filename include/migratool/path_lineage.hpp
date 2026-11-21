#pragma once
#include "migratool/gateway.hpp"
#include <cstdint>
#include <string>

namespace mt {

void path_lineage_schema(Database& db);

void record_path_event(Database& db, int64_t root_id, uint64_t frn, const std::string& node_kind,
                       const std::string& op, const std::string& old_path, const std::string& new_path,
                       int64_t size_bytes = 0, const std::string& actor = "system",
                       const std::string& source = "fs_sync",
                       const std::string& confidence = "exact_transactional",
                       const std::string& workstream_id = "",
                       const std::string& old_sha = "", const std::string& new_sha = "",
                       const std::string& reason = "");

void record_usn_journal_gap(Database& db, int64_t root_id, uint64_t old_journal_id, uint64_t new_journal_id,
                            int64_t lost_from_usn, int64_t resumed_at_usn, const std::string& reason);

void record_scan_telemetry(Database& db, int64_t root_id, const std::string& scan_kind,
                           const std::string& engine, const std::string& started_at,
                           int64_t duration_ms, int64_t files, int64_t dirs, int64_t bytes,
                           int64_t added = 0, int64_t modified = 0, int64_t moved = 0, int64_t deleted = 0,
                           bool usn_discontinuity = false, const std::string& actor = "system");

Json resolve_moved_hint(const Project& p, Database& db, const std::string& missing_path);

Json tool_path_history(const Project& p, Database& db, const Json& args);

} // namespace mt

#pragma once
#include "migratool/gateway.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mt {

void runs_telemetry_schema(Database& db);

void record_efficiency(Database& db, const std::string& tool, const std::string& profile,
                       const std::string& actor, int64_t schema_bytes, int64_t req_bytes, int64_t resp_bytes,
                       int native_calls_saved, int files_skipped_sha, int tokens_raw, int tokens_returned,
                       int64_t duration_ms, const std::string& workstream_id = "");

Json resolve_task_dag(Database& db);

Json select_affected_tests(const Project& p, Database& db, const std::vector<std::string>& touched_paths);

Json tool_agent_wake(const Project& p, Database& db, const Json& args);

Json tool_repo_map(const Project& p, Database& db, const Json& args);

Json tool_read_bundle(const Project& p, Database& db, const Json& args);

Json tool_batch_edit(const Project& p, Database& db, const Json& args, Log log = {}, Cancel* cancel = nullptr);

Json tool_run_verified(const Project& p, Database& db, const Json& args, Log log = {}, Cancel* cancel = nullptr);

Json tool_impact_analysis(const Project& p, Database& db, const Json& args);

Json tool_delta_since(const Project& p, Database& db, const Json& args);

} // namespace mt

#pragma once
#include "migratool/gateway.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mt {

void workstream_schema(Database& db);

std::string active_workstream_id(Database& db, const std::string& actor = "");

std::string create_content_snapshot(const Project& p, Database& db, const std::vector<fs::path>& abs_paths,
                                    const std::string& workstream_id, const std::string& label,
                                    const std::string& actor, bool pinned = true);

Json restore_content_snapshot(const Project& p, Database& db, const std::string& snapshot_id,
                              const std::string& actor, bool dry_run = false,
                              const std::string& conflict_policy = "abort");

std::string begin_durable_operation(const Project& p, Database& db, const std::string& workstream_id,
                                    const std::string& tool_name, const std::string& idempotency_key,
                                    const Json& files_plan, const std::string& actor);

void advance_durable_operation(Database& db, const std::string& op_id, const std::string& stage,
                               const Json& extra = Json::object());

std::optional<Json> check_idempotency_ledger(Database& db, const std::string& idempotency_key,
                                             const std::string& tool_name, const std::string& args_sha256);

void store_idempotency_ledger(Database& db, const std::string& idempotency_key,
                              const std::string& tool_name, const std::string& args_sha256,
                              const Json& response);

void check_workstream_file_conflicts(Database& db, const std::string& current_ws_id,
                                     const std::vector<std::string>& rel_paths,
                                     const std::vector<uint64_t>& frns = {});

void workstream_auto_record_touch(const Project& p, Database& db, const std::string& rel_path, uint64_t frn,
                                  const std::string& pre_sha, const std::string& post_sha,
                                  const std::string& actor, const std::string& explicit_ws_id = "");

Json workstream_detect_and_reconcile(const Project& p, Database& db);

Json workstream_prune_ephemeral_snapshots(const Project& p, Database& db,
                                          int max_ephemeral_keep = 20,
                                          double max_ephemeral_days = 14.0,
                                          bool dry_run = false);

Json tool_workstream(const Project& p, Database& db, const Json& args);

} // namespace mt

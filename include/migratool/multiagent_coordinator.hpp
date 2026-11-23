#pragma once
#include "migratool/core.hpp"
#include "migratool/sql.hpp"

namespace mt {

// Ensures all SQLite tables for multi-agent worktree coordination, integration queue,
// build provenance manifests, configurable directory classification policies, storage quotas,
// replica discovery, session pendings, and surgical PRD requirements traceability exist.
void ensure_multiagent_coordinator_schema(Database& db);

// REQ-WORKTREE-001: Persistent Worktree & Agent Progress Inventory + Seamless Handover
// Actions:
//   - "scan" | "list" | "inventory" (default): discovers live Git worktrees via `git worktree list --porcelain`,
//     inspects uncommitted changes and ahead/behind counts, reconciles with `agent_worktrees_catalog`,
//     and enforces the 4-state lifecycle: IMPLEMENTED | CONFIRMED | INTEGRATED | DELIVERED.
//   - "register" | "update": registers or updates worktree ownership (owner_agent, task_id, goal,
//     lifecycle_state, is_mandatory, active_build_pid, pinned_snapshot_id, notes, or explicit exclusion
//     with excluded_by + exclusion_reason).
//   - "remove_worktree": safely removes a registered Git worktree (`git worktree remove`) ONLY after
//     verifying uncommitted_count == 0, no active_build_pid running, and worktree is INTEGRATED/DELIVERED
//     or explicitly excluded (never deletes unintegrated mandatory work!).
//   - "prune_stale_admin": runs `git worktree prune` to clean stale Git administrative records,
//     explicitly separated from build directory storage cleanup.
//   - "resume" | "handoff": single-call handover packet when an agent hits quota or disconnects.
Json tool_worktree_inventory(const Project& p, Database& db, const Json& args);

// Alias super-tool for single-call multi-agent takeover / session resumption (REQ-WORKTREE-001 & REQ-TRACE-001)
Json tool_project_resume(const Project& p, Database& db, const Json& args);

// REQ-INTEGRATE-001: Multi-Agent Integration Manager
// Actions:
//   - "evaluate" | "status" (default): evaluates merge compatibility across registered/discovered
//     worktrees against target_branch non-destructively via `git merge-tree --write-tree`,
//     updates `integration_queue`, and computes `canonical_build_ready` (false if any mandatory
//     worktree in IMPLEMENTED or CONFIRMED state remains unintegrated without explicit exclusion).
//   - "enqueue": adds or updates a worktree branch in `integration_queue`.
//   - "mark_integrated": records verified integration of a worktree into the target branch.
//   - "merge_execute": non-destructively pre-checks via `git merge-tree --write-tree`, verifies
//     clean target tree and CONFIRMED state (plus optional `enforce_audit_gates`), executes
//     `git merge --no-ff` (with automatic `git merge --abort` on failure), optionally runs
//     verification tests, and promotes the worktree from CONFIRMED -> INTEGRATED.
Json tool_integrate_ready(const Project& p, Database& db, const Json& args);

// REQ-BUILD-001: Build Provenance Manifests & Reverse Binary Traceability
// Actions:
//   - "record" | "verify_canonical" (default): records a build in `build_manifests` + `build_artifacts`
//     and writes `.migratool/builds/<build_id>/manifest.json`. Enforces the hard rule:
//     no build can be marked INTEGRATED or DELIVERED if mandatory worktrees in IMPLEMENTED or
//     CONFIRMED state remain unintegrated (unless explicitly excluded with excluded_by + exclusion_reason).
//     If the worktree has uncommitted files (!tree_clean) and no snapshot_id was provided, automatically
//     captures a P4 content snapshot so dirty builds remain 100% reproducible.
//   - "build_and_record": executes a real build (via CMake preset / command / configured build_id) when
//     `execute: true`, tracks `active_build_pid` in `agent_worktrees_catalog` during compilation,
//     checks optional `enforce_audit_gates` via `tool_code_audit`, hashes all produced artifacts,
//     and records the immutable build manifest.
//   - "trace_binary": O(1) reverse lookup by `artifact_path` or `sha256` returning the exact
//     commit, branch, worktree, dirty snapshot, agent, integration completeness, and manifest.
//   - "list": lists build manifests and their storage conservation tiers.
Json tool_build_canonical(const Project& p, Database& db, const Json& args);

// REQ-STORAGE-001: Configurable Directory Policies, Quotas & 4-Tier Safe Storage Audit
// Categories: SOURCE, TEST_SOURCE, BUILD_OUTPUT, GENERATED, DEPENDENCY, CACHE, DOCUMENTATION, USER_DATA, UNKNOWN
// Conservation tiers: PROTECTED, TEMP_RETAINED, VERIFIED_SAFE_CLEAN, NEEDS_REVIEW
// Actions:
//   - "audit" (default): classifies project directories, worktrees, and build outputs into the 9 categories
//     and 4 conservation tiers; evaluates storage quotas (`max_build_storage_gb`, `keep_last_n_builds`,
//     `max_temp_age_days`) and detects duplicate build artifacts across build directories by SHA-256.
//   - "set_policy" | "list_policies": configures or lists per-project directory classification rules.
//   - "set_quota" | "get_quota": configures or queries storage quota and retention thresholds.
//   - "clean_verified": sends ONLY `VERIFIED_SAFE_CLEAN` directories/builds (when execute=true) to the
//     Windows Recycle Bin, strictly refusing PROTECTED, TEMP_RETAINED, and NEEDS_REVIEW.
Json tool_storage_audit(const Project& p, Database& db, const Json& args);

// REQ-DISCOVERY-001: Dispersed Project Discovery & Orphan Work Recovery
// Classifications: MAIN_PROJECT, RELATED_WORKTREE, POSSIBLE_CLONE, ORPHAN_WORK, OFFLINE_DRIVE, UNRELATED
// Actions:
//   - "discover" | "scan" (default): inspects candidate_paths / persisted search_roots (bounded depth)
//     by comparing Git root commits, remote URLs, `git merge-base` commit ancestry, and sampled source
//     file SHA-256 hashes; detects disconnected drives/paths as `OFFLINE_DRIVE` without purging history;
//     persists results in `discovered_project_replicas` and reports unintegrated commits/files.
//   - "list": returns persisted replica discovery records from SQLite.
Json tool_discover_replicas(const Project& p, Database& db, const Json& args);

// REQ-TRACE-001 & REQ-GUI-001: Master Requirements 6-Question Traceability & Executive Supervision Summary
// Actions:
//   - "summary" | "health" (default): returns low-cognitive-load executive summary:
//     pending_integrations_count, worktrees_with_pending_changes_count, old_builds_count,
//     reclaimable_storage_bytes, last_canonical_build, missing_completed_features_warning,
//     code_audit_summary, session_pendings_summary, and PRD requirements traceability coverage.
//   - "sync_prd": parses and indexes `docs/PRODUCT_REQUIREMENTS_MASTER.json` into SQLite
//     `product_requirements` and `requirement_trace_links` answering all 6 traceability questions
//     (Requirement -> File -> Test -> Last Commit -> Included Builds -> Post-Change Validity).
//   - "query_requirements" | "trace": returns only task-relevant REQ-* items filtered by
//     req_id, category, file_path, or query keyword without flooding agent context.
//   - "pendings_list" | "pendings_upsert" | "pendings_close": manages transient session pendings
//     (`session_pendings` table, e.g. `PENDIENTES_SESION_*.md`) separately from permanent master
//     requirements in `product_requirements`.
Json tool_project_health(const Project& p, Database& db, const Json& args);

} // namespace mt

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
//     verifying uncommitted_count == 0, untracked_count == 0, no active_build_pid running, and worktree
//     is INTEGRATED/DELIVERED or explicitly excluded (never deletes unintegrated mandatory work!).
//     Never calls permanent recursive deletion (`fs::remove_all`); falls back only to Windows Recycle Bin
//     or recoverable quarantine under `.migratool/quarantine/worktrees/<id>/`, failing closed with
//     `WORKTREE_REMOVE_FAILED` without deleting catalog rows if the directory remains locked or
//     `git worktree prune` fails.
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
//   - "mark_integrated": fails closed unless the worktree exists in catalog/live Git discovery, has
//     zero uncommitted/untracked changes (`MARK_INTEGRATED_REFUSED_DIRTY`), and its live HEAD commit
//     is proven via `git merge-base --is-ancestor <wt_head> <target_branch>` (`MARK_INTEGRATED_REFUSED_UNMERGED`);
//     records `verification_mode = "VERIFIED_GIT_ANCESTOR"`, `verified_source_head`, `verified_target_branch`,
//     and `verified_target_head`.
//   - "merge_execute": verifies clean source worktree (`MERGE_BLOCKED_DIRTY_SOURCE_WORKTREE`), exact
//     checked-out target branch via `git symbolic-ref --short HEAD` (`MERGE_BLOCKED_WRONG_TARGET_BRANCH`),
//     clean target worktree (`uncommitted == 0 && untracked == 0`), CONFIRMED state, non-destructive
//     `git merge-tree --write-tree` pre-check, and optional `enforce_audit_gates`; executes
//     `git merge --no-ff` (with automatic `git merge --abort` on failure) and verifies post-merge
//     `git merge-base --is-ancestor <source_head> HEAD` before promoting CONFIRMED -> INTEGRATED.
Json tool_integrate_ready(const Project& p, Database& db, const Json& args);

// REQ-BUILD-001: Build Provenance Manifests & Reverse Binary Traceability
// Actions:
//   - "record" | "verify_canonical" (default): records a `DECLARED_EXTERNAL` build in `build_manifests` +
//     `build_artifacts` and atomically writes an append-only immutable `.migratool/builds/<build_id>/manifest.json`
//     via exclusive creation (`BUILD_MANIFEST_IMMUTABLE_COLLISION` on reuse) with real `manifest_sha256`.
//     Refuses `DELIVERED` (`DELIVERED_REQUIRES_OBSERVED_BUILD`) and refuses non-zero `exit_code` for
//     CONFIRMED/INTEGRATED/DELIVERED (`BUILD_NONZERO_EXIT_CANNOT_CONFIRM`). Never synthesizes fake
//     artifact hashes (`BUILD_ARTIFACT_MISSING` when `require_existing_artifacts` is true or state is
//     INTEGRATED/DELIVERED). Enforces unintegrated mandatory worktree gate and auto-captures a P4
//     content snapshot if dirty.
//   - "build_and_record": executes a real build when `execute: true` (marking `provenance_mode = "OBSERVED_BUILD"`
//     on observed `exit_code == 0`), tracks `active_build_pid` in `agent_worktrees_catalog` during compilation,
//     defaults `enforce_audit_gates` to `true` for `DELIVERED`, requires at least one verified on-disk
//     artifact for `DELIVERED` (`DELIVERED_REQUIRES_VERIFIED_ARTIFACTS`), computes real disk SHA-256 hashes,
//     and atomically records the immutable build manifest.
//   - "trace_binary": O(1) reverse lookup by `artifact_path` or `sha256` returning the exact
//     commit, branch, worktree, dirty snapshot, agent, provenance_mode, manifest_sha256,
//     integration completeness, and manifest.
//   - "list": lists build manifests (including `provenance_mode`, `manifest_sha256`, and verified artifacts).
Json tool_build_canonical(const Project& p, Database& db, const Json& args);

// REQ-STORAGE-001: Configurable Directory Policies, Quotas & 4-Tier Safe Storage Audit
// Categories: SOURCE, TEST_SOURCE, BUILD_OUTPUT, GENERATED, DEPENDENCY, CACHE, DOCUMENTATION, USER_DATA, UNKNOWN
// Conservation tiers: PROTECTED, TEMP_RETAINED, VERIFIED_SAFE_CLEAN, NEEDS_REVIEW
// Actions:
//   - "audit" (default): classifies project directories, worktrees, and build outputs into the 9 categories
//     and 4 conservation tiers; fails closed to `NEEDS_REVIEW` (`auto_clean_allowed = false`) whenever
//     bounded source scanning is incomplete or encounters errors; exposes bounded-scan transparency
//     (`scan_complete`, `size_is_estimate`, `source_scan_complete`, `duplicate_scan_complete`,
//     `skipped_oversize_count`, `quota_evaluation_complete`), evaluates storage quotas, and detects
//     duplicate build artifacts across build directories by SHA-256.
//   - "set_policy" | "list_policies": configures or lists per-project directory classification rules.
//   - "set_quota" | "get_quota": configures or queries storage quota and retention thresholds.
//   - "clean_verified": sends ONLY `VERIFIED_SAFE_CLEAN` directories/builds (when execute=true) to the
//     Windows Recycle Bin, strictly refusing PROTECTED, TEMP_RETAINED, and NEEDS_REVIEW.
Json tool_storage_audit(const Project& p, Database& db, const Json& args);

// REQ-DISCOVERY-001: Dispersed Project Discovery & Orphan Work Recovery
// Classifications: MAIN_PROJECT, RELATED_WORKTREE, POSSIBLE_CLONE, ORPHAN_WORK, OFFLINE_DRIVE, UNRELATED
// Actions:
//   - "discover" | "scan" (default): canonicalizes project/candidate paths via `canonical_win32_path`,
//     recursively walks `search_roots` up to `max_depth` (default 2, clamped 1..5), compares Git root
//     commits, remote URLs, `git merge-base` commit ancestry, and bounded sampled source file SHA-256
//     hashes (reporting `sample_complete`, `visited_entries`, `sampled_files_count`); detects disconnected
//     drives/paths as `OFFLINE_DRIVE` without purging history; persists results in `discovered_project_replicas`.
//   - "list": returns persisted replica discovery records from SQLite.
Json tool_discover_replicas(const Project& p, Database& db, const Json& args);

// REQ-TRACE-001 & REQ-GUI-001: Master Requirements 6-Question Traceability & Executive Supervision Summary
// Actions:
//   - "summary" | "health" (default): returns low-cognitive-load executive summary:
//     pending_integrations_count, worktrees_with_pending_changes_count, old_builds_count,
//     reclaimable_storage_bytes, last_canonical_build, missing_completed_features_warning,
//     code_audit_summary, session_pendings_summary, and honest PRD `requirements_traceability` /
//     `requirements_coverage` metrics separating `file_presence_pct` from `verified_coverage_pct`.
//   - "sync_prd": parses and indexes `docs/PRODUCT_REQUIREMENTS_MASTER.json` into SQLite
//     `product_requirements` and `requirement_trace_links` answering all 6 traceability questions
//     using structured `selected_targets_json` / `command_json` suite matching, exact `git_head` commit
//     matching, and on-disk file freshness checks (never substring-matching raw dumps).
//   - "query_requirements" | "trace": returns only task-relevant REQ-* items filtered by
//     req_id, category, file_path, or query keyword without flooding agent context.
//   - "pendings_list" | "pendings_upsert" | "pendings_close": manages transient session pendings
//     (`session_pendings` table, e.g. `PENDIENTES_SESION_*.md`) separately from permanent master
//     requirements in `product_requirements`.
Json tool_project_health(const Project& p, Database& db, const Json& args);

} // namespace mt

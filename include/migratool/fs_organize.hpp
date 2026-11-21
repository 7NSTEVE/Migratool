#pragma once
#include "migratool/gateway.hpp"

namespace mt {

// Universal multi-stage content deduplicator over fs_nodes (any file type: ZIP, DLL, ROM, PNG, PDF, code)
// Stage 1: SQLite size grouping -> Stage 2: 64 KiB partial SHA-256 (cached in fs_hashes) -> Stage 3: full SHA-256.
// Skips OneDrive cloud-only placeholders by default to avoid triggering unintended downloads.
Json tool_find_duplicates(const Project& p, Database& db, const Json& args = Json::object(), Cancel* cancel = nullptr);

// Verify every non-ignored entry of a .zip archive against a directory on disk by size + IEEE CRC32 (via libzip).
Json tool_verify_archive_vs_dir(const Project& p, Database& db, const Json& args = Json::object(), Cancel* cancel = nullptr);

// Complete pre-reorganization safety topology in 1 call:
// Git repos & linked/broken worktrees, NTFS junctions/symlinks, OneDrive cloud-only count,
// regenerable build/cache directories annotated with git tracked/ignored status, and hardcoded absolute path refs.
Json tool_inspect_workspace_topology(const Project& p, Database& db, const Json& args = Json::object(), Cancel* cancel = nullptr);

// Transactional dry-run organizer: validates invariants (zero git-tracked quarantine, zero junction moves,
// zero collisions), records baseline snapshot, and persists plan.jsonl under state_dir/organize/<plan_id>/.
Json tool_organize_plan(const Project& p, Database& db, const Json& args = Json::object(), Cancel* cancel = nullptr);

// Execute a validated organize_plan atomically on same volume (MoveFileExW + \\?\ paths, SHA-256 manifest,
// pre-edit backups, journal.jsonl with resume support). Requires execute:true. Never deletes permanently.
Json tool_organize_apply(const Project& p, Database& db, const Json& args = Json::object(), Log log = {}, Cancel* cancel = nullptr);

// Replay journal.jsonl in reverse order: restores pre-edit backups (verifying SHA-256), recreates empty dirs,
// and reverses every move. Requires execute:true.
Json tool_organize_undo(const Project& p, Database& db, const Json& args = Json::object(), Log log = {}, Cancel* cancel = nullptr);

// Independent post-apply verification (Conservation 1-to-1 root+quarantine vs baseline, 100% SHA-256 manifest,
// Git HEAD/status/worktree parity).
Json tool_organize_verify(const Project& p, Database& db, const Json& args = Json::object(), Cancel* cancel = nullptr);

} // namespace mt

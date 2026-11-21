#pragma once
#include "migratool/gateway.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mt {

// Node kinds stored in fs_nodes.kind
enum class FsNodeKind : int {
    File = 0,
    Directory = 1,
    SymlinkFile = 2,
    SymlinkDir = 3,
    Junction = 4
};

// Lightweight bulk record used by Tier A (MFT) / Tier B (multi-thread crawler) / synthetic stress tests
struct FsBulkEntry {
    uint64_t frn = 0;
    uint64_t parent_frn = 0;
    std::string name;
    std::string ext;        // lowercase including leading '.', or ""
    int kind = 0;
    int64_t size_bytes = 0;
    int64_t mtime_ns = 0;
    uint32_t attrs = 0;
    std::string link_target;
};

// Additive ultra-scale schema (fs_roots, fs_extensions, fs_nodes, fs_dirs, fs_name_fts, fs_dir_fts, fs_change_log, fs_errors)
void fs_schema(Database& db);

// Mode resolution: "migration" | "workspace" | "archive"
std::string project_mode(const Project& p);

// Returns true if a relative/absolute path should never trigger live watcher loops
bool is_watch_ignored_path(const std::wstring& rel_or_abs_lower, const Project& p);

// Simple wildcard glob matcher (*, ?, **), case-insensitive on Windows paths
bool fs_glob_match(std::string_view pattern, std::string_view text);

// Obtain 64-bit NTFS File Reference Number (FRN) for an existing file/directory handle or path (0 if unavailable)
uint64_t query_path_frn(const fs::path& path, uint64_t* out_parent_frn = nullptr,
                        int64_t* out_size = nullptr, int64_t* out_mtime_ns = nullptr,
                        uint32_t* out_attrs = nullptr, DWORD* out_reparse_tag = nullptr);

// Current UTC timestamp in nanoseconds since 1970-01-01 (int64)
int64_t now_ns();

// Bulk-load a full root snapshot into fs_nodes + fs_dirs + trigram FTS5 (used by crawler and 1M stress tests)
Json fs_bulk_replace_root(Database& db, int64_t root_id, uint64_t root_frn, int64_t root_mtime_ns,
                          const std::vector<FsBulkEntry>& entries,
                          const std::vector<Json>& scan_errors = {},
                          bool build_trigram_fts = true,
                          Cancel* cancel = nullptr);

// Full or incremental ultra-scale sync across configured roots (Tier A MFT/USN + Tier B multi-thread FileIdBothDirectoryInfo + mtime reconciler)
Json fs_sync(const Project& p, Database& db, const Json& args = Json::object(),
             Log log = {}, Cancel* cancel = nullptr);

// Incremental O(1) FRN mutations (used by USN replay, ReadDirectoryChangesExW watcher, and surgical file edits)
void fs_apply_upsert(Database& db, int64_t root_id, uint64_t frn, uint64_t parent_frn,
                     const std::string& name, int kind, int64_t size_bytes, int64_t mtime_ns,
                     uint32_t attrs, const std::string& link_target = "", bool log_change = true);

bool fs_apply_rename_or_move(Database& db, int64_t root_id, uint64_t frn,
                             uint64_t new_parent_frn, const std::string& new_name,
                             int64_t new_mtime_ns = 0, bool log_change = true);

bool fs_apply_delete(Database& db, int64_t root_id, uint64_t frn, bool log_change = true);

// Synchronize a single touched filesystem path immediately into fs_nodes/fs_dirs
bool fs_sync_single_path(const Project& p, Database& db, const fs::path& abs_path, bool log_change = true);

// Ultra-scale queries (<10-25ms on 6M+ entries)
Json fs_explore_tree(const Project& p, Database& db, const Json& args);
Json fs_find_files(const Project& p, Database& db, const Json& args);
Json fs_changes_feed(const Project& p, Database& db, const Json& args);
Json fs_stats_summary(const Project& p, Database& db);

// Ensure a root entry exists in fs_roots and return its integer root_id
int64_t fs_ensure_root(const Project& p, Database& db, const Json& root_cfg);

// Reconcile a root using hierarchical directory mtime comparison + FRN move detection (no full recrawl)
Json fs_reconcile_incremental(const Project& p, Database& db, const Json& root_cfg,
                              Log log = {}, Cancel* cancel = nullptr);

// Tier A: Attempt NTFS USN Journal delta replay directly into fs_nodes/fs_dirs
// Returns {used: bool, reason: string, applied: int64, moves: int64, needs_reconcile: bool}
Json fs_usn_replay_root(const Project& p, Database& db, const Json& root_cfg, Cancel* cancel = nullptr);

} // namespace mt

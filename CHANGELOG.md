# Changelog

## 2.0.0 — Ultra-Scale 6M+ Engine, Super-Serena Workspace/LSP & Auto-Housekeeping

### Ultra-Scale Filesystem & Archive Engine (`src/fs_index.cpp`, `src/fs_crawler.cpp`, `src/watcher.cpp`)
- Normalized NTFS FRN schema (`fs_roots`, `fs_extensions`, `fs_nodes`, `fs_dirs` `WITHOUT ROWID`, external-content `fs_name_fts` + `fs_dir_fts` with `tokenize='trigram'`, bounded 100K-row `fs_change_log`, `fs_errors`).
- Streaming 8-thread `FileIdBothDirectoryInfo` crawler (50,000-row batches, deferred index/FTS5 build, `< 200 MB` peak RAM at 6M entries) + Tier A MFT/USN delta replay + Tier B hierarchical `dir_mtime_ns` + FRN offline reconciler.
- $O(1)$ directory rename/move updates (`fs_apply_rename_or_move`) and atomic overwritten-FRN eviction.
- Upgraded `IndexWatcher` to 8 MB overlapped `ReadDirectoryChangesExW(ReadDirectoryNotifyExtendedInformation)` with `FileId` + `ParentFileId`, hard anti-self-trigger filter (`.migratool`, `state_dir`, `*-wal`, `*-shm`, `*-journal`, `.avhdx`, `.vhdx`, `.tmp-*`, `.new-*`), and removal of forced 15-minute full rescans in `archive`/`workspace` modes.
- Added `mode: "workspace" | "archive" | "migration"` and external `state_dir` support (`%LOCALAPPDATA%\Migratool\workspaces\`) so indexed roots stay untouched.
- New ultra-scale tools: `explore_tree`, `find_files`, `changes_feed`.

### Super-Serena Workspace, Memories, Surgical Toolkit & Hybrid LSP
- Global workspace registry & multi-workspace federation (`src/workspace_registry.cpp`): `activate_workspace`, `list_workspaces`, `query_workspace`, `onboard_workspace`.
- Persistent project memories (`src/memories.cpp`): SQLite FTS5 + Markdown mirror + NTFS FRN staleness detection on `linked_paths` (`memory`).
- Surgical file toolkit (`src/file_toolkit.cpp`): `read_file`, `create_file`, `edit_file` (with atomic multi-file rollback, `dry_run`, and `expected_sha256`), and multi-threaded native C++23 `search_pattern`.
- Structural AST disambiguation (`include/migratool/ast_engine.hpp`, `src/gateway.cpp`): separates declaration headers and `VarDecl`/`FieldDecl`/`ParmDecl` spans from call sites (`0` false positives, `5/5` on competitor benchmark).
- Hybrid lazy LSP bridge (`src/lsp_client.cpp`): native C++23 JSON-RPC 2.0 stdio supervisor with Win32 Job Object process isolation, idle auto-reaping, standard `PATH`/env auto-discovery, and native index fallback (`get_diagnostics`, `find_declaration`, `find_implementations`, `lsp_rename_symbol`, `restart_language_server`).
- Concurrent MCP read pool (`src/mcp.cpp`): up to 4 parallel WAL read-only queries with serialized write lane.

### Automatic & On-Demand Self-Housekeeping (`src/housekeep.cpp`, `src/live.cpp`, `src/database.cpp`)
- Enabled `PRAGMA auto_vacuum=INCREMENTAL`, `page_size=16384`, and `journal_size_limit=64 MiB` on catalog databases.
- Added `housekeep` tool (`action: "status" | "run" | "vacuum"`, `dry_run`, `deep`, `compact_full`, `prune_missing_projects`).
- Added non-blocking background auto-housekeeping every 30 minutes inside `IndexWatcher` and automatic global registry trimming inside `LiveSession::finish`.
- Safely recycles stale internal temp staging files and expired edit rollback backups via the Windows Recycle Bin.

### Verification
- 17/17 CTest suites passing, including `fs_index_ultra_scale` (1M-entry stress test + live watcher O(1) rename test) and `serena_workspace_lsp` (AST, multi-workspace federation, memories, atomic rollback, mock stdio LSP, and self-housekeeping tests).

---

## 1.2.0 — Development Control, One-Shot Workflow & Compact Output
- Added `session_bootstrap`, `complete_task` one-shot atomic workflow, `project_phases`, `project_tasks`, `doc_audits`, `module_matrix`, `format: "columnar"`, and `preset: "minimal" | "agent"`.
- Added XmlLite XAML structural analyzer (`src/analyzers.cpp`) and MSVC CMake `compile_commands.json` context resolution.
- Added selective test execution (`test --case <id>` / MCP `verify(case_ids=[...])`) and multi-root proof freshness checks (`src/scale_evidence.cpp`).
- Improved bounded pagination cursor handling for large object and columnar query results.

---

## 1.0.0 — Code Engineering Engine for AI Agents
- Surgical symbol navigation (`find_symbol`, `get_symbol_body`, `get_symbols_overview`, `find_definition`, `get_diagnostics`, `range`) with exact UTF-8 byte offsets and per-symbol SHA-256 hashes.
- Semantic call/reference graph via Roslyn `CSharpCompilation` + `SemanticModel` and `libclang` USR resolution (`find_references`, `find_callers`, `find_callees`, `find_implementations`, `type_hierarchy`, `impact_analysis`).
- Transactional symbolic editing (`edit_symbol`, `batch_update`) with `dry_run`, unified diff, pre-image backup, and full disk+SQLite rollback.
- Hybrid search (`query(kind="hybrid_search")`), structural AST matching/rewriting, multi-agent claims (`claim_task`, `renew_claim`, `release_task`), bundle reconciliation (`export_bundle`, `reconcile_bundle`), and `get_task_context`.

---

## 0.3.0 — Durable Scale, Live Discovery & Exact Duplicates
- Shared project registry (`projects.sqlite`) for live discovery and session activity between CLI, MCP, and GUI.
- Durable resumable inventory engine (`scale.cpp`), exact byte-level duplicate detection including streaming ZIP inspection via `libzip`, and linked Origin/Target GUI view.
- Initial native C++23 Win32 GUI/CLI/MCP architecture with SQLite WAL, FTS5 search, immutable audit history triggers, differential test runner, and coverage import.

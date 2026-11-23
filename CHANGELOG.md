# Changelog

## [3.1.0] - 2026-11-22 — Multi-Agent Coordinator, Continuous Incremental Technical Audit Engine & P0/P1 Security Hardening

### Modular Continuous Incremental Technical Audit & Anti-Hallucination Engine (`REQ-AUDIT-001`..`REQ-AUDIT-004`, `code_audit`)
- **Incremental Code Audit Engine (`include/migratool/code_audit.hpp`, `src/code_audit.cpp`, `tests/code_audit_tests.cpp`)**: Persists 6 SQLite tables (`code_audit_files`, `code_audit_header_deps`, `code_audit_functions`, `code_audit_findings`, `code_audit_snapshots`, `agent_claim_verifications`) with `compile_commands.json` discovery, local `#include` transitive header invalidation, and cache reuse keyed by `(file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version)`.
- **Strict Language Separation & AST Cyclomatic Complexity**: Separates C (`.c`, `/TC`, `-x c`, where `NULL` is valid) from C++23 (`CPP23-MODERNIZE-NULLPTR`, `CPP23-MODERNIZE-AUTOPTR`), computes token-based AST cyclomatic complexity immune to comments/string literals, disambiguates duplicate `TEST_CASE("...")` blocks via `file_path:qualified_name:start_line`, and triages security matches inside `TEST_SOURCE` files as `PENDING_VALIDATION` (`P3`) instead of confirmed production vulnerabilities.
- **Anti-Hallucination Agent Claim Verifier (`code_audit(action="verify_claim")`)**: Independently verifies agent claims across `functions_evidence`, `compilation_evidence`, `tests_evidence`, `quality_evidence`, and `functional_equivalence_evidence` (reporting `NOT_PROVEN` unless differential cases pass).

### Multi-Agent Coordination, Integration Manager, Build Provenance & Storage Governance (`REQ-WORKTREE-001`..`REQ-GUI-001`)
- **Persistent Worktree & Agent Handover Catalog (`REQ-WORKTREE-001`, `agent_worktrees_catalog`, `project_resume` / `worktree_inventory`)**: Tracks project ownership, agent identity (`Codex`, `Antigravity`, `Claude Code`), task context, branch/HEAD commit, unintegrated commits, uncommitted modifications (`git worktree list --porcelain` + `git status --porcelain=v2`), active build PIDs, disk usage, pinned `P4` handover snapshots, safe guarded `remove_worktree`, and administrative `prune_stale_admin`.
- **Strict 4-State Work Lifecycle & Integration Manager (`REQ-INTEGRATE-001`, `integration_queue`, `integrate_ready`)**: Enforces strict separation between `IMPLEMENTED`, `CONFIRMED`, `INTEGRATED`, and `DELIVERED`. Performs non-destructive merge conflict pre-evaluation via `git merge-tree --write-tree`, guarded `merge_execute` (`git merge --no-ff --no-edit` with optional `code_audit` gate check), and blocks canonical builds while mandatory worktrees remain unintegrated.
- **Build Manifests & Reverse Binary Traceability (`REQ-BUILD-001`, `build_manifests`, `build_canonical`, `trace_binary`)**: Supports both `record` and live `build_and_record` (with automatic dirty-tree content snapshot capture and RAII `active_build_pid` tracking), writing immutable manifests under `.migratool/builds/<build_id>/manifest.json` with artifact SHA-256 hashes for $O(1)$ reverse lookup.
- **Configurable Directory Policies, Quotas & 4-Tier Storage Audit (`REQ-STORAGE-001`, `storage_audit`)**: Classifies paths into 9 verifiable categories and 4 conservation tiers (`PROTECTED`, `TEMP_RETAINED`, `VERIFIED_SAFE_CLEAN`, `NEEDS_REVIEW`), enforces configurable `storage_quota_policies` (`set_quota`/`get_quota`), detects duplicate binary artifacts by SHA-256, and recycles only verified reproducible artifacts via the Windows Recycle Bin.
- **Project Discovery & Recovery (`REQ-DISCOVERY-001`, `discover_replicas`)**: Identifies dispersed instances as `MAIN_PROJECT`, `RELATED_WORKTREE`, `POSSIBLE_CLONE`, `ORPHAN_WORK`, or `OFFLINE_DRIVE`, persisting `discovery_search_roots` and computing `merge_base_commit` / `is_ancestor_of_main`.
- **Master Requirements Specification & 6-Question Traceability (`REQ-TRACE-001`, `docs/PRODUCT_REQUIREMENTS_MASTER.md`, `docs/PRODUCT_REQUIREMENTS_MASTER.json`, `project_health`)**: Indexes 22 master requirements (`Requirement -> Implementation -> Commit -> Build -> Test -> Post-Change Freshness`), maintains isolated `session_pendings` (`pendings_upsert`/`pendings_list`/`pendings_close`), and adds 4-dimensional `cost_dimensions` telemetry in `agent_wake`.
- **Apple HIG Win32 Supervision Views `30..33` & 200% Zoom Reflow (`REQ-GUI-001`, `src/gui.cpp`, `src/scale_view.cpp`, `include/migratool/navigation.hpp`, `include/migratool/ui.hpp`)**: Added native Spanish views for `Coordinación y worktrees` (`30`), `Compilaciones y almacenamiento` (`31`), `Requisitos maestros` (`32`), and `Auditoría técnica continua` (`33`), plus 200% zoom polish (`Solo vinculados`) and `BeginDeferWindowPos` / `WM_SETREDRAW` latency optimizations (`<= 200 ms`).

### Security & Confinement Hardening (`REQ-SEC-001`..`REQ-SEC-005`)
- **Canonical Physical Confinement (`REQ-SEC-001`)**: `Project::resolve_confined` with `GetFinalPathNameByHandleW`, NTFS reparse-point/junction escape blocking, `..` traversal blocking, and external UNC path rejection across `workstream`, `read_bundle`, `fs_organize`, `scale_export`, `backup_catalog`, `git_intel`, and `lsp_client`.
- **Production Failpoint Lockdown (`REQ-SEC-002`)**: Disabled fault-injection failpoints by default in production binaries (`failpoints_enabled() == false`), restricting activation strictly to test binaries.
- **Subprocess Isolation & Non-Blocking Async I/O (`REQ-SEC-003`)**: Redirected child `hStdInput -> NUL`, enforced Win32 `JobObject` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), and added real asynchronous read timeouts so hung Git/LSP subprocesses never consume MCP `stdin` or hang the server.
- **Bounded MCP JSON-RPC Transport (`REQ-SEC-004`)**: Added a 16 MiB line limit on `stdin`, structural JSON type validation before typed access, UTF-8 tolerant serialization (`error_handler_t::replace`), and full exception guards across worker threads.
- **Safe Optional DLL Loading & Git-Scoped Source Packaging (`REQ-SEC-005`)**: Prioritized `executable_dir()` when loading `libzip.dll` and `libclang.dll`, and restricted source code packaging strictly to `git ls-files`.

### Data Integrity, Workstreams & NTFS Ultra-Scale Fixes (`REQ-DATA-001`..`REQ-DATA-006`)
- **Lossless `fs_organize` Resume & Undo (`REQ-DATA-001`)**: `organize_apply(resume=true)` never deletes `src` when `src` and `dst` coexist (`SAFETY_CONFLICT`), preserves immutable `plan_id`, and verifies SHA-256 before `organize_undo`.
- **Cryptographic Snapshot & Blob Integrity (`REQ-DATA-002`)**: Verifies SHA-256 when writing existing content-addressed blobs, pre-validates 100% of required blobs before starting `restore_snapshot`, detects external out-of-band edits (`EXTERNAL_MODIFICATION_WITHOUT_INTENT`), and tracks `existed=false` for clean new-file rollback in `batch_edit`.
- **Honest Workstream & Test Verification (`REQ-DATA-003`)**: Verifies `expected_post_sha256` on disk before promoting WAL operations to `COMMITTED`, blocks concurrent duplicate `idempotency_key` calls while `IN_PROGRESS`, implements transactional SQLite partitioning in `split_workstream`, and prohibits fabricating `passed_cnt = 1` in `run_verified` when no test runner output is detected.
- **NTFS Ultra-Scale Index Fidelity (`REQ-DATA-004`, `REQ-DATA-005`, `REQ-DATA-006`)**: Fixed producer-consumer backpressure and `complete=0` rebuild state in `crawl_root_streaming`; populated real file size and `mtime_ns` in `crawl_root_mft`; supported volume roots (`E:\`) in `usn_delta_replay`; restricted `fs_reconcile_incremental` purging strictly to `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND`; fixed Unicode character length prefix replacement (`length(?)`) in `fs_apply_rename_or_move`; purged stale trees on root path/FRN change in `fs_ensure_root`; enabled full Win32 long-path (`>260` chars) support via `native_path()` (`\\?\`) and `<longPathAware>true</longPathAware>`; added kernel named-mutex synchronization in `SharedBudgetLedger`; and fixed SQL column projections in `knowledge_search` (`r.abs_path`) and `scale_query(relations)` (`source_path`, `target_path`, `reason`).

---

## [3.0.0] - 2026-11-05 — AI Agent Project OS (13 Super-Tools, Durable Workstreams, Epistemic Knowledge & Env Registry)

### AI Agent Project OS Core (`src/workstream.cpp`, `src/agent_supertools.cpp`, `src/knowledge.cpp`, `src/docs_hub.cpp`, `src/path_lineage.cpp`, `src/git_intel.cpp`, `src/env_registry.cpp`)
- **Single-Call Orientation (`agent_wake` & `delta_since`)**: Reconciles active workstreams after crashes, power cuts, or quota resets in `< 25 ms`, verifies live disk SHA-256 and Git HEAD (`CONFIRMED` / `STALE` / `MODIFIED_EXTERNALLY`), surfaces proactive knowledge warnings, Task DAG `ready_tasks`, and `BM1..BM7` efficiency telemetry.
- **Durable 7-Stage Workstreams (`workstream`)**: Macro session state machine + per-operation WAL (`PREPARED -> BACKED_UP -> APPLYING -> APPLIED -> INDEXED -> COMMITTED -> RECOVERY`), multi-agent file lock detection (`WORKSTREAM_FILE_CONFLICT`), `idempotency_ledger`, and P4 pinned content-addressed snapshots with `ref_count == 0` Recycle Bin GC.
- **Epistemic Knowledge Ledger & Docs Hub (`knowledge`, `docs_hub`)**: Structured findings, decisions, pitfalls, and tried methods with epistemic provenance (`verified_fact`, `test_result`, `agent_hypothesis`, `stale`), automatic FRN/SHA-256 staleness invalidation, proactive warning injection into `read_bundle`, H1–H4 Markdown TOC indexing, surgical `read_section`, and `drift_report`.
- **FRN Path Lineage, Cached Git Intel & Host Toolchain Registry (`path_history`, `git_intel`, `env_tools`)**: Immutable FRN rename/move lineage (`FILE_MOVED_HINT`), cached Git status/diff/blame intelligence, and projectless host toolchain discovery (`%LOCALAPPDATA%\Migratool\env_registry.sqlite`, `D:\DevTools` + PATH shadow diagnosis `PATH_SHADOWED` / `DUPLICATE_VERSION`).
- **Surgical Engineering Super-Tools & MCP Profiles (`repo_map`, `read_bundle`, `batch_edit`, `run_verified`)**: Personalized PageRank repository maps, 25-file batch reader with `known_sha256` deduplication, atomic multi-file editor, affected-test selector + multi-language error distiller, `--profile full` (62 tools, `-73%` schema bytes) and `--profile agent-os` (22 super-tools, `-89%` schema bytes), plus 5 new Win32 GUI views (`25..29`).

---

## [2.1.0] — Universal Deduplication & Reversible Organization Suite (`fs_organize`), MFT/USN Tier A & Projectless MCP

### Transactional Organization & Deduplication (`src/fs_organize.cpp`, `src/usn.cpp`, `src/mcp.cpp`)
- **3-Stage Content Deduplicator (`find_duplicates`)**: `size -> 64 KiB -> full SHA-256` pipeline backed by `fs_hashes` with OneDrive cloud-placeholder awareness (`skip_cloud_only: true`) and cross-directory `tree_overlap` matrix.
- **Archive vs. Directory Verification & Topology Safety Map (`verify_archive_vs_dir`, `inspect_workspace_topology`)**: Streaming `libzip` CRC32/size verifier and pre-organization topology inspector for Git repositories, dirty worktrees, NTFS junctions/symlinks, OneDrive placeholders, and build folders.
- **2-Phase Transactional Reorganization (`organize_plan`, `organize_apply`, `organize_undo`, `organize_verify`)**: Resumable `journal.jsonl` WAL (`intent`/`committed`), SHA-256 quarantine manifest, cross-volume verified moves, idempotent undo, and 3-gate conservation verification.
- **Tier A Elevated MFT/USN Replay & Projectless MCP Startup**: Added `FSCTL_ENUM_USN_DATA` / `FSCTL_READ_USN_JOURNAL` 4-phase topological delta replay, Win32 `\\?\` extended-length path handling for trailing spaces/dots, and projectless MCP server startup (`migratool-cli-0.3.exe mcp` without `--project`).

---

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

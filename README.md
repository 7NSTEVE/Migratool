# Migratool v3.1.0 — AI Agent Project OS & Multi-Agent Coordinator

**Native Windows C++23 Project OS, Multi-Agent Worktree/Build/Audit Coordinator, Ultra-Scale Filesystem & Code Intelligence Engine (MCP + CLI + Win32 GUI)**

Migratool v3.1.0 is a local-first, zero-cloud, native Windows C++23 platform engineered specifically to give AI coding agents and software engineers durable memory, crash-proof continuity, fail-closed multi-agent worktree/build governance, real `libclang` continuous technical audit, epistemic knowledge provenance, and massive token/latency savings. It unifies five workloads in a single SQLite/WAL + FTS5 engine:

1. **AI Agent Project OS (`v3.0.0` — 13 Super-Tools & 2 MCP Profiles)** —
   - **`agent_wake` & `delta_since`**: Single-call orientation (`< 25 ms`) that reconciles workstreams after crashes/power cuts/quota resets, verifies live disk SHA-256 and Git HEAD (`CONFIRMED` / `STALE` / `MODIFIED_EXTERNALLY`), surfaces tree-scan freshness, proactive knowledge warnings, Task DAG `ready_tasks`, and 7 telemetry metrics (`BM1..BM7`).
   - **`workstream`**: Macro session state machine + 7-stage per-operation WAL (`PREPARED -> BACKED_UP -> APPLYING -> APPLIED -> INDEXED -> COMMITTED -> RECOVERY`), multi-agent file lock conflict detection (`WORKSTREAM_FILE_CONFLICT`), `CONFLICT_EXTERNAL_EDIT` takeover protection, `idempotency_ledger`, and P4 pinned content-addressed snapshots with `ref_count == 0` Recycle Bin GC.
   - **`knowledge` & `docs_hub`**: Epistemic ledger (`finding`, `method_tried`, `decision`, `implementation`, `pitfall`, `progress`) distinguishing `verified_fact` / `test_result` vs. `agent_hypothesis` / `stale` with automatic FRN/SHA-256 invalidation and proactive warning injection into `read_bundle`; plus H1–H4 TOC indexing, surgical `read_section`, and `drift_report`.
   - **`path_history`, `git_intel` & `env_tools`**: Immutable FRN path lineage with confidence levels (`exact_transactional`, `usn_exact`, `reconciled_inferred`, `journal_gap`), `FILE_MOVED_HINT`, cached Git intelligence (`< 5 ms`), and projectless host toolchain registry (`%LOCALAPPDATA%\Migratool\env_registry.sqlite`, `D:\DevTools` + PATH shadow doctor `PATH_SHADOWED` / `DUPLICATE_VERSION`, recipes `msvc_x64` / `msys2_ucrt64` / `jdk21`).
   - **`repo_map`, `read_bundle`, `batch_edit`, `run_verified`, `impact_analysis`**: Personalized PageRank repo maps, 25-file batch reader with `known_sha256` deduplication, 7-stage atomic multi-file editing, affected-test selector (`select_tests`) + multi-language error distiller, and multi-source impact graph.
   - **Multi-Agent Worktree, Integration, Build Provenance, Safe Storage & Continuous Technical Audit Coordinator (`v3.1.0` — 8 Super-Tools)**: `worktree_inventory` & `project_resume` (4-state lifecycle `IMPLEMENTED -> CONFIRMED -> INTEGRATED -> DELIVERED`, safe `remove_worktree`), `integrate_ready` (non-destructive `git merge-tree --write-tree` + transactional `merge_execute`), `build_canonical` (`.migratool/builds/<build_id>/manifest.json` + CMake preset execution + PID lock + $O(1)$ `trace_binary`), `storage_audit` (9 configurable directory categories + 4 conservation tiers + storage quotas), `discover_replicas` (`MAIN_PROJECT | RELATED_WORKTREE | POSSIBLE_CLONE | ORPHAN_WORK | OFFLINE_DRIVE`), `project_health` (6-question `PRODUCT_REQUIREMENTS_MASTER` traceability & executive supervision summary), and `code_audit` (incremental AST & header-dependency audit, C vs C++23 rules, test-aware security triage `PENDING_VALIDATION`, and anti-hallucination `verify_claim`).
   - **MCP Schema Compaction (`BM1`)**: `--profile full` (`<= 22,000 B`, 70 tools) and `--profile agent-os` (`<= 9,000 B`, 25 core super-tools).
2. **Ultra-Scale Filesystem & Archive Engine (6,000,000+ entries)** — Real-time and offline NTFS File Reference Number (`FRN`) tracking, instant hierarchical tree rollups (`< 10 ms`), trigram FTS5 filename/path search (`< 25 ms`), OneDrive/WOF cloud-placeholder discrimination, Win32 `\\?\` support for trailing-space/long paths, and 4-phase topological USN Journal replay without full rescans.
3. **Universal Deduplication & Reversible Organization Suite (`fs_organize`, M0 Hardened)** — Content deduplication (`size -> 64 KiB -> SHA-256`), libzip CRC32 archive verification, Git fail-closed topology inspection, strict root confinement (`A01`), 2-phase WAL journal (`intent`/`committed`), cross-volume verified SHA-256 moves, idempotent undo, and 3-gate verification.
4. **General Workspace & Super-Serena Agent Toolkit** — Zero-config external workspace activation (`workspace_kind: production | scratch | test_fixture`), projectless MCP startup, federated read-only queries, persistent FTS5 memories, structural AST search/rewrite, and lazy hybrid LSP bridge wired to `env_registry` with UTF-16 safety and Win32 Job Objects.
5. **Auditable Cross-Language Migration Control & Apple HIG Win32 GUI (34 Views)** — Semantic indexing (`libclang` + Roslyn), symbol-level SHA-256 tracking, differential verification cases, immutable SQLite audit history, 5 Agent OS views (`25: Workstreams`, `26: Linaje de rutas`, `27: Conocimiento`, `28: Docs Hub`, `29: Lenguajes y herramientas`), and 4 Multi-Agent Coordinator & Audit views (`30: Worktrees`, `31: Compilaciones`, `32: Almacenamiento`, `33: Auditoría Técnica`) under WCAG AAA contrast.

---

## Key Capabilities (v3.1.0)

### 1. Ultra-Scale 6M+ Filesystem & Archive Index (`archive` / `workspace` modes)
- **Compact Normalized FRN Schema (v2)**: `fs_roots`, `fs_extensions`, `fs_nodes` keyed by `(root_id, frn)`, `fs_dirs` (`WITHOUT ROWID`) with precomputed bottom-up recursive counts/bytes/top extensions, `fs_hashes` (persistent partial + full SHA-256 cache invalidated by `size`/`mtime_ns`), external-content `fs_name_fts` + `fs_dir_fts` (`tokenize='trigram'`), and bounded `fs_change_log` ring table (100,000 events).
- **Streaming Multi-Threaded Win32 Crawler**: 8-thread `FileIdBothDirectoryInfo` enumerator with deterministic race-free directory rollups, 50,000-row transactions, deferred secondary index/FTS5 build, and `< 200 MB` peak RAM even across millions of entries.
- **NTFS Reparse Tag & Path Correctness**: Distinguishes true symlinks/junctions (`IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`) from OneDrive Cloud Filter (`0x9000*`) / WOF tags, and applies Win32 `\\?\` extended-length paths automatically for long paths (`>= 248` chars) or path segments ending in spaces/dots.
- **Automatic Change Detection (No Lost Moves or Self-Trigger Loops)**:
  - **Tier A (Elevated)**: NTFS MFT (`FSCTL_ENUM_USN_DATA`) & 4-phase topological USN Journal delta replay (`FSCTL_READ_USN_JOURNAL`) with live FRN path verification, `CLOSE`-only noise filtering, cross-boundary move fallback to incremental reconcile, and exact `fs_dirs` rollup rebuild.
  - **Tier B (Zero-Admin)**: 8 MB overlapped `ReadDirectoryChangesExW(ReadDirectoryNotifyExtendedInformation)` providing `FileId` + `ParentFileId` on every live event, plus hierarchical `dir_mtime_ns` + FRN offline reconciler.
  - **$O(1)$ Subtree Renames/Moves**: Renaming or moving a folder with 250,000+ child files updates 1 node in `fs_nodes` + prefix-swaps `fs_dirs` without touching child file rows.
  - **Hard Anti-Self-Trigger Filter**: Ignores `.migratool`, external `state_dir`, `*-wal`, `*-shm`, `*-journal`, `.avhdx`, `.vhdx`, `.tmp-*`, and `.new-*`.
- **Archive vs. Workspace Semantics**:
  - `mode: "archive"`: Total forensic census (indexes hidden files, `.git`, extensionless files, empty directories, and symlinks/junctions with targets without loop recursion; ignores `.gitignore`).
  - `mode: "workspace"`: Software development mode (respects `.gitignore` and standard build/vendor exclusions).
  - External catalog state stored under `%LOCALAPPDATA%\Migratool\workspaces\<slug>-<hash>\` by default so indexed root directories remain untouched.

### 2. Universal Deduplication & Reversible Organization (`fs_organize`)
- **`find_duplicates`**: 3-stage deduplicator (`size -> 64 KiB -> full SHA-256`) backed by `fs_hashes`, skipping OneDrive cloud-only placeholders by default (`skip_cloud_only: true`), returning duplicate groups, reclaimable bytes, and a cross-directory `tree_overlap` matrix.
- **`verify_archive_vs_dir`**: Entry-by-entry `libzip` comparison of `.zip` archives against a target directory by size and CRC32 (with configurable `ignore_suffixes`, e.g. `.git/index`).
- **`inspect_workspace_topology`**: Single-call pre-reorganization safety map listing Git repos, dirty counts, valid/broken worktrees, NTFS junctions/symlinks, OneDrive cloud-only files, regenerable build directories (annotated with Git tracked/ignored counts), and absolute-path text references.
- **`organize_plan` / `organize_apply` / `organize_undo` / `organize_verify`**: End-to-end transactional reorganization workflow — validates safety invariants (blocks Git-tracked moves to quarantine by default, blocks junction traversal, detects collisions), moves via `MoveFileExW` without overwrites, writes a SHA-256 quarantine manifest and resumable `journal.jsonl`, reverses completely on `organize_undo`, and audits 1-to-1 file conservation + 100% manifest SHA-256 + Git integrity on `organize_verify`.

### 3. Super-Serena Agent Toolkit & Multi-Workspace Federation
- **Workspace Management**: `activate_workspace`, `list_workspaces` (supports `available_only: true` and `action: "prune"` with `dry_run`), `query_workspace` (read-only federated queries across any registered workspace without switching active state), and `onboard_workspace`.
- **Persistent Project Memories (`memory`)**: SQLite FTS5 + Markdown mirror (`state_dir/memories/<name>.md`) with `action = write | read | edit | list | search | rename | delete` and automatic NTFS `FRN` staleness detection on `linked_paths`.
- **Surgical File & Pattern Toolkit**:
  - `read_file`: line slices or automatic symbol windowing with SHA-256 verification, binary detection, and `token_budget` enforcement.
  - `create_file` / `edit_file`: `replace_content`, `replace_lines`, `insert_at_line`, `delete_lines`, and multi-file `replace_in_files` (literal or regex, `expected_sha256`, `dry_run`, and **atomic multi-file rollback**).
  - `search_pattern`: native multi-threaded C++23 literal/regex content search over indexed files with context lines, globs, automatic binary skipping, and columnar output (`format: "columnar"`).
- **Structural AST Engine**: Disambiguates call sites from declarations, comments, and string literals (`0` false positives on structural search and rewrite).
- **Hybrid Lazy LSP Bridge (30+ Languages)**: Native C++23 JSON-RPC 2.0 stdio LSP supervisor with Win32 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (zero orphan processes), idle auto-reaping (`0 MB` RAM when idle), auto-discovery of installed servers (`clangd`, `basedpyright`/`pyright`/`pylsp`, `typescript-language-server`, `rust-analyzer`, `gopls`, `dart`, `csharp-ls`, plus custom `lsp_servers` config), and transparent fallback to Migratool's native index (`get_diagnostics`, `find_declaration`, `find_implementations`, `lsp_rename_symbol`, `restart_language_server`).

### 4. Automatic & On-Demand Self-Housekeeping (`housekeep`)
- **Zero Unbounded Growth**: New catalogs use `PRAGMA auto_vacuum=INCREMENTAL`, `page_size=16384`, and `journal_size_limit=64 MiB`.
- **Background Auto-Housekeeping**: `IndexWatcher` automatically runs a non-blocking maintenance pass every 30 minutes (`housekeep_interval_ms`):
  - Bounds `fs_change_log` (100K), `requests` (20K), `connections` (1K closed), `watchers` (1K closed), `fs_errors` (10K), finished `inventory_jobs` (10), and legacy `census_entries` generations.
  - Sweeps expired `task_claims` leases and marks crashed dead-PID sessions/connections/watchers as closed.
  - Merges FTS5 trigram segments (`merge`) to prevent B-tree fragmentation.
  - Reclaims free pages (`PRAGMA incremental_vacuum`), checkpoints WAL (`PASSIVE` in background, `TRUNCATE` on-demand), and refreshes planner stats (`PRAGMA optimize`).
  - Safely sends stale internal staging temps (`*.tmp-*`, `*.new-*`) and expired edit rollback backups to the Windows Recycle Bin (never permanent deletion, never touches `catalog.sqlite*` or `memories/`).
  - Auto-trims the global registry (`sessions` and `activity`).
- **On-Demand Tool**: `housekeep(action: "status" | "run" | "vacuum", dry_run, deep, compact_full, prune_missing_projects)`.

### 5. Concurrent Projectless MCP Server (70 Tools / 2 Profiles)
- Supports MCP protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25` over stdio.
- **Projectless Startup**: `migratool-cli-0.3.exe mcp` launches cleanly without `--project`; agents can immediately call `list_workspaces`, `activate_workspace`, or `query_workspace` (or pass explicit `project` per call).
- **Compact Per-Tool Schemas**: Each tool advertises only its relevant input properties (`additionalProperties: true`), saving MCP context tokens on `tools/list`.
- **Concurrent Read Pool**: Up to **4 simultaneous read-only queries** execute in parallel over SQLite WAL while write operations serialize safely.
- **Token-Saving Output Modes**: `format: "columnar"`, `flat: true` on `explore_tree`, `preset: "minimal" | "agent"`, explicit `fields` projection, cursor pagination, and `token_budget` with progressive subtree trimming and exact `returned` counts.

---

## Verified Benchmarks

### 4-Way Competitor Benchmark (`tests/benchmark_competitors.py`, `tiktoken o200k_base`)

| Tool | Task Completion (5 tasks) | False Positives | Tool Calls | Total Output (`o200k` tokens) | Token Reduction vs Full-File/Grep |
|---|---:|---:|---:|---:|---:|
| **Migratool v2.0.0** | **100.0% (5/5)** | **0** | **5** | **2,341** | **-70.94%** |
| GitHub Serena (`oraios/serena`) | 60.0% (3/5) | 5 | 9 | 2,490 | -69.09% |
| `ast-grep` CLI | 40.0% (2/5) | 0 | 9 | 3,185 | -60.46% |
| Full-File + Grep Baseline | 60.0% (3/5) | 0 | 20 | 8,055 | 0.00% |

### Ultra-Scale Corpus Benchmarks (Real NTFS Volumes)

| Metric | 1,000,000 Synthetic Stress Test (`CTest`) | 5,803,596 Real Archive Corpus (`456.87 GB`) |
|---|---:|---:|
| Initial Full Index | 16.06 s (2.72 s insert + 13.34 s FTS5) | **5.18 min** (1.64 min crawl + 3.00 min FTS5) |
| Catalog SQLite Size | **185.98 MB** | **1.39 GiB** (~250 bytes/entry incl. trigram FTS5) |
| `find_files` (ext / columnar) | `1 ms` | **`0.90 ms`** (SQLite internal) |
| `find_files` (trigram substring) | `1 ms` | **`4.97 – 5.17 ms`** (SQLite internal) |
| `explore_tree` (depth=2 rollups) | `21 ms` | **`2.95 ms / folder`** (`76.8 ms` for 26 folders) |
| 250,000-Child Folder Rename | **`375 ms`** ($O(1)$ file touches) | Live $O(1)$ via FRN / USN delta (`503 ms` CLI) |

---

## Building from Source (Windows x64)

### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 / Build Tools with **MSVC C++23** (`cl.exe`)
- **CMake 3.25+** and **Ninja**
- **vcpkg** packages (`x64-windows`): `nlohmann-json`, `sqlite3` (ensure `vcpkg` is on `PATH` or set `VCPKG_ROOT`)
- **libclang.dll** (LLVM/Clang on `PATH` or `LLVM_DIR`; LLVM C headers are bundled under `third_party/llvm-headers/` with Apache-2.0 LLVM license)
- **libzip** (`zip.h` + `libzip.dll` via MSYS2 UCRT64 or vcpkg; set `MSYS2_ROOT` or put on `PATH`)
- **.NET 10 SDK** (for the local Roslyn C# semantic adapter under `adapters/roslyn/`)

### Build & Test Commands
From an **x64 Native Tools Command Prompt for VS**:

```powershell
cmake --preset release
cmake --build --preset release --parallel 8
ctest --preset release --output-on-failure
```

Or into a custom build directory:

```powershell
cmake -S . -B build/v2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/v2 --config Release --parallel 8
ctest --test-dir build/v2 --output-on-failure
```

This builds:
- `build/<dir>/bin/migratool-cli-0.3.exe` (and synchronized alias `migratool-cli.exe`) — CLI & MCP stdio server
- `build/<dir>/bin/Migratool-0.3.exe` — Native Win32 GUI
- 18 automated CTest suites covering core engine, gateway, durable scale, live sync, GUI lifecycle, ultra-scale 1M stress tests, Serena workspace/memories/toolkit, LSP bridge, self-housekeeping, and `fs_organize` + USN/OneDrive/trailing-space regressions.

---

## Quick Start

### 1. MCP Server Configuration (Projectless or Bound)
Add Migratool to your MCP client configuration (see [examples/integration/mcp-client.json](examples/integration/mcp-client.json)). As of v2.1.0, `--project` is optional:

```json
{
  "mcpServers": {
    "migratool": {
      "command": "<PATH_TO_MIGRATOOL>/build/delivery/bin/migratool-cli-0.3.exe",
      "args": ["mcp"]
    }
  }
}
```

### 2. Zero-Config Workspace, Deduplication & Organization (CLI — PowerShell 5.1 Safe)

> **PowerShell 5.1 tip:** Windows PowerShell 5.1 strips inner double quotes inside `--json '{...}'`. Prefer **direct CLI flags**, **`--json-stdin`** (piped JSON), or **`--input file.json`**.

```powershell
# Index a large archive without writing any files into its root (direct flags)
.\build\delivery\bin\migratool-cli-0.3.exe agent activate --path "D:/ArchiveCorpus" --name "ArchiveCorpus" --mode archive

# Explore directory rollups in milliseconds (flat columnar table)
.\build\delivery\bin\migratool-cli-0.3.exe agent explore --project <workspace.json> --depth 2 --sort bytes --flat --format columnar

# Trigram substring or extension search via direct flags or stdin JSON
.\build\delivery\bin\migratool-cli-0.3.exe agent find --project <workspace.json> --query "manifest" --format columnar
'{"ext":".dll","limit":50,"format":"columnar"}' | .\build\delivery\bin\migratool-cli-0.3.exe agent find --project <workspace.json> --json-stdin

# Universal deduplication, workspace topology & ZIP-vs-directory verification
.\build\delivery\bin\migratool-cli-0.3.exe agent duplicates --project <workspace.json> --min-bytes 1024 --limit 50
.\build\delivery\bin\migratool-cli-0.3.exe agent topology --project <workspace.json>
.\build\delivery\bin\migratool-cli-0.3.exe agent verify-zip --project <workspace.json> --zip "backup.zip" --target-dir "extracted_dir"

# Transactional reorganization: plan -> apply -> verify (or undo)
.\build\delivery\bin\migratool-cli-0.3.exe agent organize-plan --project <workspace.json> --input plan_req.json
.\build\delivery\bin\migratool-cli-0.3.exe agent organize-apply --project <workspace.json> --plan-id <id> --execute
.\build\delivery\bin\migratool-cli-0.3.exe agent organize-verify --project <workspace.json> --plan-id <id>
.\build\delivery\bin\migratool-cli-0.3.exe agent organize-undo --project <workspace.json> --plan-id <id> --execute

# Check or run self-housekeeping & prune dead workspaces
.\build\delivery\bin\migratool-cli-0.3.exe agent workspaces --available-only
.\build\delivery\bin\migratool-cli-0.3.exe agent housekeep --project <workspace.json> --action status
```

### 3. Documentation & AI Agent Skill
- [AI Agent Skill (`.agents/skills/migratool/SKILL.md`)](.agents/skills/migratool/SKILL.md) — Drop-in skill file teaching AI agents the zero-shell workflow for all 70 MCP tools (`25` in `--profile agent-os`).
- [Master Product Requirements & Traceability (`docs/PRODUCT_REQUIREMENTS_MASTER.md`)](docs/PRODUCT_REQUIREMENTS_MASTER.md) — Authoritative specification and machine-readable traceability catalog ([`docs/PRODUCT_REQUIREMENTS_MASTER.json`](docs/PRODUCT_REQUIREMENTS_MASTER.json)).
- [Agent Protocol Reference (`docs/AGENT_PROTOCOL.md`)](docs/AGENT_PROTOCOL.md)
- [GUI Navigation Guide (`docs/INTERFAZ_NAVEGACION_Y_USO.md`)](docs/INTERFAZ_NAVEGACION_Y_USO.md)
- [Integration Examples (`examples/integration/`)](examples/integration/)

---

## License

MIT License — see [LICENSE](LICENSE). Bundled LLVM C headers in `third_party/llvm-headers/` are licensed under Apache-2.0 with LLVM Exception.

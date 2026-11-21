# Migratool v2.0

**Native Windows C++23 Ultra-Scale Code & Filesystem Intelligence Engine (MCP + CLI + Win32 GUI)**

Migratool is a local-first, zero-cloud, native Windows C++23 platform designed for AI coding agents and software engineers. It unifies three workloads in a single SQLite/WAL + FTS5 engine:

1. **Ultra-Scale Filesystem & Archive Engine (6,000,000+ entries)** — Real-time and offline NTFS File Reference Number (`FRN`) tracking, instant hierarchical tree rollups (`< 10 ms`), trigram FTS5 filename/path search (`< 25 ms`), and $O(1)$ directory move/rename updates without full rescans.
2. **General Workspace & Super-Serena Agent Toolkit** — Zero-config external workspace activation (never pollutes protected roots), cross-workspace federated read-only queries, persistent FTS5 project memories with Markdown mirrors and NTFS move/delete link tracking, atomic multi-file surgical editing with rollback, structural AST pattern search/rewrite (`0` false positives), and a lazy hybrid LSP bridge for 30+ languages with Win32 Job Object lifecycle management.
3. **Auditable Cross-Language Migration Control** — Semantic indexing (`libclang` + Roslyn `CSharpCompilation` + multi-language structural scanners), symbol-level SHA-256 tracking, differential process/file/SQLite verification cases, and immutable SQLite audit history.

---

## Key Capabilities (v2.0.0)

### 1. Ultra-Scale 6M+ Filesystem & Archive Index (`archive` / `workspace` modes)
- **Compact Normalized FRN Schema**: `fs_roots`, `fs_extensions`, `fs_nodes` keyed by `(root_id, frn)`, `fs_dirs` (`WITHOUT ROWID`) with precomputed bottom-up recursive counts/bytes/top extensions, external-content `fs_name_fts` + `fs_dir_fts` (`tokenize='trigram'`), and bounded `fs_change_log` ring table (100,000 events).
- **Streaming Multi-Threaded Win32 Crawler**: 8-thread `FileIdBothDirectoryInfo` enumerator with 50,000-row transactions, deferred secondary index/FTS5 build, and `< 200 MB` peak RAM even across millions of entries.
- **Automatic Change Detection (No Lost Moves or Self-Trigger Loops)**:
  - **Tier A (Elevated)**: NTFS MFT (`FSCTL_ENUM_USN_DATA`) & unlimited-batch USN Journal delta replay (`FSCTL_READ_USN_JOURNAL`).
  - **Tier B (Zero-Admin)**: 8 MB overlapped `ReadDirectoryChangesExW(ReadDirectoryNotifyExtendedInformation)` providing `FileId` + `ParentFileId` on every live event, plus hierarchical `dir_mtime_ns` + FRN offline reconciler.
  - **$O(1)$ Subtree Renames/Moves**: Renaming or moving a folder with 250,000+ child files updates 1 node in `fs_nodes` + prefix-swaps `fs_dirs` without touching child file rows.
  - **Hard Anti-Self-Trigger Filter**: Ignores `.migratool`, external `state_dir`, `*-wal`, `*-shm`, `*-journal`, `.avhdx`, `.vhdx`, `.tmp-*`, and `.new-*`.
- **Archive vs. Workspace Semantics**:
  - `mode: "archive"`: Total forensic census (indexes hidden files, `.git`, extensionless files, empty directories, and symlinks/junctions with targets without loop recursion; ignores `.gitignore`).
  - `mode: "workspace"`: Software development mode (respects `.gitignore` and standard build/vendor exclusions).
  - External catalog state stored under `%LOCALAPPDATA%\Migratool\workspaces\<slug>-<hash>\` by default so indexed root directories remain untouched.

### 2. Super-Serena Agent Toolkit & Multi-Workspace Federation
- **Workspace Management**: `activate_workspace`, `list_workspaces`, `query_workspace` (read-only federated queries across any registered workspace without switching active state), and `onboard_workspace`.
- **Persistent Project Memories (`memory`)**: SQLite FTS5 + Markdown mirror (`state_dir/memories/<name>.md`) with `action = write | read | edit | list | search | rename | delete` and automatic NTFS `FRN` staleness detection on `linked_paths`.
- **Surgical File & Pattern Toolkit**:
  - `read_file`: line slices or automatic symbol windowing with SHA-256 verification, binary detection, and `token_budget` enforcement.
  - `create_file` / `edit_file`: `replace_content`, `replace_lines`, `insert_at_line`, `delete_lines`, and multi-file `replace_in_files` (literal or regex, `expected_sha256`, `dry_run`, and **atomic multi-file rollback**).
  - `search_pattern`: native multi-threaded C++23 literal/regex content search over indexed files with context lines, globs, automatic binary skipping, and columnar output (`format: "columnar"`).
- **Structural AST Engine**: Disambiguates call sites from declarations, comments, and string literals (`0` false positives on structural search and rewrite).
- **Hybrid Lazy LSP Bridge (30+ Languages)**: Native C++23 JSON-RPC 2.0 stdio LSP supervisor with Win32 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (zero orphan processes), idle auto-reaping (`0 MB` RAM when idle), auto-discovery of installed servers (`clangd`, `basedpyright`/`pyright`/`pylsp`, `typescript-language-server`, `rust-analyzer`, `gopls`, `dart`, `csharp-ls`, plus custom `lsp_servers` config), and transparent fallback to Migratool's native index (`get_diagnostics`, `find_declaration`, `find_implementations`, `lsp_rename_symbol`, `restart_language_server`).

### 3. Automatic & On-Demand Self-Housekeeping (`housekeep`)
- **Zero Unbounded Growth**: New catalogs use `PRAGMA auto_vacuum=INCREMENTAL`, `page_size=16384`, and `journal_size_limit=64 MiB`.
- **Background Auto-Housekeeping**: `IndexWatcher` automatically runs a non-blocking maintenance pass every 30 minutes (`housekeep_interval_ms`):
  - Bounds `fs_change_log` (100K), `requests` (20K), `connections` (1K closed), `watchers` (1K closed), `fs_errors` (10K), finished `inventory_jobs` (10), and legacy `census_entries` generations.
  - Sweeps expired `task_claims` leases and marks crashed dead-PID sessions/connections/watchers as closed.
  - Merges FTS5 trigram segments (`merge`) to prevent B-tree fragmentation.
  - Reclaims free pages (`PRAGMA incremental_vacuum`), checkpoints WAL (`PASSIVE` in background, `TRUNCATE` on-demand), and refreshes planner stats (`PRAGMA optimize`).
  - Safely sends stale internal staging temps (`*.tmp-*`, `*.new-*`) and expired edit rollback backups to the Windows Recycle Bin (never permanent deletion, never touches `catalog.sqlite*` or `memories/`).
  - Auto-trims the global registry (`sessions` and `activity`).
- **On-Demand Tool**: `housekeep(action: "status" | "run" | "vacuum", dry_run, deep, compact_full, prune_missing_projects)`.

### 4. Concurrent MCP Server (42 Tools)
- Supports MCP protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25` over stdio.
- **Concurrent Read Pool**: Up to **4 simultaneous read-only queries** execute in parallel over SQLite WAL while write operations serialize safely.
- **Token-Saving Output Modes**: `format: "columnar"`, `preset: "minimal" | "agent"`, explicit `fields` projection, cursor pagination, and `token_budget`.

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
- `build/<dir>/bin/migratool-cli-0.3.exe` — CLI & MCP stdio server
- `build/<dir>/bin/Migratool-0.3.exe` — Native Win32 GUI
- 17 automated CTest suites covering core engine, gateway, durable scale, live sync, GUI lifecycle, ultra-scale 1M stress tests, Serena workspace/memories/toolkit, LSP bridge, and self-housekeeping.

---

## Quick Start

### 1. MCP Server Configuration
Add Migratool to your MCP client configuration (see [examples/integration/mcp-client.json](examples/integration/mcp-client.json)):

```json
{
  "mcpServers": {
    "migratool": {
      "command": "<PATH_TO_MIGRATOOL>/build/delivery/bin/migratool-cli-0.3.exe",
      "args": ["mcp", "--project", "<PATH_TO_PROJECT_OR_WORKSPACE_JSON>"]
    }
  }
}
```

### 2. Zero-Config Workspace or Archive Activation (CLI)

```powershell
# Index a large archive without writing any files into its root
.\build\v2\bin\migratool-cli-0.3.exe agent activate --json '{"path":"D:/ArchiveCorpus","name":"ArchiveCorpus","mode":"archive"}'

# Explore directory rollups in milliseconds
.\build\v2\bin\migratool-cli-0.3.exe agent explore --project <workspace.json> --json '{"depth":2,"sort":"bytes"}'

# Trigram substring or extension search in columnar format
.\build\v2\bin\migratool-cli-0.3.exe agent find --project <workspace.json> --json '{"query":"manifest","format":"columnar"}'

# Check or run self-housekeeping
.\build\v2\bin\migratool-cli-0.3.exe agent housekeep --project <workspace.json> --json '{"action":"status"}'
.\build\v2\bin\migratool-cli-0.3.exe agent housekeep --project <workspace.json> --json '{"action":"run"}'
```

### 3. Documentation & AI Agent Skill
- [AI Agent Skill (`.agents/skills/migratool/SKILL.md`)](.agents/skills/migratool/SKILL.md) — Drop-in skill file teaching AI agents the zero-shell workflow for all 42 MCP tools.
- [Agent Protocol Reference (`docs/AGENT_PROTOCOL.md`)](docs/AGENT_PROTOCOL.md)
- [GUI Navigation Guide (`docs/INTERFAZ_NAVEGACION_Y_USO.md`)](docs/INTERFAZ_NAVEGACION_Y_USO.md)
- [Integration Examples (`examples/integration/`)](examples/integration/)

---

## License

MIT License — see [LICENSE](LICENSE). Bundled LLVM C headers in `third_party/llvm-headers/` are licensed under Apache-2.0 with LLVM Exception.

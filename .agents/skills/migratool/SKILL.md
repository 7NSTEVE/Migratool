---
name: migratool
description: >
  Flujo de trabajo óptimo para agentes de IA usando las 49 herramientas MCP y CLI de
  Migratool v2.1.0: Motor Ultra-Escala para 6M+ archivos/carpetas con auto-seguimiento NTFS FRN,
  Deduplicación Universal + Reorganización Transaccional Reversible (`fs_organize`),
  Autoaseo automático y bajo demanda (`housekeep`), Modo Workspace General sin proyecto fijo +
  Federación Multi-Proyecto, Memorias Persistentes FTS5, Toolkit Quirúrgico con Rollback,
  AST Estructural (0 falsos positivos), Puente LSP Híbrido (30+ lenguajes) y Migraciones Verificadas
  con mínimo consumo de comandos, tokens y contexto.
---

# Migratool v2.1 — Guía de Eficiencia Extrema para Agentes de IA

## Regla de Oro: Política Zero-Shell

Cuando el servidor MCP `migratool` esté activo (incluso sin `--project` fijo):
- **NO ejecutes** `run_command` con PowerShell (`Get-ChildItem`, `Select-String`, `Get-Content`), Python (`os.walk`, `hashlib`, `zipfile`, `sqlite3`) ni `grep` para explorar árboles gigantes, buscar duplicados, auditar repos/worktrees, reorganizar carpetas, leer código o refactorizar.
- **USA directamente** las 49 herramientas MCP de `migratool` (o `migratool-cli-0.3.exe agent <subcomando>` solo si MCP no está disponible).
- **Lecturas concurrentes activas:** hasta 4 consultas de solo lectura corren en paralelo sobre SQLite WAL sin bloquearse mutuamente.
- **Nota crítica para PowerShell 5.1 (si usas CLI):** Windows PowerShell 5.1 elimina comillas dobles internas en `--json '{...}'`. Usa siempre **flags directos** (`--path`, `--depth`, `--query`, `--flat`, `--format`), **`--json-stdin`** (JSON por tubería) o **`--input archivo.json`**.

---

## 1. Flujo Canónico de Organización y Deduplicación Masiva (4 Llamadas)

Ideal para prompts tipo *"ordena, simplifica, deduplica y reorganiza 40,000+ archivos y miles de carpetas sin romper repos Git ni perder nada"*:

1. **Activar sin ensuciar la raíz (`activate_workspace`)**:
   ```json
   {
     "path": "D:/CarpetasDesordenadas",
     "name": "MiCarpeta",
     "mode": "archive",
     "index_tier": "metadata",
     "sync": true,
     "threads": 8
   }
   ```
   - `mode: "archive"`: censo forense total (incluye ocultos, `.git`, carpetas vacías, nombres con espacio final vía `\\?\`, distingue junctions reales vs placeholders de OneDrive; ignora `.gitignore`).
   - Guarda `workspace.json` y `catalog.sqlite` fuera del árbol objetivo (`%LOCALAPPDATA%\Migratool\workspaces\...`).

2. **Diagnóstico completo en paralelo (3 lecturas concurrentes)**:
   - **Panorama del árbol (`explore_tree`)**:
     ```json
     {"path": "", "depth": 2, "sort": "bytes", "flat": true, "format": "columnar", "limit": 50}
     ```
   - **Mapa de seguridad y topología (`inspect_workspace_topology`)**:
     ```json
     {"inspect_git": true, "scan_path_refs": true}
     ```
     Detecta en 1 llamada: repos Git + conteo de sucios, worktrees válidos y rotos, junctions/symlinks NTFS, archivos cloud-only de OneDrive, carpetas de build regenerables (con conteo de archivos rastreados vs ignorados por Git) y referencias a rutas absolutas.
   - **Deduplicación universal (`find_duplicates`)**:
     ```json
     {"min_bytes": 1024, "limit": 100, "skip_cloud_only": true}
     ```
     Ejecuta pipeline nativo en 3 etapas (`tamaño -> 64 KiB -> SHA-256` cacheado en `fs_hashes`) para cualquier tipo de archivo (ZIP, DLL, ROM, PNG, PDF, código) + matriz `tree_overlap`. Si hay archivos `.zip` duplicados con carpetas extraídas, verifica 1-a-1 con **`verify_archive_vs_dir`**.

3. **Planificación dry-run con invariantes de seguridad (`organize_plan`)**:
   ```json
   {
     "plan_id": "limpieza_20261121",
     "quarantine_dir": "D:/_Cuarentena/MiCarpeta",
     "operations": [
       {"op": "quarantine_file", "category": "Q1", "src": "temp.obj", "reason": "build_artifact"},
       {"op": "quarantine_dir", "category": "Q2", "src": "repo/build", "reason": "gitignored_build"},
       {"op": "move_file", "src": "notas.txt", "dst": "Docs/notas.txt"},
       {"op": "move_dir", "src": "scripts_viejos", "dst": "Tools/scripts"},
       {"op": "rmdir_empty_tree", "src": "carpeta_vacia"},
       {"op": "edit_text_paths", "target": "Tools/scripts/run.py", "replacements": [["/scripts_viejos/", "/Tools/scripts/"]]}
     ]
   }
   ```
   Valida invariantes antes de tocar el disco: bloquea mover archivos rastreados por Git a cuarentena (`allow_git_tracked: false` por defecto), bloquea atravesar junctions, detecta colisiones y guarda snapshot base + `plan.jsonl`.

4. **Ejecución transaccional, auditoría y reversión (`organize_apply` -> `organize_verify` / `organize_undo`)**:
   - **Aplicar:** `organize_apply({"plan_id": "limpieza_20261121", "execute": true, "sync_after": true})` — usa `MoveFileExW` (`\\?\`, sin sobrescritura), calcula SHA-256 antes de cuarentenar, escribe `manifest.jsonl` + `journal.jsonl` (reanudable con `resume: true`).
   - **Auditar:** `organize_verify({"plan_id": "limpieza_20261121"})` — verifica conservación 1-a-1 (`raíz + cuarentena == base`), 100% SHA-256 del manifiesto e integridad `HEAD`/estado/worktrees de todos los repos Git.
   - **Revertir si hace falta:** `organize_undo({"plan_id": "limpieza_20261121", "execute": true})` — reproduce `journal.jsonl` en orden inverso y verifica SHA-256 restaurado.

---

## 2. Explorar 6,000,000+ Archivos y Carpetas (`explore_tree` / `find_files` / `changes_feed`)

- **Panorama jerárquico instantáneo (`explore_tree`)**: `< 10 ms`, admite `depth: 1..4`, `sort: "bytes"|"files"|"dirs"|"name"|"mtime"`, `flat: true`, `format: "columnar"`, `extensions_limit`.
- **Búsqueda trigrama FTS5 / extensión / glob (`find_files`)**:
  ```json
  {"query": "bootloader", "limit": 30, "format": "columnar"}
  ```
  ```json
  {"ext": ".dll", "path_prefix": "01_Firmware", "limit": 50, "format": "columnar"}
  ```
- **Auditoría automática de cambios por FRN (`changes_feed`)**:
  ```json
  {"since_seq": 0, "ops": ["moved", "renamed", "created", "deleted"], "limit": 50}
  ```
  Con replay topológico de USN Journal en 4 fases (Tier A elevado) o `ReadDirectoryChangesExW` + reconciliador offline (Tier B sin Admin).

---

## 3. Flujo Super-Serena para Código, Memorias y Multi-Workspace

1. **Orientación automática (`onboard_workspace`)**: detecta estructura, lenguajes y marcadores de build y persiste la memoria `_onboarding`.
2. **Memorias persistentes con FTS5 y rastreo FRN (`memory`)**:
   - `action`: `write | read | edit | list | search | rename | delete`.
   - Pasar `linked_paths: ["src/app.py"]` en `write` detecta automáticamente al leer si el archivo enlazado se movió o borró.
3. **Consultas federadas y mantenimiento de registro (`list_workspaces` / `query_workspace`)**:
   - Filtrar solo accesibles: `list_workspaces({"available_only": true})`.
   - Podar registros obsoletos: `list_workspaces({"action": "prune", "dry_run": true})`.
   - Consultar otro workspace sin cambiar el activo:
     ```json
     {
       "workspace": "ArchiveCorpus",
       "tool": "find_duplicates",
       "arguments": {"min_bytes": 4096, "limit": 20}
     }
     ```
4. **Toolkit quirúrgico con rollback atómico**:
   - `read_file`: rangos de línea (`start_line`/`end_line`) o ventana automática por `symbol`, con `sha256` y detección de binarios.
   - `create_file`: crea archivo autorizado y sincroniza `fs_nodes` al instante.
   - `edit_file`: `mode = replace_content | replace_lines | insert_at_line | delete_lines | replace_in_files` (literal o regex, `expected_sha256`, `dry_run`, rollback automático ante fallo).
   - `search_pattern`: buscador C++23 multi-hilo sobre archivos indexados con `is_regex`, `context_lines`, `include_glob`/`exclude_glob` y `format: "columnar"`.
5. **Semántica Híbrida (AST Nativo 0 ms + Puente LSP 30+ lenguajes)**:
   - AST estructural sin falsos positivos en comentarios/strings ni declaraciones (`query(kind="hybrid_search", ast_pattern=...)`, `edit_symbol(op="ast_rewrite")`).
   - Puente LSP perezoso bajo demanda (`get_diagnostics`, `find_declaration`, `find_implementations`, `lsp_rename_symbol` con rollback atómico, `restart_language_server`) con fallback transparente al índice nativo Roslyn/`libclang`.

---

## 4. Autoaseo y Salud del Catálogo (`housekeep`)

Migratool v2.1 se autolimpia automáticamente en segundo plano cada 30 minutos (`housekeep_interval_ms`), mantiene `PRAGMA auto_vacuum=INCREMENTAL` y `journal_size_limit=64 MiB`, recorta el registro global y acota `fs_change_log`/`requests`/`connections`/`watchers`/`task_claims`.
- **Diagnóstico (solo lectura):** `housekeep({"action": "status"})`
- **Limpieza inmediata (con `dry_run` opcional):** `housekeep({"action": "run", "deep": true, "prune_missing_projects": false})`
- **Compactación completa de disco (`VACUUM` + `INCREMENTAL`):** `housekeep({"action": "vacuum"})`

---

## 5. Flujo Canónico de Migración en 3 Llamadas

1. **Arranque (`session_bootstrap`)**: `{"token_budget": 2500, "limit": 5, "depth": 2}` -> revisión, estado global, `denominator_blockers`, módulos y tareas.
2. **Inspección quirúrgica (`get_task_context`)**: `outline_only: true` para esqueleto o `task_ids: [...]` (hasta 50 símbolos) con cuerpos, callees, callers y candidatos destino.
3. **Cierre atómico (`complete_task`)**: escribe en `target` (`write_files`) o aplica `edits`, auto-sincroniza sin `INDEX_STALE`, mapea (`auto_map_symbols: true`), libera claims y ejecuta `verify_cases`.

---

## Equivalentes CLI (`migratool-cli-0.3.exe` / `migratool-cli.exe` — Seguro en PowerShell 5.1)

```powershell
# Activar y explorar un archivo de millones de entradas sin tocar su raíz (flags directos)
migratool-cli-0.3.exe agent activate --path "D:/ArchiveCorpus" --name "ArchiveCorpus" --mode archive
migratool-cli-0.3.exe agent explore --project <workspace.json> --depth 2 --sort bytes --flat --format columnar
migratool-cli-0.3.exe agent find --project <workspace.json> --query "bootloader" --format columnar
migratool-cli-0.3.exe agent workspaces --available-only

# Deduplicación, topología, verificación ZIP y organización reversible
migratool-cli-0.3.exe agent duplicates --project <workspace.json> --min-bytes 1024 --limit 50
migratool-cli-0.3.exe agent topology --project <workspace.json>
migratool-cli-0.3.exe agent verify-zip --project <workspace.json> --zip "copia.zip" --target-dir "carpeta_extraida"
migratool-cli-0.3.exe agent organize-plan --project <workspace.json> --input plan.json
migratool-cli-0.3.exe agent organize-apply --project <workspace.json> --plan-id limpieza_1 --execute
migratool-cli-0.3.exe agent organize-verify --project <workspace.json> --plan-id limpieza_1
migratool-cli-0.3.exe agent organize-undo --project <workspace.json> --plan-id limpieza_1 --execute

# JSON arbitrario sin corrupción de comillas en PowerShell 5.1
'{"workspace":"ArchiveCorpus","tool":"explore_tree","arguments":{"depth":1,"flat":true}}' | migratool-cli-0.3.exe agent query-ws --json-stdin

# Código, memorias, LSP y autoaseo
migratool-cli-0.3.exe agent onboard --project <workspace.json>
migratool-cli-0.3.exe agent memory --project <workspace.json> --action list
migratool-cli-0.3.exe agent grep --project <workspace.json> --pattern "ExecuteNonQuery" --format columnar
migratool-cli-0.3.exe agent diagnostics --project <workspace.json> --path "src/main.cpp"
migratool-cli-0.3.exe agent housekeep --project <workspace.json> --action status
```

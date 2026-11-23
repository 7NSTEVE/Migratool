---
name: migratool
description: >
  Flujo de trabajo óptimo para agentes de IA usando las 69 herramientas MCP y CLI de
  Migratool v3.1.0 (AI Agent Project OS & Multi-Agent Coordinator): Arranque y orientación en 1 llamada
  (`agent_wake` y `project_resume` con verificación en vivo disco/Git), Coordinación Multi-Agente de
  Worktrees (`worktree_inventory` con 4 estados `IMPLEMENTED..DELIVERED`), Gestor de Integración sin
  tocar el árbol (`integrate_ready` con `git merge-tree`), Manifiestos de Compilación y Trazabilidad
  Inversa Binario->Código (`build_canonical` / `trace_binary`), Auditoría Segura de Almacenamiento en
  4 niveles (`storage_audit`), Descubrimiento de Réplicas y Trabajo Huérfano (`discover_replicas`),
  Trazabilidad Quirúrgica de Requisitos (`project_health` sobre `PRODUCT_REQUIREMENTS_MASTER`),
  Continuidad de Sesiones (`workstream`), Libro Mayor de Conocimiento (`knowledge`), Docs Hub
  (`docs_hub`), Linaje FRN (`path_history`), Toolchains (`env_tools`), `repo_map`, `read_bundle`,
  `batch_edit`, `run_verified`, Motor Ultra-Escala 6M+ archivos y `fs_organize`.
---

# Migratool v3.0 — AI Agent Project OS (Guía de Superpoderes para Agentes)

## Regla de Oro: Arranque en 1 Llamada (`agent_wake`) y Política Zero-Shell

Cuando el servidor MCP `migratool` esté activo (`--profile agent-os` con 22 super-herramientas en `<= 9 KB` de esquema, o `--profile full` con las 62 herramientas en `<= 22 KB`):
1. **Despertar o retomar cualquier proyecto en 1 sola llamada (`agent_wake`)**:
   - Reconcilia workstreams interrumpidos por cortes de energía, cierres o límites de cuota (`INTERRUPTED_RECOVERABLE`) y operaciones de 7 etapas (`PREPARED..RECOVERY`).
   - Verifica en vivo (`< 3 ms`) el `SHA-256` en disco y `.git/HEAD` (`CONFIRMED`, `STALE`, `MODIFIED_EXTERNALLY`).
   - Devuelve cuándo se leyó por última vez el árbol completo (`tree_freshness`), alertas proactivas de `knowledge` (`pitfalls`, `method_tried(failed)`), la cola DAG `ready_tasks` y las 7 métricas `BM1..BM7`.
2. **Ciclo de desarrollo en 4 llamadas (`BM3`)**:
   - `agent_wake` $\rightarrow$ `read_bundle` (hasta 25 archivos con skip por `known_sha256`, `FILE_MOVED_HINT` y `knowledge_warnings`) $\rightarrow$ `batch_edit` (7 etapas durables + `idempotency_key`) $\rightarrow$ `run_verified` (`auto_select_tests: true` + destilador de errores `< 500 B`).
3. **Consultar qué lenguajes, compiladores o SDKs están instalados sin escanear discos (`env_tools`)**:
   - Funciona **con o sin proyecto activo**: `env_tools({"action": "summary"})`, `env_tools({"action": "query", "query": "cmake"})`, `env_tools({"action": "doctor"})` (detecta `PATH_SHADOWED` como `java` Zulu 11 vs. JDK 21 en `D:\DevTools`).

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

## 6. Coordinación Multi-Agente de Worktrees, Integración, Builds Canónicos y Almacenamiento Seguro (`v3.1.0`)

1. **Inventario de Worktrees y Relevo ante Corte de Cuota (`worktree_inventory` & `project_resume`)**:
   - Estados estrictos: `IMPLEMENTED` $\rightarrow$ `CONFIRMED` $\rightarrow$ `INTEGRATED` $\rightarrow$ `DELIVERED`.
   - `worktree_inventory({"action": "scan"})`: descubre todos los worktrees de Git (`git worktree list --porcelain`), cambios sin commit y commits adelantados/atrasados.
   - `project_resume({"task_query": "BUILD"})`: paquete de relevo en 1 llamada con worktrees activos, ramas obligatorias pendientes de integrar, último build canónico y solo los requisitos `REQ-*` relevantes de `docs/PRODUCT_REQUIREMENTS_MASTER.json`.
2. **Evaluación de Integración No Destructiva (`integrate_ready`)**:
   - `integrate_ready({"action": "evaluate", "target_branch": "main"})`: verifica conflictos con `git merge-tree --write-tree` sin modificar el árbol de trabajo y calcula `canonical_build_ready`.
3. **Compilación Canónica y Trazabilidad Inversa Binario $\rightarrow$ Código (`build_canonical`)**:
   - Bloquea declarar cualquier build como `INTEGRATED` o `DELIVERED` si existen worktrees obligatorios en estado `IMPLEMENTED` o `CONFIRMED` sin integrar (salvo exclusión explícita con `excluded_by` y `exclusion_reason`).
   - `build_canonical({"action": "trace_binary", "sha256": "<hash>"})`: identifica en $O(1)$ qué commit, rama, worktree, agente y pruebas produjeron cualquier `.exe` o `.dll`.
4. **Auditoría de Almacenamiento en 4 Niveles y Descubrimiento de Réplicas (`storage_audit` & `discover_replicas` & `project_health`)**:
   - `storage_audit({"action": "audit"})`: clasifica carpetas en 9 categorías configurables y 4 niveles (`PROTECTED`, `TEMP_RETAINED`, `VERIFIED_SAFE_CLEAN`, `NEEDS_REVIEW`), prohibiendo borrar a ciegas por nombre (`old`, `backup`, `temp`, `tests`, `include`, `tools`).
   - `discover_replicas({"action": "discover", "candidate_paths": [...]})`: clasifica copias como `MAIN_PROJECT`, `RELATED_WORKTREE`, `POSSIBLE_CLONE` u `ORPHAN_WORK` e identifica archivos únicos no integrados.
   - `project_health({"action": "summary"})`: resumen ejecutivo de baja carga cognitiva, respuesta a las 6 preguntas de trazabilidad (`action: "query_requirements"`) y separación de `session_pendings` frente a requisitos maestros.
5. **Auditoría Técnica Continua e Incremental y Verificación de Afirmaciones (`code_audit`)**:
   - `code_audit({"action": "scan_incremental"})`: caché incremental por `(file_sha256, compile_flags_sha256, headers_deps_sha256)`, distinción C (`NULL`) vs C++23 (`nullptr`), complejidad ciclomática AST, desambiguación de `TEST_CASE` por línea y triage de seguridad en `tests/` como `PENDING_VALIDATION`.
   - `code_audit({"action": "verify_claim", "actor": "Claude", "module_or_path": "src/...", "claimed_functions_count": 12, "claimed_compiled": true, "claimed_tests_passed": true})`: verifica entregas contra evidencia real en 5 dimensiones separadas (`AST`, `build_manifests`, `verified_runs`, calidad y equivalencia funcional).

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

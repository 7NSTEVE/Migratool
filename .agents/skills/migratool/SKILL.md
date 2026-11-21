---
name: migratool
description: >
  Flujo de trabajo óptimo para agentes de IA usando las 42 herramientas MCP y CLI de
  Migratool v2.0.0: Motor Ultra-Escala para 6M+ archivos/carpetas con auto-seguimiento NTFS FRN,
  Autoaseo automático y bajo demanda (housekeep), Modo Workspace General + Federación Multi-Proyecto,
  Memorias Persistentes FTS5, Toolkit Quirúrgico con Rollback, AST Estructural (0 falsos positivos),
  Puente LSP Híbrido (30+ lenguajes) y Migraciones Verificadas con mínimo consumo de comandos,
  tokens y contexto.
---

# Migratool v2.0 — Guía de Eficiencia Extrema para Agentes de IA

## Regla de Oro: Política Zero-Shell

Cuando el servidor MCP `migratool` esté activo (o exista un workspace/catálogo):
- **NO ejecutes** `run_command` con PowerShell (`Get-ChildItem`, `Select-String`, `Get-Content`), Python (`os.walk`, `sqlite3`) ni `grep` para explorar árboles gigantes, buscar archivos, leer código o refactorizar.
- **USA directamente** las 42 herramientas MCP de `migratool` (o `migratool-cli-0.3.exe agent <subcomando>` solo si MCP no está disponible).
- **Lecturas concurrentes activas:** hasta 4 consultas de solo lectura corren en paralelo sobre SQLite WAL sin bloquearse mutuamente.

---

## 1. Explorar 6,000,000+ Archivos y Carpetas (Modo `archive` / `workspace`)

### Activar cualquier carpeta sin ensuciar su raíz (`activate_workspace`)
Guarda automáticamente `workspace.json` y `catalog.sqlite` fuera del directorio objetivo (`%LOCALAPPDATA%\Migratool\workspaces\...`), respetando raíces protegidas o de solo lectura:

```json
{
  "path": "D:/ArchiveCorpus",
  "name": "ArchiveCorpus",
  "mode": "archive",
  "index_tier": "metadata",
  "sync": true,
  "threads": 8
}
```

- `mode: "archive"`: censo forense total (incluye ocultos, `.git`, carpetas vacías, enlaces sin seguir bucles; ignora `.gitignore` aunque contenga `/*`).
- `mode: "workspace"`: desarrollo de software (respeta `.gitignore` y excluye `node_modules`/`build`).
- Auto-seguimiento NTFS FRN en vivo (`ReadDirectoryChangesExW` 8 MB sin Admin + USN Journal cuando elevado): renombrar o mover una carpeta con 250K+ hijos actualiza rutas en milisegundos sin re-escanear y sin auto-disparo en `.avhdx`/`*-wal`/`*-journal`.

### Navegar y buscar en `< 25 ms`
1. **Panorama jerárquico instantáneo (`explore_tree`)**:
   ```json
   {"path": "", "depth": 2, "sort": "bytes", "limit": 25, "include_samples": false}
   ```
2. **Búsqueda trigrama FTS5 / extensión / glob (`find_files`)**:
   ```json
   {"query": "bootloader", "limit": 30, "format": "columnar"}
   ```
   ```json
   {"ext": ".dll", "path_prefix": "01_Firmware", "limit": 50, "format": "columnar"}
   ```
3. **Auditoría automática de movimientos/altas/bajas (`changes_feed`)**:
   ```json
   {"since_seq": 0, "ops": ["moved", "renamed", "created", "deleted"], "limit": 50}
   ```

---

## 2. Flujo Super-Serena para Código, Memorias y Multi-Workspace

1. **Orientación automática (`onboard_workspace`)**: detecta estructura, lenguajes y marcadores de build y persiste la memoria `_onboarding`.
2. **Memorias persistentes con FTS5 y rastreo FRN (`memory`)**:
   - `action`: `write | read | edit | list | search | rename | delete`.
   - Pasar `linked_paths: ["src/app.py"]` en `write` permite detectar automáticamente al leer si el archivo enlazado se movió o borró.
3. **Consultas federadas entre proyectos (`list_workspaces` / `query_workspace`)**:
   ```json
   {
     "workspace": "ArchiveCorpus",
     "tool": "find_files",
     "arguments": {"query": "manifest", "limit": 20, "format": "columnar"}
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

## 3. Autoaseo y Salud del Catálogo (`housekeep`)

Migratool v2.0 se autolimpia automáticamente en segundo plano cada 30 minutos (`housekeep_interval_ms`), mantiene `PRAGMA auto_vacuum=INCREMENTAL` y `journal_size_limit=64 MiB`, recorta el registro global y acota `fs_change_log`/`requests`/`connections`/`watchers`/`task_claims`. También admite inspección y compactación explícita:
- **Diagnóstico (solo lectura):** `housekeep({"action": "status"})`
- **Limpieza inmediata (con `dry_run` opcional):** `housekeep({"action": "run", "deep": true, "prune_missing_projects": false})`
- **Compactación completa de disco (`VACUUM` + `INCREMENTAL`):** `housekeep({"action": "vacuum"})`

---

## 4. Flujo Canónico de Migración en 3 Llamadas

1. **Arranque (`session_bootstrap`)**: `{"token_budget": 2500, "limit": 5, "depth": 2}` -> revisión, estado global, `denominator_blockers`, módulos y tareas.
2. **Inspección quirúrgica (`get_task_context`)**: `outline_only: true` para esqueleto o `task_ids: [...]` (hasta 50 símbolos) con cuerpos, callees, callers y candidatos destino.
3. **Cierre atómico (`complete_task`)**: escribe en `target` (`write_files`) o aplica `edits`, auto-sincroniza sin `INDEX_STALE`, mapea (`auto_map_symbols: true`), libera claims y ejecuta `verify_cases`.

---

## Equivalentes CLI (`migratool-cli-0.3.exe`)

```powershell
# Activar y explorar un archivo de millones de entradas sin tocar su raíz
migratool-cli-0.3.exe agent activate --json '{"path":"D:/ArchiveCorpus","name":"ArchiveCorpus","mode":"archive"}'
migratool-cli-0.3.exe agent explore --project <workspace.json> --json '{"depth":2}'
migratool-cli-0.3.exe agent find --project <workspace.json> --json '{"query":"bootloader","format":"columnar"}'
migratool-cli-0.3.exe agent workspaces
migratool-cli-0.3.exe agent query-ws --json '{"workspace":"ArchiveCorpus","tool":"explore_tree","arguments":{"depth":1}}'

# Código, memorias, LSP y autoaseo
migratool-cli-0.3.exe agent onboard --project <workspace.json>
migratool-cli-0.3.exe agent memory --project <workspace.json> --json '{"action":"list"}'
migratool-cli-0.3.exe agent grep --project <workspace.json> --json '{"pattern":"ExecuteNonQuery","format":"columnar"}'
migratool-cli-0.3.exe agent diagnostics --project <workspace.json> --json '{"path":"src/main.cpp"}'
migratool-cli-0.3.exe agent housekeep --project <workspace.json> --json '{"action":"status"}'
```

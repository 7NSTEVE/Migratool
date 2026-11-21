# Protocolo de agentes de Migratool

La guía operativa extensa está en el [Manual completo para agentes](docs/MANUAL_AGENTES.md). Incluye una instrucción lista para entregar a un agente, procedimientos de trabajo y recuperación, referencia de parámetros y [solicitudes JSON](examples/integration/agent-playbook.json).

La misma lógica atiende la CLI JSON y MCP stdio con ejecución local. Las consultas del catálogo leen SQLite. El inventario, el contexto con cuerpo, el registro por rutas y la comprobación de hashes pueden leer los archivos del proyecto. Las verificaciones configuradas ejecutan comandos cuando la solicitud incluye autorización explícita.

### Contratos del motor durable en la compilación de validación 1.1.0

Consulte la [guía de trabajo reanudable](docs/MANUAL_AGENTES.md#45-catálogo-durable-y-trabajo-reanudable). La GUI y la CLI deben usar la misma identidad de compilación y el mismo catálogo. Los nombres de archivo de los ejecutables conservan el sufijo 0.3 por compatibilidad.

- `inventory_sync` / `agent sync` admite `wait:false` / `--async`. El acuse incluye `job_id`. `scope` admite `root` y `subdirectory`, `file` o `files`. En el motor durable, `defer_analysis:true` conserva las tareas posteriores al censo.
- `job_status` / `agent jobs` consulta el ID. `job_control` / `agent job` admite `pause`, `resume`, `cancel` y `retry` con actor. Consulte el mismo ID para confirmar el cambio de estado.
- `export_inventory` / `agent export` escribe los archivos activos del catálogo en un destino nuevo. `wait:false` / `--async` devuelve `job_id`; `job_status` entrega la ruta, el conteo y SHA-256 en `result` al finalizar. El archivo conserva un punto de reanudación por cada lote confirmado. Si cambian las filas del catálogo entre lotes, `EXPORT_CATALOG_CHANGED` exige reintentar con el catálogo actual.
- `project_settings` / `agent settings` lee `config_revision` y aplica `settings` mediante `action:set`, `actor` y `expected_config_revision`.
- `acquire_task` / `agent acquire` reserva un archivo pendiente. La respuesta conserva `task_id` y `claim` si hay un fallo posterior al leer el contexto. El agente puede consultar el contexto de nuevo o liberar la reserva.
- `import_existing_work` / `agent reuse` lee catálogos y registros externos por lotes. `external_path` o `--external-path` identifica automáticamente el formato. `catalog_path` y `registry_path` conservan la selección explícita. Use `verify:true` para comprobar las huellas de las rutas existentes. `action:revalidate`, `ids:[...]` y `actor` revalidan de 1 a 1000 candidatos registrados. Los identificadores se obtienen de `query` con `kind:reuse_candidates`. Un cambio concurrente en la misma evidencia rechaza la escritura del lote con `REUSE_EVIDENCE_CONFLICT`. Las notas y correspondencias ajenas se conservan.
- `project_diagnostics` / `agent doctor` informa de la identidad de compilación, la base efectiva y los adaptadores locales.
- `query` admite `directories`, `census_entries`, `scan_errors`, `archive_members` y `reuse_candidates`. Conserve los filtros al usar un cursor.

La respuesta de sincronización distingue el censo publicado, el análisis pendiente y el estado terminal del trabajo. Una respuesta compactada conserva el identificador del trabajo dentro del presupuesto admitido. Use `--budget-tokens` para ajustar la cota de bytes de la respuesta JSON.

## CLI

Ejecutable 0.3: `build\delivery\bin\migratool-cli-0.3.exe`.

La GUI 0.3 descubre los proyectos usados por esta CLI y por sus sesiones MCP mediante `.migratool\projects.sqlite`. `projects` consulta proyectos y sesiones y muestra la ruta efectiva; `activity --project <ruta>` recupera mensajes del proyecto. La variable opcional `MIGRATOOL_REGISTRY_PATH` debe ser idéntica en los clientes y la GUI que deban compartir este registro. La pantalla `Origen ↔ destino` presenta las listas vinculadas y `Agentes en vivo` observa operaciones y resultados. El ciclo de actualización de la GUI es de un segundo, más el tiempo de lectura del catálogo y los commits del escritor. El inventario y las correspondencias siguen en la base de cada proyecto.

Los subcomandos `agent context`, `sync`, `next`, `query`, `batch`, `report`, `status`, `changes`, `history`, `verify`, `statistics` y `connections` requieren `--project <ruta>`. Aceptan `--input <argumentos.json>` y `--budget-tokens N`. La salida de `agent` es JSON compacto UTF-8; diagnósticos van a stderr. Se conservan `init`, `scan`, `inventory`, `suggest`, `status`, `pending`, `map`, `mark`, `note`, `history`, `build`, `test`, `coverage`, `report` y `check`.

`agent watch --project <ruta> --seconds 60 --reconcile-ms 900000` mantiene el seguimiento incremental durante un intervalo explícito. La GUI y cada sesión MCP mantienen su propio watcher mientras están abiertas. No instala servicios ni tareas del sistema.

Configure `inventory_mode` como `universal` o `source_only` en el JSON del proyecto. En `source_only`, el recorrido identifica fuentes, scripts, recursos de interfaz y scripts de compilación. `code_extensions` y `code_filenames` amplían la detección, e `include_all_code` controla qué código generado y externo cuenta para el avance en cada raíz. `inventory_sync` informa `inventory_mode` y `filtered_non_code`. La GUI y MCP también reconcilian un catálogo existente al abrir el proyecto. Para obtener altas, consulte `query` con `kind:new`, `role:source` y `since_revision` de la última lectura.

`code_duplicate_mode` admite `off`, `files` y `files_and_zip`. Tras sincronizar, `query` con `kind:code_duplicates` devuelve una fila por copia con tamaño, huella, cantidad del grupo y ruta. Una copia ZIP incluye `entry_path` y `entry_index`. `kind:code_duplicate_errors` devuelve los archivos o entradas que no se pudieron leer. El campo `report` distingue si el resultado está disponible, vigente y completo. Las entradas ZIP se registran como evidencia de duplicados separada del inventario de migración.

`capabilities` describe los comandos. Códigos de salida: 0 operación/consulta realizada; 1 error; 3 prueba/compilación no aprobada; 4 comprobación incompleta o sin casos. Una consulta con salida 0 puede informar pendientes.

## MCP

Lanzar `migratool-cli-0.3.exe mcp --project <ruta>`. Implementa MCP con handshake `initialize` para las versiones `2024-11-05`, `2025-03-26`, `2025-06-18` y `2025-11-25`. Las solicitudes de otra versión reciben la versión negociable `2025-11-25`. No anuncia la semántica sin handshake de `2026-07-28`; un cliente moderno con fallback debe negociar la versión legacy anunciada.

Un mensaje JSON-RPC por línea. Métodos: `initialize`, `notifications/initialized`, `ping`, `tools/list`, `tools/call` y `notifications/cancelled`. No hay texto de diagnóstico en stdout. Solo se admite una operación en curso; el cliente puede cancelarla por `requestId`. El cierre de stdin cancela la operación propia, cierra el watcher y termina el servidor.

Para versiones desde junio de 2025, el resultado principal va en `structuredContent` y el texto contiene un acuse breve para no duplicar los datos. Para versiones anteriores va como JSON compacto en un bloque de texto. `isError:true` distingue errores de ejecución de herramientas; errores JSON-RPC distinguen protocolo o herramienta inexistente.

Ejemplo de configuración para clientes que usan `mcpServers`: [examples/integration/mcp-client.json](../examples/integration/mcp-client.json). No se modifica la configuración real de ningún cliente del equipo.

## Herramientas

- `project_context`: nombre, revisión, última sincronización, clasificación, porcentajes, pendientes y próximos IDs.
- `inventory_sync`: `full:true` recalcula hashes. Devuelve cambios y métricas de hash reutilizado, errores y duplicados.
- `query`: `kind` puede ser `files`, `symbols`, `documentation`, `dependencies`, `relations`, `notes`, `mappings`, `search`, `new`, `ignored`, `orphans` o `excluded`. Filtros: `root`, `role`, `language`, `category`, `relevant`, `git_state`, `deleted`, `text`, `id`, `ids` o `file`, según tipo. `queries:[{...},{...}]` reúne consultas de lectura en una invocación.
- `migration_status`: `level:project|files|symbols`. Los porcentajes se calculan sobre el índice conocido y señalan el último sync; no afirman paridad universal.
- `next_tasks`: archivos con migración/evidencia pendiente, priorizados por cantidad de dependencias pendientes resueltas. Las dependencias ambiguas conservan su estado desconocido. Incluye ruta, documentos, candidatos de destino, notas y faltantes.
- `changes_since`: `since_revision` excluye los registros anteriores. Para consumir un delta paginado, termine las páginas y conserve `through_revision` para la consulta siguiente.
- `batch_update`: actor, revisión esperada y operaciones atómicas descritas abajo.
- `verify`: `action:build|test|scan|coverage` y `execute:true`. Para cobertura: `input`, `root`, `format:llvm|cobertura|generic`. Ejecuta únicamente los comandos del proyecto.
- `evidence_report`: resultados, estados, causas y rutas de los comprobantes.
- `history`: filtra `entity`, `actor`, `action`, `since_revision`; `detail:full` incluye antes/después y evidencia.
- `connections`: conexiones MCP y observación de sus procesos vivos.
- `statistics`: invocaciones, bytes y tiempo medido por herramienta.

Las herramientas devuelven JSON compacto, `revision`, y cuando son listados, `items`, `total`, `cursor` y `next_cursor`.

## Presupuesto, páginas y caché

`fields:["id","path"]` selecciona campos de cada elemento. `limit` admite 1–1000. `cursor` tiene forma `revision:offset`: si cambió la revisión, se rechaza como `CURSOR_STALE`. No mezcla páginas de dos estados distintos. Cambios desde una revisión se consultan con `since_revision`.

`token_budget` admite 256–100000, por defecto 800. No cuenta tokens de un modelo concreto. Aplica una cota conservadora de bytes UTF-8 del objeto JSON compacto, informada como `budget.policy:utf8_bytes_upper_bound`. `response_bytes` informa el tamaño final. Los sobres del transporte MCP y un salto de línea de CLI no forman parte de esa cota. Si un elemento no cabe, devuelve una indicación para aumentar el presupuesto o pedir menos campos, conservando el cursor. No elimina campos dentro de un registro sin avisar.

`project_context` reduce primero el lote de próximos IDs. `query` reduce páginas. En resultados de verificación o sincronización muy grandes, el acuse puede indicar `truncated`; los registros permanecen consultables mediante `changes_since` o `evidence_report`. Un lote comprometido devuelve `ok`, revisión y número de operaciones, sin repetir todos los datos.

La caché de progreso usa revisión del índice, configuración y metadatos de archivos de evidencia. Se invalida por cambios indexados, lotes, nuevas verificaciones y eventos del watcher. El sync completo recalcula los hashes. Cuando ningún watcher está vivo, el cliente debe sincronizar para comprobar cambios externos; una respuesta de caché no es observación continua del disco.

Mientras otra conexión escribe, las consultas pueden leer el catálogo en modo WAL. La caché y las estadísticas no bloquean esas lecturas; `statistics_recorded:false` indica que no se persistió la medición de esa respuesta.

## Actualizaciones atómicas

```json
{
  "expected_revision": 123,
  "actor": "Codex",
  "operations": [
    {"op":"mark","source":"F12","targets":["F37","F40"],"complete":true,"cases":["regression_1"],"note":"Revisado con los casos indicados."},
    {"op":"map","source":"S19","targets":["S51"],"complete":false,"note":"Falta cancelación."},
    {"op":"note","entity":"F12","text":"Comprobar el flujo de error."},
    {"op":"classify","file":"F15","category":"source","relevant":true}
  ]
}
```

IDs ilustrativos: consulte los reales con `query`. Obtenga `expected_revision` justo antes del lote. `REVISION_CONFLICT` rechaza el lote entero; relea los cambios y decida de nuevo. No se reintenta una declaración obsoleta automáticamente. Los targets deben pertenecer a raíces `target` y el origen a `source`. Las marcas capturan los hashes actuales del índice y los casos deben existir en la configuración.

Los eventos guardan hora UTC, actor, operación, entidad y estados anterior/nuevo. La ruta absoluta se consulta por ID y se conserva en los eventos pertinentes. Una declaración completa puede estar sin pruebas; ese estado se presenta como `DECLARED_COMPLETE_UNVERIFIED`.

## Adaptadores de análisis

Un adaptador se configura en `analyzers`:

```json
{"id":"mi-parser","extensions":[".lenguaje"],"argv":["E:/Herramientas/parser.exe","{file}","{output}"],"timeout_ms":30000}
```

Marcadores: `{file}`, `{root}`, `{project}`, `{output}`. El proceso escribe en `{output}` o stdout:

```json
{"protocol":"migratool.analyzer/1","symbols":[{"name":"operacion","qualified_name":"Modulo.operacion","kind":"function","signature":"(int)","line":1}],"dependencies":[{"kind":"call","name":"otra","line":2}],"diagnostics":[],"errors":0}
```

No inventar símbolos si el parser no dispone de ellos. `errors>0` conserva análisis parcial y diagnósticos. Los analizadores son ejecutables elegidos por el usuario; no se descarga ni ejecuta ninguno remoto automáticamente.

## Fuentes de especificaciones

- MCP stdio legacy: <https://modelcontextprotocol.io/specification/2025-11-25/basic/transports>.
- Diferencias con stdio 2026: <https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio>.
- SQLite FTS5: <https://www.sqlite.org/fts5.html>.
- ReadDirectoryChangesW: <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw>.
- Roslyn: <https://learn.microsoft.com/en-us/dotnet/csharp/roslyn-sdk/work-with-syntax>.
- Clang compilation database: <https://clang.llvm.org/docs/JSONCompilationDatabase.html>.

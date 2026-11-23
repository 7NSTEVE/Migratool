# PRODUCT_REQUIREMENTS_MASTER — Migratool v3.1.0
## AI Agent Project OS & Multi-Agent Worktree / Build / Storage Coordinator

- **Documento:** Especificación Maestra de Requisitos del Producto (`PRODUCT_REQUIREMENTS_MASTER.md`)
- **Compañero estructurado:** `docs/PRODUCT_REQUIREMENTS_MASTER.json`
- **Versión del sistema:** `3.1.0`
- **Fecha de vigencia:** `2026-11-22`
- **Plataforma objetivo:** Windows 10/11 x64 (`C++23` nativo, Win32 GUI Apple HIG, CLI, Servidor MCP JSON-RPC 2.0 sobre `stdio`, SQLite WAL + FTS5)

---

## 1. Propósito y Principios No Negociables del Sistema

Migratool no se limita a indexar código ni a recordar qué hizo cada agente de IA: su propósito es **administrar, coordinar, integrar, verificar y conservar sin pérdida el trabajo concurrente o secuencial de múltiples agentes de IA** (`Codex`, `Antigravity`, `Claude Code`, agentes locales y desarrolladores humanos) sobre proyectos complejos y archivos masivos en Windows.

> **Meta Operativa de Incorporación Multi-Agente:**
> Un agente debe poder incorporarse a cualquier proyecto administrado por Migratool y conocer, con la mínima cantidad de llamadas posible, qué existe, dónde está, qué se hizo, qué falta, quién está trabajando, qué cambios no están integrados, cuál compilación contiene qué funcionalidades y qué puede limpiarse sin peligro.

### 1.1 Regla Suprema de Integridad sobre Eficiencia
**Migratool jamás debe ahorrar llamadas a herramientas, tokens o tiempo de ejecución a costa de perder trabajo, ocultar conflictos, truncar verificaciones o presentar como completada una integración o compilación que no fue verificada con evidencia directa.**

Cuando se optimiza la eficiencia del protocolo MCP, Migratool mide y reporta por separado cuatro dimensiones de costo que no deben confundirse entre sí:
1. **Cantidad de llamadas a herramientas MCP** (`tool_calls_count`).
2. **Tokens de entrada y salida** (`input_tokens`, `output_tokens` bajo `o200k_base`).
3. **Tiempo real de ejecución** (`elapsed_ms`).
4. **Consumo real de cuota del proveedor** (cuando el entorno o proveedor lo exponga).

Ninguna superherramienta (`project_resume`, `integrate_ready`, `build_canonical`, `storage_audit`, `discover_replicas`, `project_health`) puede ocultar errores internos ni ejecutar operaciones destructivas implícitas: siempre debe devolver identificadores deterministas, evidencias verificables, advertencias explícitas y acciones pendientes.

### 1.2 Invariante Cero-Pérdida de Progreso Multi-Agente
Ningún progreso válido de un agente o worktree puede perderse silenciosamente por:
- Cierre inesperado de sesión, caída de proceso o corte de energía.
- Agotamiento de cuota del modelo (`quota exhaustion`), límite de ventana de contexto o cambio de agente/modelo a mitad de tarea.
- Fallo de red, timeout de transporte MCP o bloqueo de subprocesos (`git`, servidores `LSP`, compiladores).
- Operaciones de organización de archivos (`fs_organize`), limpieza de compilaciones o mantenimiento automático (`housekeep`).

Cuando un agente se detiene o agota su cuota mientras trabaja en una funcionalidad, Migratool conserva e identifica automáticamente el estado exacto de su worktree, sus modificaciones sin commit, su último checkpoint/snapshot anclado (`P4`), los archivos bloqueados o tocados y los pasos pendientes. El agente de relevo recibe ese contexto en una sola llamada (`project_resume` / `agent_wake`) sin re-escanear a ciegas el repositorio y sin marcar como terminado un trabajo meramente porque el agente saliente afirmó haber concluido.

### 1.3 Separación Estricta de los 4 Estados del Ciclo de Vida del Trabajo
Compilar correctamente en una rama o worktree aislado **no equivale** a haber integrado ni entregado el trabajo del proyecto. Si el Agente A implementa una interfaz en su worktree y el Agente B corrige errores y compila desde otro worktree sin integrar la rama del Agente A, el ejecutable resultante carece del trabajo del Agente A. Para impedir esta pérdida funcional, Migratool impone cuatro estados mutuamente diferenciados que **jamás pueden confundirse ni saltarse**:

| Orden | Estado Canónico | Nombre en Español | Definición Exacta e Invariante de Evidencia |
| :---: | :--- | :--- | :--- |
| **1** | `IMPLEMENTED` | Implementado | El código o cambio fue escrito en un worktree o rama de trabajo por un agente o usuario, pero aún no cuenta con verificación completa de compilación y pruebas ni respaldo confirmado. |
| **2** | `CONFIRMED` | Confirmado | Los cambios están guardados en un commit de Git o snapshot verificable por SHA-256, compilan limpiamente y pasan las pruebas requeridas con evidencia directa registrada (`exit_code == 0` y conteo real de pruebas ejecutadas `> 0`). |
| **3** | `INTEGRATED` | Integrado | Los cambios en estado `CONFIRMED` fueron fusionados sin conflictos en la rama principal o de integración del proyecto y compilados exitosamente dentro del build integrado junto con el resto de avances aceptados. |
| **4** | `DELIVERED` | Entregado | El build en estado `INTEGRATED` fue empaquetado o publicado en el ejecutable final o release canónico del proyecto, acompañado de un manifiesto de compilación inmutable (`.migratool/builds/<build_id>/manifest.json`) con hashes SHA-256 de todos los artefactos y trazabilidad completa de requisitos y commits incluidos. |

#### Reglas de Transición de Estado
1. `IMPLEMENTED -> CONFIRMED`: Requiere snapshot o commit verificable + compilación exitosa + ejecución real de las suites de prueba asociadas sin fabricar conteos de éxito cuando no se corrieron pruebas.
2. `CONFIRMED -> INTEGRATED`: Requiere evaluación previa libre de conflictos (`git merge-tree --write-tree`), fusión efectiva en la rama de integración/principal y recompilación + validación del árbol integrado.
3. `INTEGRATED -> DELIVERED`: Requiere generación de manifiesto canónico con `build_id`, `commit_sha`, estado del árbol (`dirty_state`), lista de worktrees/features integrados, hashes SHA-256 de los binarios finales y superación de todas las compuertas obligatorias (`acceptance_gates`).
4. **Degradación automática ante cambios externos:** Si un archivo o worktree con estado `CONFIRMED` o `INTEGRATED` sufre modificaciones en disco no registradas por un intent transaccional (`EXTERNAL_MODIFICATION_WITHOUT_INTENT`), su estado retrocede de forma fail-closed hasta que sea reconciliado y re-verificado.

### 1.4 Regla Dura de Compilación Final Canónica
> **Bloqueo de Build Final con Trabajo Obligatorio Pendiente:**
> Ningún build puede declararse `INTEGRATED` ni `DELIVERED` (compilación final canónica) si existen worktrees registrados con trabajo obligatorio en estado `CONFIRMED` o `IMPLEMENTED` pendiente de integrar en la rama principal/integración, **salvo que exista una exclusión explícita registrada con motivo, autor (`excluded_by`), marca de tiempo y justificación auditable (`exclusion_reason`)**.

Esta regla no impide que un agente ejecute compilaciones locales o experimentales (`build_type: "experimental" | "debug" | "test"`) en su propio worktree para iterar, pero prohíbe etiquetar un binario experimental o parcial como el build final integrado del proyecto.

### 1.5 Confinamiento Físico Fail-Closed
Toda operación de lectura, escritura, edición por lotes, restauración de snapshots, organización de archivos, exportación de evidencia, respaldo de catálogo o ejecución de herramientas sobre rutas del proyecto debe validar el confinamiento físico real mediante `Project::resolve_confined` usando resolución por descriptor del sistema operativo (`GetFinalPathNameByHandleW`) y detección explícita de puntos de reparse NTFS (`FILE_ATTRIBUTE_REPARSE_POINT`, symlinks y directory junctions). Cualquier ruta que utilice secuencias de escape (`..`), rutas UNC externas no autorizadas o junctions/symlinks cuyo destino resuelto escape de las raíces autorizadas del proyecto debe ser rechazada inmediatamente (`PATH_OUTSIDE_PROJECT_ROOT` / `REPARSE_POINT_ESCAPE_BLOCKED`).

### 1.6 Prohibición de Limpieza Ciega por Nombre y Políticas de Almacenamiento
Queda estrictamente prohibido eliminar, mover a cuarentena o descartar carpetas basándose únicamente en su nombre (`old`, `backup`, `temp`, `build`, `tests`, `include`, `tools`) o únicamente en su antigüedad:
- Una carpeta llamada `old`, `backup` o `temp` puede contener código fuente irrepetible o avances huérfanos aún no confirmados en Git.
- Los archivos dentro de `tests/`, `include/`, `tools/` o `scripts/` son código fuente real del proyecto aunque no residan dentro de `src/`.
- `git worktree prune` solo limpia metadatos administrativos obsoletos de `.git/worktrees`; no equivale a auditar ni retirar de forma segura un worktree real ni sus artefactos (lo cual exige verificar ausencia de cambios sin commit, ausencia de procesos activos y uso controlado de `git worktree remove` o reciclaje seguro).

#### Clasificación Canónica de Directorios y Rutas (9 Categorías)
Cada ruta evaluada por Migratool debe recibir una clasificación verificable configurable por proyecto (por ejemplo, proyectos donde todo código propio vive bajo `src/` frente a proyectos estándar con múltiples raíces de código):
1. `SOURCE`: Código fuente de producción de la aplicación (`src/`, módulos principales).
2. `TEST_SOURCE`: Código fuente de pruebas unitarias, de integración y regresión (`tests/`, `spec/`).
3. `BUILD_OUTPUT`: Artefactos binarios e intermedios producidos por compiladores y enlazadores (`build/`, `out/`, `bin/`, `obj/`).
4. `GENERATED`: Código o activos generados automáticamente de forma reproducible a partir de fuentes declarativas (`zip-headers/`, código generado por CMake/IDL).
5. `DEPENDENCY`: Dependencias externas descargables o reproducibles (`vcpkg_installed/`, `node_modules/`, paquetes NuGet).
6. `CACHE`: Cachés temporales de herramientas, análisis o compilación (`.cache/`, cachés de LSP/CMake).
7. `DOCUMENTATION`: Especificaciones, manuales, diagramas y contratos del proyecto (`docs/`, `README.md`, `CHANGELOG.md`).
8. `USER_DATA`: Bases de datos locales, catálogos `.migratool/`, memorias, snapshots y datos de usuario.
9. `UNKNOWN`: Rutas aún no clasificadas que exigen inspección antes de cualquier acción de limpieza.

#### Niveles de Conservación de Almacenamiento (4 Tiers)
Todo directorio de compilación, worktree o árbol candidato a mantenimiento se clasifica en uno de cuatro niveles de conservación:

| Nivel Canónico | Nombre en Español | Criterio de Asignación y Política de Acción |
| :--- | :--- | :--- |
| `PROTECTED` | Protegido | Código con cambios sin confirmar (`dirty`), worktrees activos, snapshots/respaldos de recuperación (`P4`), código fuente (`SOURCE`, `TEST_SOURCE`, `DOCUMENTATION`) y compilaciones finales canónicas (`DELIVERED` / `INTEGRATED`) marcadas para conservación. **Prohibida cualquier eliminación.** |
| `TEMP_RETAINED` | Conservado temporalmente | Compilaciones recientes dentro de la ventana de retención, bitácoras de pruebas fallidas y artefactos necesarios para diagnosticar errores activos. **Se conservan hasta cumplir política de retención y ausencia de incidentes.** |
| `VERIFIED_SAFE_CLEAN` | Limpieza segura verificada | Artefactos `BUILD_OUTPUT`, `GENERATED` o `CACHE` 100% reproducibles, sin archivos fuente ni modificaciones sin commit, con proceso propietario inactivo, sin referencias de un build `PROTECTED` y con política de retención cumplida. **Elegibles para envío seguro a la Papelera de Reciclaje de Windows.** |
| `NEEDS_REVIEW` | Requiere revisión | Carpetas huérfanas, clones o proyectos duplicados, rutas clasificadas como `UNKNOWN` o árboles que contienen posibles avances no integrados. **Bloqueados para limpieza automática; requieren decisión explícita.** |

### 1.7 Distinción entre Archivos de Requisitos y Archivos de Sesión
Para evitar pérdida de especificaciones a largo plazo cuando múltiples agentes colaboran en distintas sesiones:
- `docs/PRODUCT_REQUIREMENTS_MASTER.md`: Fuente maestra legible por humanos y agentes que establece **qué debe cumplir permanente y obligatoriamente el producto**.
- `docs/PRODUCT_REQUIREMENTS_MASTER.json`: Representación estructurada indexable por Migratool para consultas quirúrgicas de trazabilidad (`Requisito -> Implementación -> Prueba -> Commit -> Compilación`) sin obligar a los agentes a leer el documento completo en cada turno.
- Archivos de pendientes de sesión (ej. `PENDIENTES_SESION_*.md` o workstreams activos): Registran **qué trabajo falta realizar en una sesión específica**. Concluir una tarea de sesión jamás elimina ni desactiva el requisito maestro permanente en `PRODUCT_REQUIREMENTS_MASTER.*`.

---

## 2. Catálogo Maestro de Requisitos Funcionales y No Funcionales (`REQ-*`)

Cada requisito del catálogo maestro define su identificador estable (`REQ-*`), categoría, prioridad (`P0`, `P1`, `P2`), estado actual en la línea base auditada, descripción técnica detallada, reglas de invariante inquebrantables, archivos de implementación en el árbol de código, suites de prueba de CTest asociadas y criterios de aceptación medibles.

---

### 2.1 Seguridad y Confinamiento (`REQ-SEC-001` .. `REQ-SEC-005`)

#### `REQ-SEC-001` — Confinamiento físico canónico de rutas y bloqueo de escape por reparse points
- **ID:** `REQ-SEC-001`
- **Título:** Confinamiento físico canónico (`Project::resolve_confined`, detección de reparse points, bloqueo de traversal `..` y rutas UNC externas)
- **Categoría:** `SECURITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Toda operación que reciba rutas relativas o absolutas desde clientes MCP, CLI o archivos de manifiesto debe resolver y validar que el destino físico final reside estrictamente dentro de las raíces autorizadas del proyecto o del `state_dir` asignado. La función central `Project::resolve_confined` normaliza la ruta con soporte extendido `\\?\`, abre o inspecciona la cadena de directorios existentes con `CreateFileW(..., FILE_FLAG_BACKUP_SEMANTICS)` y `GetFinalPathNameByHandleW`, e inspecciona cada componente para detectar `FILE_ATTRIBUTE_REPARSE_POINT` (`IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`). Este confinamiento se aplica obligatoriamente en `workstream`, `read_bundle`, `fs_organize`, `scale_export`, `backup_catalog`, `git_intel` y `lsp_client`.
- **Reglas de invariante:**
  1. Ninguna ruta con segmentos `..` que resuelva fuera de la raíz del proyecto o `state_dir` puede ser leída, creada, modificada, respaldada ni eliminada.
  2. Ninguna ruta UNC externa (`\\servidor\recurso`) ni unidad distinta no registrada en el proyecto puede atravesarse mediante llamadas MCP de proyecto.
  3. Si cualquier ancestro o destino final dentro del árbol del proyecto es un symlink o junction NTFS cuyo destino físico (`GetFinalPathNameByHandleW`) cae fuera de las raíces autorizadas, la operación aborta en modo fail-closed antes de tocar el archivo.
- **Archivos de implementación:**
  - `include/migratool/core.hpp`
  - `src/platform.cpp`
  - `src/workstream.cpp`
  - `src/agent_supertools.cpp`
  - `src/fs_organize.cpp`
  - `src/scale_export.cpp`
  - `src/database.cpp`
  - `src/git_intel.cpp`
  - `src/lsp_client.cpp`
- **Suites de prueba asociadas:**
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `durable_inventory_export` (`tests/inventory_export_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Intentar leer o editar `..\..\Windows\System32\drivers\etc\hosts` o una ruta UNC externa en `workstream`, `read_bundle`, `batch_edit`, `scale_export` o `backup_catalog` devuelve error explícito de confinamiento y `0` bytes filtrados o escritos.
  2. Crear un directory junction dentro del workspace que apunte a un directorio externo y ejecutar `read_bundle`, `workstream`, `fs_organize`, `git_intel` o `lsp_client` sobre él resulta bloqueado en el 100% de los casos con código de error estructurado.

---

#### `REQ-SEC-002` — Desactivación en producción de puntos de inyección de fallos (`failpoints`)
- **ID:** `REQ-SEC-002`
- **Título:** Desactivación en producción de puntos de inyección de fallos (`failpoints_enabled() == false` por defecto)
- **Categoría:** `SECURITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Los mecanismos de inyección de fallos utilizados para probar recuperación ante caídas a mitad de protocolo WAL (`MIGRATOOL_FAILPOINT`, fallos simulados entre etapas `PREPARED..COMMITTED` o en `fs_organize`) deben estar deshabilitados por defecto en los binarios de producción (`migratool-cli-0.3.exe`, `migratool-cli.exe`, `Migratool-0.3.exe`). La función `failpoints_enabled()` debe retornar `false` de manera determinista en ejecución normal y solo puede habilitarse explícitamente desde binarios de prueba compilados o inicializados bajo arnés de test autorizado.
- **Reglas de invariante:**
  1. En cualquier binario de producción (`CLI`, `MCP`, `GUI`), `failpoints_enabled()` es `false` por defecto e ignora variables o parámetros no autorizados que intenten provocar abortos artificiales en operaciones de usuario.
  2. Las suites de prueba de resiliencia (`agent_os_resilience`, `fs_organize_and_usn_regression`) pueden activar failpoints en proceso mediante la API interna de test para verificar la recuperación transaccional de las 7 etapas WAL.
- **Archivos de implementación:**
  - `include/migratool/core.hpp`
  - `src/platform.cpp`
  - `src/workstream.cpp`
  - `src/fs_organize.cpp`
- **Suites de prueba asociadas:**
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Verificar en tiempo de ejecución del binario de producción que `failpoints_enabled() == false` incluso si se invoca desde un entorno con variables residuales no autorizadas.
  2. Verificar que las pruebas de caída simulada en `agent_os_resilience` siguen ejecutando y recuperando las etapas `PREPARED`, `BACKED_UP`, `APPLYING` y `APPLIED` cuando el arnés de pruebas habilita explícitamente los failpoints.

---

#### `REQ-SEC-003` — Aislamiento de subprocesos Git y LSP sin bloqueo del servidor MCP
- **ID:** `REQ-SEC-003`
- **Título:** Aislamiento de subprocesos Git y LSP (`hStdInput -> NUL`, `JobObject` con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, lectura asíncrona con timeout real)
- **Categoría:** `SECURITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Cuando el servidor MCP ejecuta subprocesos hijos (`git.exe`, servidores de lenguaje LSP como `clangd`, `pyright`, `rust-analyzer`, o ejecutores de pruebas), ningún subproceso puede heredar el descriptor `stdin` del servidor MCP JSON-RPC ni quedar huérfano si el proceso padre termina o si la operación excede su presupuesto de tiempo. Todo subproceso no interactivo (`git_intel`, comandos de verificación) debe redirigir su `hStdInput` a un handle `NUL` de Windows (`\\.\NUL`), ejecutarse dentro de un Win32 `JobObject` configurado con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` y drenar `stdout`/`stderr` de forma asíncrona con un timeout real que mate el árbol de procesos si se bloquea.
- **Reglas de invariante:**
  1. Ningún subproceso hijo lanzado por `git_intel` o utilidades de plataforma puede leer bytes del pipe `stdin` del servidor MCP (`hStdInput` siempre conectado a `NUL` o a un pipe dedicado cerrado inmediatamente).
  2. Todo subproceso hijo (`git`, `LSP`, `run_verified`) está asociado a un `JobObject` con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, garantizando `0` procesos huérfanos al cerrar el handle o terminar Migratool.
  3. Si un subproceso excede su `timeout_ms` o deja de responder sin cerrar sus pipes, la lectura asíncrona aborta limpiamente tras el timeout, termina el `JobObject` y devuelve un estado de timeout estructurado sin bloquear el bucle principal de MCP.
- **Archivos de implementación:**
  - `src/platform.cpp`
  - `src/git_intel.cpp`
  - `src/lsp_client.cpp`
  - `src/agent_supertools.cpp`
- **Suites de prueba asociadas:**
  - `serena_workspace_lsp` (`tests/serena_lsp_tests.cpp`)
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `gateway` (`tests/gateway_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Un subproceso hijo que intente leer de `stdin` recibe `EOF` inmediato desde `NUL` y no consume mensajes JSON-RPC destinados al servidor MCP.
  2. Un subproceso colgado deliberadamente más allá de `timeout_ms` es terminado por el `JobObject` dentro del margen de tolerancia (`< timeout_ms + 500 ms`), liberando todos los descriptores y dejando `0` procesos hijos vivos.

---

#### `REQ-SEC-004` — Transporte JSON-RPC MCP acotado, validación estructural y tolerancia UTF-8
- **ID:** `REQ-SEC-004`
- **Título:** Transporte JSON-RPC MCP acotado (límite de línea de 16 MiB, validación estructural antes de acceso tipado, serialización UTF-8 tolerante, workers protegidos contra `std::terminate`)
- **Categoría:** `SECURITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El servidor MCP (`src/mcp.cpp`) debe protegerse contra agotamiento de memoria (DoS por línea infinita sin `\n`), contra excepciones de tipos en `nlohmann::json` (`type_error` cuando un campo esperado como objeto o arreglo llega con otro tipo), contra bytes no válidos en UTF-8 provenientes de nombres de archivos o salidas de herramientas nativas de Windows, y contra excepciones no capturadas en hilos del pool de lectura concurrente.
- **Reglas de invariante:**
  1. El lector de `stdin` del servidor MCP aplica un límite estricto de `16 MiB` (`16 * 1024 * 1024` bytes) por línea JSON-RPC; si una línea supera ese tamaño sin delimitador de fin de línea, descarta el exceso de manera segura y responde con error JSON-RPC `-32600` / `-32700` sin agotar la RAM del proceso.
  2. Antes de realizar cualquier acceso tipado sobre `params`, `arguments` o campos anidados, el despachador valida estructuralmente `is_object()`, `is_array()`, `is_string()`, `is_number()` o `is_boolean()`.
  3. Toda serialización de respuestas JSON-RPC utiliza `dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)` para que secuencias de bytes no UTF-8 nunca derriben la sesión MCP.
  4. Todos los workers del pool concurrente capturan `const std::exception&` y `...`, evitando que cualquier fallo interno dispare `std::terminate`.
- **Archivos de implementación:**
  - `src/mcp.cpp`
  - `src/gateway.cpp`
- **Suites de prueba asociadas:**
  - `gateway` (`tests/gateway_tests.cpp`)
  - `serena_workspace_lsp` (`tests/serena_lsp_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Enviar payloads JSON malformados, tipos cruzados (`"params": 123`, `"arguments": []`), cadenas con bytes inválidos `0xFF 0xFE` o mensajes que excedan 16 MiB devuelve respuestas de error JSON-RPC válidas sin caída del proceso (`exit_code` activo) y permite procesar la siguiente solicitud MCP normalmente.

---

#### `REQ-SEC-005` — Carga segura de DLLs opcionales y empaquetado de código fuente restringido a `git ls-files`
- **ID:** `REQ-SEC-005`
- **Título:** Carga segura de DLLs opcionales (`libzip.dll`, `libclang.dll`) priorizando `executable_dir()` y empaquetado de código fuente restringido estrictamente a `git ls-files`
- **Categoría:** `SECURITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  La carga dinámica de bibliotecas (`libzip.dll`, `libclang.dll`) debe evitar el secuestro de DLLs (`DLL hijacking`) desde el directorio de trabajo actual (`CWD`). Migratool debe priorizar la carga desde `executable_dir()` (el directorio del binario de Migratool) o la ruta absoluta verificada de compilación usando `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`. Asimismo, cualquier rutina de exportación o empaquetado del código fuente del repositorio debe construir su lista de archivos exclusivamente a partir de `git ls-files` (confinado a la raíz del proyecto), evitando incluir secretos no rastreados, bases de datos locales `.migratool/`, credenciales privadas o artefactos binarios ignorados.
- **Reglas de invariante:**
  1. `LoadLibraryExW` nunca busca primero en el directorio de trabajo actual (`CWD`) de un proyecto no confiable; prioriza `executable_dir()` y rutas absolutas confiables con flags de búsqueda segura.
  2. El empaquetado de código fuente ignora por completo archivos no rastreados por Git y verifica con `git ls-files` / `git check-ignore` que ningún archivo privado o fuera de control de versiones sea empaquetado.
- **Archivos de implementación:**
  - `src/platform.cpp`
  - `src/analyzers.cpp`
  - `src/duplicates.cpp`
  - `src/fs_organize.cpp`
  - `src/scale_export.cpp`
- **Suites de prueba asociadas:**
  - `durable_inventory_export` (`tests/inventory_export_tests.cpp`)
  - `indexed_semantic_context` (`tests/analysis_context_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. La resolución de `libzip.dll` y `libclang.dll` carga primero desde `executable_dir()` o la ruta absoluta configurada sin cargar DLLs colocadas en un `CWD` arbitrario.
  2. Un archivo no rastreado (`.env`, `.migratool/catalog.sqlite`, archivo privado local) presente en el árbol de trabajo nunca es incluido en un paquete de exportación de código fuente.

---

### 2.2 Integridad de Datos, Workstreams y NTFS (`REQ-DATA-001` .. `REQ-DATA-006`)

#### `REQ-DATA-001` — Preservación sin pérdida en `fs_organize` (`resume`, `plan_id` inmutable y verificación SHA-256 en `undo`)
- **ID:** `REQ-DATA-001`
- **Título:** Preservación sin pérdida en `fs_organize` (`resume=true` nunca elimina `src` cuando `src` y `dst` coexisten -> `SAFETY_CONFLICT`; `plan_id` inmutable; verificación SHA-256 antes de `organize_undo`)
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El subsistema de organización y deduplicación transaccional (`src/fs_organize.cpp`) opera bajo un diario WAL de dos fases (`intent` / `committed`). Si una ejecución previa se interrumpe y se invoca `organize_apply(resume=true)`, y para una entrada del diario tanto el archivo origen (`src`) como el destino (`dst`) existen simultáneamente en disco, Migratool **nunca** debe borrar `src` asumiendo que el movimiento ya terminó: debe marcar la entrada como `SAFETY_CONFLICT` y preservar ambos archivos para evitar pérdida de datos si `src` o `dst` fueron modificados. Además, el `plan_id` generado en `organize_plan` es inmutable durante todo su ciclo de vida, y `organize_undo` debe verificar el hash SHA-256 del archivo en cuarentena/destino contra el manifiesto antes de restaurarlo sobre la ruta original.
- **Reglas de invariante:**
  1. Bajo `resume=true`, la coexistencia física de `src` y `dst` produce `SAFETY_CONFLICT` con `0` eliminaciones automáticas.
  2. El `plan_id` de un plan creado no puede ser sobrescrito ni mutado al aplicar, reanudar o deshacer el plan.
  3. `organize_undo` verifica el SHA-256 real del archivo en `dst` frente al `sha256` registrado en el manifiesto del plan; si el hash difiere o el destino original ya fue ocupado por un archivo distinto, aborta esa entrada con conflicto auditable sin sobrescribir ni destruir datos.
- **Archivos de implementación:**
  - `src/fs_organize.cpp`
  - `include/migratool/fs_organize.hpp`
- **Suites de prueba asociadas:**
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Simular un corte con `src` y `dst` presentes en disco y ejecutar `organize_apply(resume=true)` reporta `SAFETY_CONFLICT` y conserva intactos tanto `src` como `dst`.
  2. Modificar 1 byte de un archivo movido en cuarentena e invocar `organize_undo` detecta la discrepancia de SHA-256, bloquea la restauración corrupta y reporta el error exacto.

---

#### `REQ-DATA-002` — Integridad criptográfica de snapshots y blobs en `workstream` y rollback limpio en `batch_edit`
- **ID:** `REQ-DATA-002`
- **Título:** Integridad de snapshots y blobs en `workstream` (verificación SHA-256 de blobs existentes, pre-validación del 100% de blobs antes de restaurar, detección `EXTERNAL_MODIFICATION_WITHOUT_INTENT`, rollback de archivos nuevos `existed=false`)
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El almacén de blobs direccionados por contenido (`content-addressed blob store`) de `workstream` debe garantizar integridad de extremo a extremo:
  1. Cuando se escribe un blob cuyo nombre SHA-256 ya existe en disco, Migratool recalcula el SHA-256 del archivo almacenado; si está corrupto, lo reemplaza atómicamente en lugar de reutilizarlo a ciegas.
  2. Antes de restaurar cualquier snapshot (`restore_snapshot`) o ejecutar un rollback, Migratool pre-valida en una primera fase que el **100% de los blobs requeridos existen y coinciden con su SHA-256 esperado** antes de modificar un solo archivo del árbol de trabajo.
  3. Si durante la reconciliación de inicio (`agent_wake` / `reconcile_workstreams`) un archivo asociado a un workstream presenta un hash en disco distinto al registrado sin que exista una operación WAL en curso, se reporta explícitamente `EXTERNAL_MODIFICATION_WITHOUT_INTENT` y nunca se sobrescribe silenciosamente.
  4. En `batch_edit`, cada archivo objetivo registra si existía previamente (`existed: true | false`); si una edición multi-archivo falla a mitad de lote, el rollback restaura la imagen previa de los archivos con `existed=true` y elimina limpiamente (vía Papelera o limpieza de staging transaccional) los archivos recién creados con `existed=false`.
- **Reglas de invariante:**
  1. Ninguna restauración de snapshot comienza a mutar el árbol de trabajo si falta aunque sea un blob o si el SHA-256 de cualquier blob en el almacén no coincide.
  2. Ninguna modificación externa hecha por el usuario u otro proceso fuera de un intent WAL activo es revertida automáticamente sin autorización explícita.
  3. Un fallo en el archivo $k$ de un `batch_edit` de $N$ archivos deja el árbol de trabajo exactamente con el mismo conjunto de archivos y hashes SHA-256 que tenía antes de iniciar el lote.
- **Archivos de implementación:**
  - `src/workstream.cpp`
  - `src/agent_supertools.cpp`
  - `src/file_toolkit.cpp`
- **Suites de prueba asociadas:**
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `serena_workspace_lsp` (`tests/serena_lsp_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Corromper 1 byte de un blob en `.migratool/blobs/` e invocar `restore_snapshot` aborta en la fase de pre-validación con `0` archivos modificados en el workspace.
  2. Un `batch_edit` que crea un archivo nuevo y falla en la segunda edición elimina el archivo nuevo durante el rollback, dejando el directorio limpio.
  3. Editar un archivo en disco fuera de Migratool y llamar a `agent_wake` devuelve el estado `EXTERNAL_MODIFICATION_WITHOUT_INTENT` con los hashes anterior y actual.

---

#### `REQ-DATA-003` — Reconciliación honesta de `workstream`, idempotencia concurrente y conteo real en `run_verified`
- **ID:** `REQ-DATA-003`
- **Título:** Reconciliación honesta de `workstream` y `run_verified` (verificación de `expected_post_sha256` antes de promover a `COMMITTED`, bloqueo de `idempotency_key` en estado `IN_PROGRESS`, implementación real de `split_workstream`, y prohibición de fabricar `passed_cnt = 1`)
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  1. Durante la recuperación o avance del protocolo WAL de 7 etapas (`PREPARED -> BACKED_UP -> APPLYING -> APPLIED -> INDEXED -> COMMITTED`), una operación en estado `APPLIED` o `INDEXED` solo puede promoverse a `COMMITTED` tras verificar que el SHA-256 real en disco de cada archivo coincide con su `expected_post_sha256`.
  2. El `idempotency_ledger` registra toda clave nueva con estado `IN_PROGRESS` antes de ejecutar la mutación; si llega una solicitud duplicada mientras la primera sigue `IN_PROGRESS`, es rechazada con conflicto de ejecución concurrente (`IDEMPOTENCY_KEY_IN_PROGRESS`), y solo devuelve respuesta cacheada cuando la operación original alcanzó `COMPLETED`.
  3. La acción `split_workstream` debe ejecutar una partición real en SQLite: crea el nuevo workstream hijo, transfiere la propiedad de los archivos/operaciones seleccionados, actualiza los bloqueos de archivos (`workstream_files`) y registra el evento de linaje en el historial del workstream padre e hijo.
  4. En `run_verified`, si el comando ejecutado termina con `exit_code == 0` pero el analizador de salida no detecta ningún test ejecutado en los formatos soportados (`CTest`, `gtest`, `pytest`, `cargo test`, `dotnet test`, `jest`), `passed_cnt` debe reportarse honestamente como `0` con `tests_detected: false` (nunca fabricar `passed_cnt = 1`).
- **Reglas de invariante:**
  1. Ninguna operación WAL se marca `COMMITTED` si el hash en disco difiere de `expected_post_sha256`.
  2. Ninguna clave de idempotencia en estado `IN_PROGRESS` permite una segunda ejecución paralela de la misma operación.
  3. `run_verified` jamás inventa conteos de pruebas pasadas (`passed_cnt > 0`) sin evidencia textual o estructurada del runner de pruebas.
- **Archivos de implementación:**
  - `src/workstream.cpp`
  - `src/agent_supertools.cpp`
- **Suites de prueba asociadas:**
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Ejecutar `run_verified` con un comando que devuelve `0` sin ejecutar tests (`cmd /c echo ok`) reporta `exit_code: 0`, `passed_cnt: 0` y `tests_detected: false`.
  2. Invocar `split_workstream` sobre un workstream con 4 archivos transfiriendo 2 al nuevo workstream mueve exactamente esos 2 registros en `workstream_files` y conserva los otros 2 en el workstream original.

---

#### `REQ-DATA-004` — Fidelidad del índice NTFS ultra-escala, MFT/USN y reconciliación incremental segura
- **ID:** `REQ-DATA-004`
- **Título:** Fidelidad del índice NTFS ultra-escala (`crawl_root_streaming` sin interbloqueo y `complete=0` durante reconstrucción; `crawl_root_mft` con tamaño y `mtime` reales; `usn_delta_replay` en raíces de unidad `E:\`; `fs_reconcile_incremental` conservador; `fs_apply_rename_or_move` con `length(?)` Unicode; `fs_ensure_root` con limpieza al cambiar ruta/FRN)
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El motor de indexación de sistemas de archivos ultra-escala (`src/fs_index.cpp`, `src/fs_crawler.cpp`) debe cumplir seis garantías de fidelidad sobre volúmenes NTFS reales:
  1. `crawl_root_streaming`: La cola productor-consumidor entre los hilos de enumeración Win32 y el hilo escritor SQLite nunca puede interbloquearse cuando la cola está llena o cuando un trabajador lanza error; además, mientras el índice se reconstruye, el registro en `fs_roots` mantiene `complete = 0` y solo se marca `complete = 1` al confirmar la transacción final y los índices FTS5.
  2. `crawl_root_mft`: Cuando se utiliza enumeración MFT (`FSCTL_ENUM_USN_DATA`), los metadatos que los registros USN básicos no incluyen (`size` exacto y `mtime_ns` real) se obtienen mediante consulta rápida de atributos Win32/MFT para no almacenar tamaños ni fechas en cero.
  3. `usn_delta_replay`: Soporta correctamente tanto subdirectorios como raíces de unidad completas (`E:\`, `D:\`), resolviendo los FRN padres al identificador de raíz del volumen sin fallar por formato de ruta de volumen (`\\.\E:`).
  4. `fs_reconcile_incremental`: Durante la reconciliación offline o incremental, un nodo existente en `fs_nodes` solo puede purgarse de la base de datos si el sistema operativo confirma explícitamente `ERROR_FILE_NOT_FOUND` o `ERROR_PATH_NOT_FOUND` (nunca purgar ante `ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION` o unidad temporalmente desconectada).
  5. `fs_apply_rename_or_move`: El reemplazo de prefijos de ruta en SQLite para subárboles renombrados utiliza `substr(rel_path, length(?) + 1)` (longitud en caracteres Unicode de SQLite) en lugar de desplazar por longitud de bytes UTF-8, preservando rutas con tildes, eñes y caracteres multibyte (`Conversación`, `Lenguajes de Programación`).
  6. `fs_ensure_root`: Si un `root_id` existente cambia de ruta canónica o de `root_frn`, purga de manera consistente los nodos, directorios e índices FTS5 del árbol anterior antes de re-indexar.
- **Reglas de invariante:**
  1. Un catálogo interrumpido a mitad de `crawl_root_streaming` permanece con `complete = 0` y es detectado como incompleto en la siguiente apertura.
  2. Un error transitorio de permisos o bloqueo (`ERROR_SHARING_VIOLATION`, `ERROR_ACCESS_DENIED`) jamás provoca la eliminación del registro del archivo en `fs_nodes`.
  3. Renombrar un directorio cuyo nombre contiene caracteres UTF-8 multibyte (`á, é, í, ó, ú, ñ, CJK`) conserva el 100% de las rutas relativas de sus hijos sin truncar ni desplazar caracteres.
- **Archivos de implementación:**
  - `src/fs_index.cpp`
  - `src/fs_crawler.cpp`
  - `src/usn.cpp`
  - `src/watcher.cpp`
- **Suites de prueba asociadas:**
  - `fs_index_ultra_scale` (`tests/fs_index_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Renombrar mediante `fs_apply_rename_or_move` un árbol bajo `Carpeta_Camión_Diseño/sub` con hijos anidados preserva intactos todos los nombres relativos en `fs_nodes` y `fs_dirs`.
  2. Simular un archivo bloqueado con acceso exclusivo (`dwShareMode = 0`) durante `fs_reconcile_incremental` conserva su fila en `fs_nodes` sin eliminarla.

---

#### `REQ-DATA-005` — Soporte completo de rutas largas (`>260` caracteres) en Windows
- **ID:** `REQ-DATA-005`
- **Título:** Soporte completo de rutas largas (`>260` caracteres) en Windows mediante `native_path()` (`\\?\`) y `<longPathAware>true</longPathAware>` en todos los ejecutables y suites de prueba
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Los proyectos modernos con árboles profundos o worktrees anidados superan con frecuencia el límite heredado `MAX_PATH` (`260` caracteres) de Win32. Migratool debe convertir automáticamente rutas locales y UNC a su representación extendida Win32 (`\\?\C:\...` o `\\?\UNC\...`) a través de `native_path()` en todas las llamadas a APIs de sistema de archivos (`CreateFileW`, `FindFirstFileExW`, `MoveFileExW`, `GetFileAttributesExW`, `RemoveDirectoryW`), y vincular el manifiesto de aplicación `resources/app.manifest` con `<longPathAware>true</longPathAware>` en todos los ejecutables de producción y binarios de prueba CTest.
- **Reglas de invariante:**
  1. Toda ruta de longitud `>= 248` caracteres o con segmentos especiales se procesa con el prefijo `\\?\` en la capa Win32 (`native_path()`).
  2. Todos los ejecutables compilados en `CMakeLists.txt` incluyen soporte de rutas largas habilitado y verificable en rutas de prueba con más de 300 caracteres.
- **Archivos de implementación:**
  - `src/platform.cpp`
  - `resources/app.manifest`
  - `CMakeLists.txt`
- **Suites de prueba asociadas:**
  - `fs_index_ultra_scale` (`tests/fs_index_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Crear, indexar, leer con `read_bundle`, editar con `batch_edit`, respaldar en snapshot de `workstream` y organizar con `fs_organize` archivos situados a más de `310` caracteres de longitud total de ruta se completa sin errores `ERROR_PATH_NOT_FOUND` ni `ERROR_FILENAME_EXCED_RANGE`.

---

#### `REQ-DATA-006` — Sincronización interproceso real en `SharedBudgetLedger` y exactitud de esquemas SQL
- **ID:** `REQ-DATA-006`
- **Título:** Sincronización interproceso real en `SharedBudgetLedger` y corrección de consultas SQL (`r.abs_path` en `knowledge_search`, `source_path/target_path/reason` en `scale_query(relations)`)
- **Categoría:** `DATA_INTEGRITY`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  1. `SharedBudgetLedger` coordina el presupuesto de llamadas, tokens y concurrencia entre múltiples instancias de Migratool (`CLI`, `MCP`, `GUI`) usando memoria compartida o estado sincronizado con un mutex con nombre de Win32 (`CreateMutexW` con adquisición y liberación RAII sobre `WAIT_OBJECT_0` / `WAIT_ABANDONED`) para garantizar lecturas y actualizaciones atómicas entre procesos.
  2. La consulta SQL de `knowledge_search` sobre vínculos de archivos utiliza la columna real `r.abs_path` de `fs_roots` al reconstruir rutas vinculadas por FRN, evitando errores de preparación SQLite (`no such column`).
  3. La consulta `scale_query(kind="relations")` proyecta las columnas reales del esquema (`source_path`, `target_path`, `reason`, `relation_kind`, `confidence`), garantizando que las relaciones entre orígenes y destinos se devuelvan sin fallos de columna inexistente.
- **Reglas de invariante:**
  1. Toda mutación en `SharedBudgetLedger` está protegida por sincronización interproceso real de kernel de Windows (`Named Mutex`), recuperándose limpiamente incluso si un proceso participante muere mientras sostenía el mutex (`WAIT_ABANDONED`).
  2. Ninguna consulta SQL en `knowledge` ni en `scale_query` referencia nombres de columnas inexistentes en el esquema vigente del catálogo.
- **Archivos de implementación:**
  - `src/platform.cpp`
  - `src/knowledge.cpp`
  - `src/scale.cpp`
  - `src/queries.cpp`
- **Suites de prueba asociadas:**
  - `knowledge_docs_lineage` (`tests/knowledge_docs_lineage_tests.cpp`)
  - `durable_inventory` (`tests/scale_tests.cpp`)
  - `live_sync` (`tests/live_sync_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Invocar `knowledge(action="search")` con entradas que poseen `linked_paths` vinculados a `fs_roots` ejecuta la consulta SQL con éxito (`0` errores SQLite) y resuelve la ruta absoluta.
  2. Invocar `scale_query(kind="relations")` devuelve las filas con `source_path`, `target_path` y `reason` correctamente poblados.

---

### 2.3 Coordinación Multi-Agente, Worktrees, Builds y Almacenamiento (`REQ-WORKTREE-001` .. `REQ-GUI-001`)

#### `REQ-WORKTREE-001` — Registro central persistente de worktrees y protocolo de relevo multi-agente
- **ID:** `REQ-WORKTREE-001`
- **Título:** Registro central persistente de worktrees y progreso por agente (`agent_worktrees_catalog`, supertool `worktree_inventory` / `project_resume`, protocolo de relevo ante corte de cuota o cambio de agente)
- **Categoría:** `MULTI_AGENT_COORDINATION`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Migratool mantiene una tabla persistente `agent_worktrees_catalog` en SQLite que combina el descubrimiento automático de Git (`git worktree list --porcelain` y `git status --porcelain=v2`) con los metadatos de coordinación multi-agente. Para cada worktree se registra y actualiza incrementalmente:
  - `worktree_id`, `project_id`, `worktree_path`, `branch_name`, `head_commit_sha`, `base_commit_sha`.
  - `agent_name` / `owner_agent` (ej. `Codex`, `Antigravity`, `Claude Code`), `task_id` y `task_summary` (qué funcionalidad o corrección desarrolla).
  - `lifecycle_state` (`IMPLEMENTED`, `CONFIRMED`, `INTEGRATED`, `DELIVERED`), `is_mandatory` (`1` por defecto, `0` solo con exclusión registrada), `excluded_by`, `exclusion_reason`.
  - `dirty_files_count`, `untracked_files_count`, `unintegrated_commits_count`, `modified_files_json`.
  - `active_build_pid` (para detectar si hay una compilación en ejecución y evitar interferencias), `disk_usage_bytes` y `last_activity_utc`.
  - `handover_notes` y `last_snapshot_id` (`workstream_id`) para que, si un agente agota su cuota o se desconecta, la superherramienta `project_resume` entregue al siguiente agente el paquete completo de relevo en una sola llamada.
- **Reglas de invariante:**
  1. Todo worktree vinculado al repositorio Git del proyecto o registrado por un agente aparece en `agent_worktrees_catalog` y persiste a través de reinicios del sistema.
  2. Si un worktree contiene modificaciones sin commit (`dirty_files_count > 0`) o si sus pruebas no han sido verificadas, su estado no puede elevarse por encima de `IMPLEMENTED` aunque el agente saliente haya escrito en texto libre que terminó.
  3. El protocolo de relevo (`project_resume`) informa siempre qué worktrees están activos o abandonados, qué archivos están sin confirmar, cuál fue el último snapshot verificable y cuáles son los pasos pendientes antes de integrar.
- **Archivos de implementación:**
  - `include/migratool/database.hpp`
  - `src/database.cpp`
  - `src/git_intel.cpp`
  - `src/agent_supertools.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `agent_os_resilience` (`tests/agent_os_resilience_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Crear o registrar múltiples worktrees (`Agente A`, `Agente B`) con commits propios y modificaciones sin commit, e invocar `project_resume` (o `worktree_inventory`) devuelve para cada uno su rama, agente propietario, tarea, conteo exacto de commits no integrados, archivos sucios y estado del ciclo de vida (`IMPLEMENTED` / `CONFIRMED` / `INTEGRATED` / `DELIVERED`).
  2. Simular la interrupción de un agente a mitad de tarea y consultar `project_resume` desde una nueva sesión recupera el `workstream_id`, el último snapshot `P4`, los archivos modificados y las tareas pendientes en `< 50 ms` de sobrecarga de catálogo.

---

#### `REQ-INTEGRATE-001` — Gestor de integración multi-agente y compuerta de bloqueo de build final
- **ID:** `REQ-INTEGRATE-001`
- **Título:** Gestor de integración multi-agente (`integration_queue`, supertool `integrate_ready`, verificación no destructiva de conflictos con `git merge-tree --write-tree`, bloqueo de build final si hay worktrees obligatorios sin integrar)
- **Categoría:** `MULTI_AGENT_COORDINATION`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El **Integration Manager** de Migratool coordina la convergencia de todos los worktrees hacia la rama principal o de integración mediante la tabla `integration_queue` y la superherramienta `integrate_ready`:
  1. Descubre todos los worktrees y ramas con commits que aún no están ancestros de la rama de integración (`git rev-list --count <target>..<branch>`).
  2. Evalúa de manera **no destructiva** la compatibilidad de fusión de cada rama pendiente contra la rama de integración utilizando `git merge-tree --write-tree <target_branch> <source_branch>`, identificando el árbol resultante o la lista exacta de archivos en conflicto sin alterar el directorio de trabajo ni el índice de Git.
  3. Registra cada elemento en `integration_queue` con su orden de prioridad, estado de conflicto (`CLEAN`, `CONFLICT`, `BLOCKED_DIRTY`, `INTEGRATED`, `EXCLUDED`), pruebas requeridas y resultado de verificación.
  4. Hace cumplir la **Regla Dura de Compilación Final**: bloquea cualquier intento de declarar un build como `INTEGRATED` o `DELIVERED` si existen worktrees obligatorios (`is_mandatory == 1`) con trabajo en estado `IMPLEMENTED` o `CONFIRMED` que no haya sido integrado, a menos que se registre una exclusión explícita con `excluded_by` y `exclusion_reason`.
- **Reglas de invariante:**
  1. La detección de conflictos de integración (`integrate_ready(action="check" | "plan")`) es 100% no destructiva y no ensucia ningún árbol de trabajo.
  2. Ningún worktree con archivos modificados sin confirmar (`dirty`) o con pruebas fallidas puede fusionarse automáticamente en la rama de integración.
  3. Ninguna compilación puede marcarse como versión integrada o entregada (`INTEGRATED` / `DELIVERED`) mientras queden avances obligatorios pendientes en `integration_queue`.
- **Archivos de implementación:**
  - `include/migratool/database.hpp`
  - `src/database.cpp`
  - `src/git_intel.cpp`
  - `src/agent_supertools.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Con dos worktrees (`Agente A` con una funcionalidad en `CONFIRMED` aún no fusionada y `Agente B` en la rama principal), intentar ejecutar o promover `build_canonical` sin integrar al `Agente A` es rechazado con código de bloqueo explícito (`UNINTEGRATED_MANDATORY_WORKTREES`) listando el worktree y los commits faltantes.
  2. Cuando dos ramas modifican la misma línea de un archivo, `integrate_ready` detecta el conflicto vía `git merge-tree --write-tree` y reporta los archivos en conflicto sin modificar el árbol de trabajo activo.
  3. Cuando se registra una exclusión explícita con `excluded_by` y `exclusion_reason`, la decisión queda auditada en SQLite y la compuerta de integración documenta la exclusión en el manifiesto resultante.

---

#### `REQ-BUILD-001` — Manifiestos de compilación inmutables y trazabilidad inversa binario -> código
- **ID:** `REQ-BUILD-001`
- **Título:** Manifiestos de compilación y trazabilidad inversa binario -> código (`build_manifests`, `.migratool/builds/<build_id>/manifest.json`, supertool `build_canonical` y acción `trace_binary` por ruta o SHA-256)
- **Categoría:** `BUILD_PROVENANCE`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Para eliminar la incertidumbre sobre qué ejecutable corresponde a qué código fuente, qué rama o qué agente lo compiló, Migratool registra cada compilación gestionada en la tabla SQLite `build_manifests` y escribe un manifiesto JSON auditable en `.migratool/builds/<build_id>/manifest.json`.
  Cada manifiesto contiene:
  - `build_id`, `project_name`, `requesting_agent`, `worktree_path`, `branch_name`, `commit_sha`.
  - `dirty_state` (`CLEAN` o identificador de snapshot / lista de archivos sin confirmar con sus SHA-256).
  - `build_kind` (`Debug`, `Release`, `Test`, `CanonicalRelease`), `compiler_identity` (ej. `MSVC C++23 14.44`), `cmake_preset` o comando exacto y `build_dir`.
  - `lifecycle_state` (`IMPLEMENTED`, `CONFIRMED`, `INTEGRATED`, `DELIVERED`), `integration_completeness` (`COMPLETE` | `PARTIAL` | `EXPERIMENTAL`), `integrated_worktrees` y `excluded_worktrees`.
  - `artifacts`: arreglo de artefactos producidos (`.exe`, `.dll`, `.pdb`) con su ruta relativa/absoluta, tamaño en bytes y `sha256`.
  - `total_disk_bytes`, `conservation_tier` (`PROTECTED` para builds `DELIVERED`/`INTEGRATED` activos; `TEMP_RETAINED` para builds experimentales recientes) y `created_at_utc`.
  La acción `trace_binary` (dentro de `build_canonical` / `project_health`) permite consultar cualquier ruta de archivo `.exe`/`.dll` o cualquier hash `sha256` y recuperar instantáneamente el commit, rama, worktree, agente, estado de integración y funcionalidades/requisitos incluidos en ese binario.
- **Reglas de invariante:**
  1. Todo binario registrado en un build canónico posee un hash SHA-256 indexado en `build_manifests` que permite trazabilidad inversa $O(1)$ (`SHA-256 -> build_id -> commit_sha + worktrees integrados`).
  2. Si un binario en disco fue compilado desde un worktree con modificaciones sin commit o sin integrar los demás worktrees obligatorios, su manifiesto registra `integration_completeness = "PARTIAL"` o `"EXPERIMENTAL"` y nunca puede ostentar el estado `DELIVERED`.
- **Archivos de implementación:**
  - `include/migratool/database.hpp`
  - `src/database.cpp`
  - `src/agent_supertools.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Ejecutar `build_canonical` para registrar/verificar una compilación genera tanto la fila en `build_manifests` como el archivo `.migratool/builds/<build_id>/manifest.json` con todos los campos obligatorios y hashes SHA-256 reales de los artefactos.
  2. Invocar `trace_binary` pasando la ruta o el hash SHA-256 de un ejecutable devuelve exactamente el `build_id`, `commit_sha`, `worktree_path`, `requesting_agent`, `integration_completeness` y la lista de worktrees/requisitos incluidos.

---

#### `REQ-STORAGE-001` — Auditoría segura de almacenamiento, políticas configurables de carpetas y 4 niveles de conservación
- **ID:** `REQ-STORAGE-001`
- **Título:** Auditoría segura de almacenamiento y políticas configurables de carpetas (`storage_audit`, 9 categorías de directorio y 4 niveles de conservación `PROTECTED`, `TEMP_RETAINED`, `VERIFIED_SAFE_CLEAN`, `NEEDS_REVIEW`)
- **Categoría:** `STORAGE_MANAGEMENT`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  La superherramienta `storage_audit` inventaría el consumo de almacenamiento de los directorios del proyecto, sus worktrees asociados y sus múltiples carpetas de compilación (`build`, `build-old`, `build-debug`, `build-release`, cachés), aplicando una política de clasificación configurable por proyecto (`directory_policies`):
  1. Clasifica cada directorio en una de las 9 categorías canónicas: `SOURCE`, `TEST_SOURCE`, `BUILD_OUTPUT`, `GENERATED`, `DEPENDENCY`, `CACHE`, `DOCUMENTATION`, `USER_DATA` o `UNKNOWN`. Soporta tanto proyectos donde todo el código vive bajo `src/` como proyectos multi-raíz donde `tests/`, `include/`, `tools/`, `scripts/` y `resources/` son carpetas legítimas de código o activos que **jamás** deben tratarse como basura descartable.
  2. Asigna a cada carpeta de compilación, worktree o directorio temporal uno de los 4 niveles de conservación:
     - `PROTECTED`: Contiene archivos rastreados por Git o modificaciones sin commit, es un directorio `SOURCE`/`TEST_SOURCE`/`DOCUMENTATION`/`USER_DATA`, pertenece a un worktree activo o corresponde a un build canónico `DELIVERED`/`INTEGRATED` protegido.
     - `TEMP_RETAINED`: Build o registro de pruebas reciente dentro de la ventana de retención configurada o asociado a una sesión/proceso en ejecución.
     - `VERIFIED_SAFE_CLEAN`: Directorio `BUILD_OUTPUT`, `GENERATED` o `CACHE` 100% reproducible, con `0` archivos fuente o modificaciones sin commit, sin procesos activos bloqueándolo, sin ser el último build `PROTECTED` y con la política de retención cumplida.
     - `NEEDS_REVIEW`: Carpeta huérfana, posible clon o directorio `UNKNOWN` que contiene archivos no clasificados o posibles avances sin confirmar.
  3. Calcula el espacio total utilizado y el espacio potencialmente recuperable (`reclaimable_bytes` = suma exclusiva de `VERIFIED_SAFE_CLEAN`), y cuando se solicita limpieza (`clean_verified_only: true`, con soporte `dry_run: true` por defecto), envía únicamente los elementos `VERIFIED_SAFE_CLEAN` a la Papelera de Reciclaje de Windows sin borrado permanente.
- **Reglas de invariante:**
  1. Ningún directorio clasificado como `SOURCE`, `TEST_SOURCE`, `DOCUMENTATION` o `USER_DATA` puede recibir jamás el nivel `VERIFIED_SAFE_CLEAN`.
  2. Ningún directorio que contenga archivos modificados sin commit o archivos no rastreados con extensión de código fuente dentro de un árbol de compilación o temporal puede limpiarse automáticamente: se promueve a `PROTECTED` o `NEEDS_REVIEW`.
  3. La limpieza nunca se decide solo por el nombre de la carpeta (`old`, `backup`, `temp`, `build`) ni solo por su antigüedad, y toda eliminación autorizada usa la Papelera de Reciclaje de Windows.
- **Archivos de implementación:**
  - `src/agent_supertools.cpp`
  - `src/housekeep.cpp`
  - `src/fs_organize.cpp`
  - `src/database.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `fs_organize_and_usn_regression` (`tests/fs_organize_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. En un proyecto con `src/`, `tests/`, `include/`, `tools/`, `build-release/` (build canónico protegido), `build-old/` (reproducible y antiguo) y `backup-temp/` (con un archivo `.cpp` no confirmado), `storage_audit` clasifica `src/`, `tests/`, `include/`, `tools/` y `build-release/` como `PROTECTED`, `backup-temp/` como `PROTECTED` / `NEEDS_REVIEW` (nunca limpiable) y únicamente `build-old/` como `VERIFIED_SAFE_CLEAN`.
  2. Cambiar la política de directorios del proyecto actualiza de forma determinista la clasificación sin perder protección sobre archivos fuente o sucios.

---

#### `REQ-DISCOVERY-001` — Descubrimiento inteligente de réplicas, clones y trabajo huérfano sin barridos repetitivos
- **ID:** `REQ-DISCOVERY-001`
- **Título:** Descubrimiento inteligente de réplicas, clones y trabajo huérfano (`discover_replicas`, clasificación `MAIN_PROJECT`, `RELATED_WORKTREE`, `POSSIBLE_CLONE`, `ORPHAN_WORK` basada en identidad Git, remotos y hashes de archivos)
- **Categoría:** `PROJECT_DISCOVERY`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  El módulo **Project Discovery & Recovery** (`discover_replicas`) localiza instancias relacionadas de un mismo proyecto dispersas en distintas carpetas, raíces indexadas o unidades (`D:\`, `E:\`, `%TEMP%`), sin depender del nombre de la carpeta y sin escanear todos los discos en cada sesión:
  1. Aprovecha el índice NTFS existente (`fs_roots`, `fs_nodes`, `fs_hashes`), el registro global de workspaces (`projects.sqlite`) y raíces candidatas especificadas para descubrir repositorios y árboles de código, persistiendo los hallazgos en la tabla `discovered_replicas` para consultas posteriores incrementales.
  2. Compara identidad real mediante: commit raíz de Git (`git rev-list --max-parents=0 HEAD`), URLs de remotos Git, ancestros compartidos en el grafo de commits, estructura de directorios e intersección de hashes SHA-256 de archivos fuente.
  3. Clasifica cada instancia descubierta en una de cuatro categorías:
     - `MAIN_PROJECT`: El repositorio principal canónico activo del proyecto.
     - `RELATED_WORKTREE`: Un worktree oficial o vinculado que comparte objetos Git o historial directo con el proyecto principal.
     - `POSSIBLE_CLONE`: Una copia o clon independiente del mismo proyecto (mismo commit raíz o alta superposición de hashes SHA-256) ubicada en otra ruta o unidad.
     - `ORPHAN_WORK`: Un directorio temporal o copia parcial (ej. `%TEMP%\agent-task-*` o carpeta sin `.git` completo) que comparte firmas SHA-256 con el proyecto y contiene archivos modificados o únicos que no están presentes en la rama principal.
  4. Reporta para cada instancia qué cambios ya están incorporados en `MAIN_PROJECT`, cuáles son duplicados exactos y cuáles representan avances únicos no integrados.
- **Reglas de invariante:**
  1. Dos carpetas con el mismo nombre pero con commit raíz distinto y `0%` de coincidencia de hashes de código nunca se fusionan ni se tratan como el mismo proyecto.
  2. Dos carpetas con nombres completamente distintos pero que comparten commit raíz de Git o superposición verificada de hashes SHA-256 de código fuente son vinculadas y clasificadas correctamente.
  3. `discover_replicas` jamás fusiona ni elimina automáticamente carpetas descubiertas: registra la evidencia y marca cualquier instancia con archivos únicos como protegida hasta su revisión o integración.
- **Archivos de implementación:**
  - `src/agent_supertools.cpp`
  - `src/git_intel.cpp`
  - `src/database.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Dado un proyecto principal, un worktree en otra ruta, un clon con nombre distinto y una carpeta temporal sin `.git` pero con archivos fuente del proyecto modificados, `discover_replicas` los clasifica respectivamente como `MAIN_PROJECT`, `RELATED_WORKTREE`, `POSSIBLE_CLONE` y `ORPHAN_WORK`, identificando los archivos modificados únicos en la copia huérfana.

---

#### `REQ-TRACE-001` — Índice consultable de requisitos y matriz de trazabilidad quirúrgica para agentes
- **ID:** `REQ-TRACE-001`
- **Título:** Índice consultable de requisitos y matriz de trazabilidad (`Requisito -> Implementación -> Prueba -> Commit -> Compilación`, supertool `project_health` / `requirements_trace` con entrega quirúrgica por tarea)
- **Categoría:** `TRACEABILITY`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  Para que `docs/PRODUCT_REQUIREMENTS_MASTER.md` y `docs/PRODUCT_REQUIREMENTS_MASTER.json` actúen como especificación viva sin saturar la ventana de contexto de los agentes de IA:
  1. Migratool indexa `docs/PRODUCT_REQUIREMENTS_MASTER.json` (y sincroniza sus metadatos en SQLite en la tabla `requirements_trace_matrix`) manteniendo para cada `req_id` la cadena completa de trazabilidad permanente:
     $$\text{Requisito (`req\_id`)} \longrightarrow \text{Implementación (`implementation\_files`)} \longrightarrow \text{Prueba (`test\_suites`)} \longrightarrow \text{Commit (`commit\_sha`)} \longrightarrow \text{Compilación (`build\_id`)}$$
  2. La operación `requirements_trace` / `project_health` permite filtrar quirúrgicamente por `req_id`, `category`, `file_path`, `query` o `task_context`, devolviendo al agente **únicamente los requisitos relacionados con su tarea activa**, junto con el estado de existencia en disco de sus archivos de implementación, el último resultado conocido de sus suites de prueba y si están incluidos en el último build canónico.
  3. Calcula en tiempo real el porcentaje de cobertura de requisitos del producto por estado (`IMPLEMENTED`, `CONFIRMED`, `INTEGRATED`, `DELIVERED`) y detecta cualquier regresión o deriva si un archivo de implementación o suite de prueba vinculado a un requisito desaparece o falla.
- **Reglas de invariante:**
  1. Ningún agente necesita leer el archivo maestro completo cuando consulta por archivo o área funcional: `requirements_trace` devuelve solo el subconjunto relevante dentro del presupuesto de tokens solicitado.
  2. Cada requisito indexado responde de forma verificable seis preguntas: (a) qué funcionalidad se necesita, (b) en cuáles archivos está implementada, (c) qué prueba demuestra que funciona, (d) en qué commit se incorporó, (e) en cuáles compilaciones está incluida y (f) si sigue funcionando tras cambios posteriores.
- **Archivos de implementación:**
  - `docs/PRODUCT_REQUIREMENTS_MASTER.md`
  - `docs/PRODUCT_REQUIREMENTS_MASTER.json`
  - `src/agent_supertools.cpp`
  - `src/docs_hub.cpp`
  - `src/database.cpp`
  - `src/mcp.cpp`
- **Suites de prueba asociadas:**
  - `env_git_supertools` (`tests/env_git_supertools_tests.cpp`)
  - `knowledge_docs_lineage` (`tests/knowledge_docs_lineage_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. Invocar `project_health` / `requirements_trace` filtrando por `file_path: "src/fs_organize.cpp"` o `category: "MULTI_AGENT_COORDINATION"` devuelve exclusivamente los requisitos aplicables (`REQ-SEC-001`, `REQ-DATA-001`, `REQ-STORAGE-001`, o los `REQ-WORKTREE/INTEGRATE/BUILD`) con sus archivos, suites de prueba y estado de verificación verificados contra el árbol real.

---

#### `REQ-GUI-001` — Panel de supervisión humana de baja carga cognitiva bajo Apple HIG
- **ID:** `REQ-GUI-001`
- **Título:** Panel de supervisión humana de baja carga cognitiva bajo Apple HIG (resumen ejecutivo claro de integraciones pendientes, worktrees con cambios, builds antiguos, espacio recuperable, estado del último build canónico y diseño adaptativo a zoom 200%)
- **Categoría:** `HUMAN_SUPERVISION_GUI`
- **Prioridad:** `P2`
- **Estado:** `CONFIRMED`
- **Descripción técnica:**
  La interfaz gráfica nativa Win32 de Migratool (`src/gui.cpp`, `src/scale_view.cpp`) debe ofrecer un resumen ejecutivo de supervisión humana diseñado bajo los principios de **Apple Human Interface Guidelines (Apple HIG)** — precisión, simplicidad, elegancia, jerarquía visual clara, tema luminoso neutro (*Porcelana Claro*) con contraste WCAG AAA, cero estética gamer/neón, cero emojis decorativos y cero jerga interna innecesaria:
  1. En lugar de abrumar al usuario con decenas de tablas simultáneas, presenta con jerarquía inmediata los indicadores accionables del proyecto activo:
     - **Integración pendiente:** cantidad de avances confirmados listos o pendientes por integrar en la rama principal.
     - **Worktrees con cambios pendientes:** conteo y detalle de worktrees con archivos sin confirmar o ramas de agentes (`Codex`, `Antigravity`, `Claude Code`) aún no fusionadas.
     - **Compilaciones antiguas y espacio potencialmente recuperable:** conteo de builds antiguos y volumen en bytes/GB clasificable como `VERIFIED_SAFE_CLEAN`.
     - **Última compilación verificada (Build Canónico):** estado claro de la última versión integrada (`pruebas completadas`, `commit`, y advertencia explícita si la funcionalidad de algún agente todavía no está incluida en ese ejecutable).
  2. Cumple con escalado real `PerMonitorV2` (4K) y reflujo adaptativo de paneles a **zoom 200%** (`wrap_mode` de 2 filas en barras de herramientas, ocultamiento o compactación bajo demanda de barras laterales y divisores proporcionales con anchos mínimos garantizados para que ningún texto ni control se trunque o superponga).
- **Reglas de invariante:**
  1. El panel de supervisión muestra siempre la distinción honesta entre la última compilación verificada y cualquier funcionalidad de un agente que aún no esté integrada en ella.
  2. Al escalar la interfaz hasta `200%` de zoom o DPI alto (`PerMonitorV2`), ningún botón, contador o etiqueta sufre recorte (`clipping`), solapamiento ni desbordamiento fuera del área cliente.
- **Archivos de implementación:**
  - `src/gui.cpp`
  - `src/scale_view.cpp`
  - `src/comparison_view.cpp`
  - `include/migratool/gui_theme.hpp`
- **Suites de prueba asociadas:**
  - `gui_acceptance` (`tests/gui_acceptance_tests.cpp`)
  - `gui_lifecycle` (`tests/gui_lifecycle.cpp`)
  - `live_sync` (`tests/live_sync_tests.cpp`)
- **Criterios de aceptación medibles:**
  1. La suite automatizada `gui_acceptance` valida la ausencia de solapamientos y recortes en las vistas de la GUI (incluyendo zoom `200%` y vistas del Agent OS / coordinación), verificando contraste WCAG AAA y actualización en vivo desde el catálogo SQLite.

---

### 2.4 Auditoría Técnica Continua e Incremental y Verificación de Afirmaciones (`REQ-AUDIT-001` .. `REQ-AUDIT-004`)

#### `REQ-AUDIT-001` — Motor modular de auditoría técnica continua e incremental
- **ID:** `REQ-AUDIT-001`
- **Título:** Motor modular de auditoría técnica continua e incremental con invalidación por dependencias de headers y `compile_commands.json`
- **Categoría:** `CONTINUOUS_TECHNICAL_AUDIT`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:** Subsistema `code_audit` en C++23 nativo con caché persistente SQLite indexada por `(file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version)`. Reutiliza resultados de archivos sin cambios y re-analiza únicamente archivos modificados y unidades de traducción cuyos encabezados incluidos (`#include`) o banderas de `compile_commands.json` hayan cambiado.
- **Archivos de implementación:** `include/migratool/code_audit.hpp`, `src/code_audit.cpp`, `src/gateway.cpp`
- **Suites de prueba asociadas:** `code_audit` (`tests/code_audit_tests.cpp`)

#### `REQ-AUDIT-002` — Precisión AST sin falsos positivos (C vs C++23, complejidad ciclomática AST, `TEST_CASE` por línea)
- **ID:** `REQ-AUDIT-002`
- **Título:** Separación estricta C vs C++23 (`NULL` vs `nullptr`), complejidad ciclomática AST y desambiguación de `TEST_CASE` por ubicación
- **Categoría:** `CONTINUOUS_TECHNICAL_AUDIT`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:** Extracción sintáctica/AST de funciones y macros de prueba (`TEST_CASE`) identificadas por `(file_path, qualified_name, start_line)`, cálculo de complejidad ciclomática sobre nodos de control de flujo reales (ignorando comentarios y literales de cadena) y aplicación diferenciada de reglas según el lenguaje real.
- **Archivos de implementación:** `include/migratool/code_audit.hpp`, `src/code_audit.cpp`
- **Suites de prueba asociadas:** `code_audit` (`tests/code_audit_tests.cpp`)

#### `REQ-AUDIT-003` — Triage de seguridad consciente de pruebas y cobertura `compile_commands.json`
- **ID:** `REQ-AUDIT-003`
- **Título:** Clasificación de patrones de seguridad en archivos de prueba como `PENDING_VALIDATION` y detección de archivos fuente no compilados
- **Categoría:** `CONTINUOUS_TECHNICAL_AUDIT`
- **Prioridad:** `P1`
- **Estado:** `CONFIRMED`
- **Descripción técnica:** Clasifica patrones de alta entropía, claves ficticias y rutas absolutas dentro de archivos de prueba (`TEST_SOURCE` / `tests/`) como `PENDING_VALIDATION` en lugar de vulnerabilidades `CONFIRMED`, y cruza los archivos fuente contra `compile_commands.json` para identificar código desconectado.
- **Archivos de implementación:** `include/migratool/code_audit.hpp`, `src/code_audit.cpp`
- **Suites de prueba asociadas:** `code_audit` (`tests/code_audit_tests.cpp`)

#### `REQ-AUDIT-004` — Verificador anti-alucinación de afirmaciones de agentes (`verify_claim`) y atribución histórica
- **ID:** `REQ-AUDIT-004`
- **Título:** Evaluación independiente en 5 dimensiones de entregas declaradas por agentes (`verify_claim`) y atribución de regresiones por commit/agente
- **Categoría:** `CONTINUOUS_TECHNICAL_AUDIT`
- **Prioridad:** `P0`
- **Estado:** `CONFIRMED`
- **Descripción técnica:** Verifica independientemente las entregas declaradas por cualquier agente cruzando 5 dimensiones separadas (conteo AST de funciones, evidencia de compilación vigente en `build_manifests`, evidencia de pruebas en `verified_runs`, hallazgos de calidad/complejidad y equivalencia funcional en casos diferenciales) y conecta cada regresión con el commit, rama, agente, prueba y build correspondiente.
- **Archivos de implementación:** `include/migratool/code_audit.hpp`, `src/code_audit.cpp`, `src/multiagent_coordinator.cpp`
- **Suites de prueba asociadas:** `code_audit` (`tests/code_audit_tests.cpp`), `multiagent_coordinator` (`tests/multiagent_coordinator_tests.cpp`)

---

## 3. Matriz Consolidada de Trazabilidad (`Requisito -> Implementación -> Prueba`)

| ID Requisito | Categoría | Prioridad | Estado | Archivos Principales de Implementación | Suites CTest de Verificación |
| :--- | :--- | :---: | :---: | :--- | :--- |
| `REQ-SEC-001` | `SECURITY` | `P0` | `CONFIRMED` | `src/platform.cpp`, `src/workstream.cpp`, `src/fs_organize.cpp`, `src/scale_export.cpp`, `src/database.cpp`, `src/git_intel.cpp`, `src/lsp_client.cpp` | `agent_os_resilience`, `fs_organize_and_usn_regression`, `env_git_supertools`, `durable_inventory_export` |
| `REQ-SEC-002` | `SECURITY` | `P0` | `CONFIRMED` | `include/migratool/core.hpp`, `src/platform.cpp`, `src/workstream.cpp`, `src/fs_organize.cpp` | `agent_os_resilience`, `fs_organize_and_usn_regression` |
| `REQ-SEC-003` | `SECURITY` | `P0` | `CONFIRMED` | `src/platform.cpp`, `src/git_intel.cpp`, `src/lsp_client.cpp`, `src/agent_supertools.cpp` | `serena_workspace_lsp`, `env_git_supertools`, `gateway` |
| `REQ-SEC-004` | `SECURITY` | `P0` | `CONFIRMED` | `src/mcp.cpp`, `src/gateway.cpp` | `gateway`, `serena_workspace_lsp` |
| `REQ-SEC-005` | `SECURITY` | `P0` | `CONFIRMED` | `src/platform.cpp`, `src/analyzers.cpp`, `src/duplicates.cpp`, `src/fs_organize.cpp`, `src/scale_export.cpp` | `durable_inventory_export`, `indexed_semantic_context`, `fs_organize_and_usn_regression` |
| `REQ-DATA-001` | `DATA_INTEGRITY` | `P0` | `CONFIRMED` | `src/fs_organize.cpp`, `include/migratool/fs_organize.hpp` | `fs_organize_and_usn_regression` |
| `REQ-DATA-002` | `DATA_INTEGRITY` | `P0` | `CONFIRMED` | `src/workstream.cpp`, `src/agent_supertools.cpp`, `src/file_toolkit.cpp` | `agent_os_resilience`, `env_git_supertools`, `serena_workspace_lsp` |
| `REQ-DATA-003` | `DATA_INTEGRITY` | `P0` | `CONFIRMED` | `src/workstream.cpp`, `src/agent_supertools.cpp` | `agent_os_resilience`, `env_git_supertools` |
| `REQ-DATA-004` | `DATA_INTEGRITY` | `P0` | `CONFIRMED` | `src/fs_index.cpp`, `src/fs_crawler.cpp`, `src/usn.cpp`, `src/watcher.cpp` | `fs_index_ultra_scale`, `fs_organize_and_usn_regression` |
| `REQ-DATA-005` | `DATA_INTEGRITY` | `P0` | `CONFIRMED` | `src/platform.cpp`, `resources/app.manifest`, `CMakeLists.txt` | `fs_index_ultra_scale`, `fs_organize_and_usn_regression`, `agent_os_resilience` |
| `REQ-DATA-006` | `DATA_INTEGRITY` | `P1` | `CONFIRMED` | `src/platform.cpp`, `src/knowledge.cpp`, `src/scale.cpp`, `src/queries.cpp` | `knowledge_docs_lineage`, `durable_inventory`, `live_sync` |
| `REQ-WORKTREE-001` | `MULTI_AGENT_COORDINATION` | `P0` | `CONFIRMED` | `src/multiagent_coordinator.cpp`, `src/git_intel.cpp`, `src/gateway.cpp` | `multiagent_coordinator`, `env_git_supertools` |
| `REQ-INTEGRATE-001` | `MULTI_AGENT_COORDINATION` | `P0` | `CONFIRMED` | `src/multiagent_coordinator.cpp`, `src/git_intel.cpp`, `src/gateway.cpp` | `multiagent_coordinator`, `env_git_supertools` |
| `REQ-BUILD-001` | `BUILD_PROVENANCE` | `P0` | `CONFIRMED` | `src/multiagent_coordinator.cpp`, `src/gateway.cpp` | `multiagent_coordinator`, `env_git_supertools` |
| `REQ-STORAGE-001` | `STORAGE_MANAGEMENT` | `P1` | `CONFIRMED` | `src/multiagent_coordinator.cpp`, `src/housekeep.cpp`, `src/fs_organize.cpp` | `multiagent_coordinator`, `fs_organize_and_usn_regression` |
| `REQ-DISCOVERY-001` | `PROJECT_DISCOVERY` | `P1` | `CONFIRMED` | `src/multiagent_coordinator.cpp`, `src/git_intel.cpp` | `multiagent_coordinator` |
| `REQ-TRACE-001` | `TRACEABILITY` | `P1` | `CONFIRMED` | `docs/PRODUCT_REQUIREMENTS_MASTER.json`, `src/multiagent_coordinator.cpp`, `src/docs_hub.cpp` | `multiagent_coordinator`, `knowledge_docs_lineage` |
| `REQ-GUI-001` | `HUMAN_SUPERVISION_GUI` | `P2` | `CONFIRMED` | `src/gui.cpp`, `src/scale_view.cpp`, `include/migratool/navigation.hpp` | `gui_acceptance`, `gui_lifecycle`, `live_sync` |
| `REQ-AUDIT-001` | `CONTINUOUS_TECHNICAL_AUDIT` | `P0` | `CONFIRMED` | `include/migratool/code_audit.hpp`, `src/code_audit.cpp`, `src/gateway.cpp` | `code_audit` |
| `REQ-AUDIT-002` | `CONTINUOUS_TECHNICAL_AUDIT` | `P1` | `CONFIRMED` | `include/migratool/code_audit.hpp`, `src/code_audit.cpp` | `code_audit` |
| `REQ-AUDIT-003` | `CONTINUOUS_TECHNICAL_AUDIT` | `P1` | `CONFIRMED` | `include/migratool/code_audit.hpp`, `src/code_audit.cpp` | `code_audit` |
| `REQ-AUDIT-004` | `CONTINUOUS_TECHNICAL_AUDIT` | `P0` | `CONFIRMED` | `include/migratool/code_audit.hpp`, `src/code_audit.cpp`, `src/multiagent_coordinator.cpp` | `code_audit`, `multiagent_coordinator` |

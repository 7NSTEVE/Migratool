# Parche MCP/CLI para Migratool 0.3 (fuentes, no binarios)

Fecha: 2026-11-05. Se aplica sobre el ZIP `Migratool(1).zip` proporcionado.

## Cambios

- `src/mcp.cpp`: `tools/call` entrega el JSON real en `content[0].text` para **todas** las versiones negociadas y conserva `structuredContent` en 2025-06-18 y 2025-11-25. Se mantiene `isError` y la política de limitación del gateway.
- `src/mcp.cpp` / `src/gateway.cpp`: todos los esquemas MCP exponen `project` opcional. Si se suministra, debe ser la ruta absoluta al archivo de configuración. El proyecto predeterminado del servidor no cambia; cada llamada carga su proyecto. Las conexiones/watcher del servidor permanecen asociadas al proyecto de arranque: para mantener el índice del proyecto alternativo al día use su `inventory_sync`, la GUI o un watcher independiente.
- `src/main.cpp`: selección por `--project` > `MIGRATOOL_PROJECT` > `migration.json` del directorio actual o un ancestro. Si no existe ninguna, se emite error y no se selecciona un proyecto al azar.
- `src/main.cpp`: `agent/*` acepta `--json` inline (**incompatible** con `--input` en la misma invocación) y flags directos `--role`, `--file`, `--id`, `--level`, `--actor`, `--action`, `--root`, `--category`, `--language`, `--detail`, `--entity`, `--expected-revision`, `--execute`, más los existentes. `--execute` es un interruptor, no lleva valor.
- `src/main.cpp`: el comando tradicional `mark` admite IDs cortos `F123`/`S456` o rutas absolutas; conserva sincronización previa, normalización de rutas e historial.
- `mt.cmd`: atajo local opcional a `build/delivery/bin/migratool-cli-0.3.exe`. Para invocar `mt` desde cualquier directorio agregue la carpeta del proyecto a `PATH` o instale su propio lanzador. No se modifica el PATH del usuario.
- `tests/gateway_tests.cpp`: nuevas aserciones para el contenido textual MCP, el proyecto opcional, las opciones CLI y el JSON inline.

## Reconstrucción y pruebas en Windows

En `<PATH_TO_MIGRATOOL>`, desde un entorno de desarrollo MSVC x64 con las dependencias originales instaladas:

```powershell
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --output-on-failure
```

`ctest` incorpora las pruebas adicionales dentro de `gateway`. Compruebe también manualmente un cliente MCP real (Codex / Antigravity) porque el empaquetador y las capas de presentación pueden diferir.

## Ejemplos cortos

```powershell
$env:MIGRATOOL_PROJECT = 'E:\Auditorias\Programa\migration.json'
.\mt.cmd agent context --budget-tokens 800
.\mt.cmd agent query --kind files --role source --fields id,path --limit 5
.\mt.cmd agent status --level files --limit 5
.\mt.cmd agent verify --action test --execute
```

`verify --execute` **ejecuta** los casos configurados con los permisos de Windows del usuario. Use solamente configuraciones confiables.

Al iniciar MCP, también puede usar el proyecto de arranque existente. Para consultar otro catálogo sin reiniciar MCP, incluya `"project":"E:/Auditorias/Otro/migration.json"` en `arguments` de `tools/call`; por defecto se sigue usando el proyecto del servidor.

La nueva interfaz y el esquema no garantizan automáticamente compatibilidad con todas las aplicaciones cliente MCP.

## Verificación de esta entrega

**No se compiló ni ejecutó CTest en esta sesión:** el entorno de generación de este ZIP es Linux y no dispone de MSVC, Windows SDK ni runtime de ejecución Windows. Las nuevas aserciones se entregan para reproducir la verificación en la máquina del usuario. Se realizaron comprobaciones estáticas de integridad del parche, pero no se certifica que compile hasta ejecutar las pruebas arriba indicadas.

Este ZIP es una **versión de fuentes para integrar**, no un ejecutable actualizado. No elimine sus catálogos `.migratool`, ni sobrescriba sus bases de datos o la carpeta `build` al aplicar la actualización.

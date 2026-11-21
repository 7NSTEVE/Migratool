# Migratool: navegación y uso cotidiano

El recorrido habitual empieza por las carpetas y los archivos. La GUI usa el mismo catálogo que la CLI y el servidor MCP. El [manual de agentes](docs/MANUAL_AGENTES.md) contiene los comandos y el contrato técnico.

## Recorrido principal

1. Abra **Proyecto → Crear proyecto**. Dé un nombre al proyecto y elija las carpetas del código original, del resultado y de la configuración. Use **Abrir proyecto** si ya tiene un archivo `migration.json`.
2. Abra **Proyecto → Carpetas y filtros** para añadir varias carpetas, elegir qué archivos se analizan y activar la detección de duplicados en carpetas o ZIP. **Tipos de código** permite añadir extensiones o nombres y ajustar el alcance del código generado y las dependencias. **Recursos** reúne los límites del motor. **Configuración JSON** conserva la edición de todos los ajustes.
3. Pulse **Escanear proyecto**. La sección **Actividad** permite consultar los trabajos y, cuando corresponda, pausarlos, reanudarlos, cancelarlos o reintentarlos.
4. En **Migración → Origen y destino**, seleccione un archivo y use **Registrar avance**. Puede pegar la ruta completa de cada destino o elegirlo con el selector. El estado, la nota, el autor y la fecha quedan registrados en el catálogo. **Filtros** muestra opciones para ver pendientes o limitar la lista a los destinos vinculados al origen seleccionado.
5. En **Revisión**, consulte las pruebas, la cobertura, los duplicados y el trabajo existente. Las comprobaciones usan la configuración del proyecto; disponer de un archivo de destino no demuestra por sí solo que su comportamiento esté verificado.

## Dónde está cada función

- **Inicio:** estado del proyecto y acceso a proyectos recientes mediante el menú Proyecto.
- **Migración:** origen y destino, archivos pendientes, carpetas, inventario completo y correspondencias registradas.
- **Actividad:** trabajos, agentes en vivo e historial de cambios.
- **Revisión:** pruebas, cobertura, duplicados, trabajo existente, símbolos, dependencias, búsqueda y pertenencia a Git/compilación.
- **Proyecto:** carpetas y configuración, integración con agentes y diagnóstico local.

Las vistas secundarias se eligen en la lista de cada sección o en **Ir a**. **Ver detalle** muestra las rutas completas y los motivos pendientes de la fila seleccionada. **Copiar rutas** copia las rutas de esa fila. **Detalles técnicos** abre el registro para investigar una operación.

Use **Ctrl+1** a **Ctrl+5** para cambiar de sección, **Ctrl+F** para ir a la búsqueda, **F5** para volver a consultar la vista y **F1** para obtener ayuda. **Ver → Aumentar texto** amplía el contenido hasta el 200 %. **Ctrl+0** restablece el tamaño. **Ver → Barra lateral** permite usar un selector de secciones y deja más espacio al contenido. La ventana adapta la comparación a paneles contiguos, paneles apilados o un selector de lista según el espacio.

**Exportar inventario** está en **Migración → Todos los archivos** y **Actividad → Trabajos del proyecto**. Elija un destino nuevo y un formato. La exportación aparece en Trabajos, donde puede pausarla, continuarla o cancelarla. Seleccione el resultado guardado para ver su ruta completa y la huella SHA-256.

## Cómo interpretar el avance

- **Sin destino:** falta registrar una correspondencia para ese archivo.
- **Avance parcial:** existen correspondencias, pero quedan unidades o comprobaciones pendientes.
- **Migración registrada:** una persona o agente declaró terminada la migración con referencias vigentes.
- **Verificado en casos:** hay evidencia vigente de los casos configurados. Ese estado tiene un alcance concreto; no acredita todas las entradas posibles.
- **Por calcular:** todavía no existe una base suficiente para mostrar un porcentaje global definitivo. El inventario, el análisis, la declaración de migración y las pruebas se presentan por separado.

El cálculo detallado usa los símbolos inventariados, con una unidad por archivo cuando no hay desglose de símbolos. Las rutas y la evidencia completa siguen disponibles para los agentes y en los detalles del archivo.

## Localizar y comprobar trabajo anterior

Abra **Revisión → Trabajo existente** y pulse **Importar trabajo**. El formulario permite elegir un catálogo SQLite de archivos o un registro SQLite de recuperaciones, localizarlo con el selector y limitar la importación a una carpeta del catálogo. El formato se detecta automáticamente. El selector de procedencia permite indicarlo explícitamente cuando haga falta. El autor identifica la operación en el historial.

La comprobación por SHA-256 está seleccionada inicialmente. Puede elegir registrar candidatos para verificarlos después. El tamaño del lote va de 1 a 1000 registros y permite mantener la ventana disponible mientras se procesa el trabajo. **Cancelar** interrumpe la comprobación sin confirmar el lote incompleto. El origen externo se abre en modo de solo lectura.

Si quedan registros, aparece **Continuar importación**. Procesa el siguiente lote con los mismos ajustes. Al seleccionar un resultado se habilita **Revalidar selección**, que vuelve a leer sus archivos y compara sus hashes con la procedencia registrada. El estado y la fecha de comprobación se actualizan y la acción queda en el historial. La comprobación conserva las notas y correspondencias de migración.

Un hash de procedencia ausente queda explicado como **Falta el hash de procedencia**. Una salida que cambió o que está abierta para escritura no obtiene el estado de par comprobado. La revalidación contrasta los hashes registrados en Migratool. Para incorporar cambios en el registro externo, impórtelo de nuevo.

La continuación de importación se mantiene durante la sesión de la ventana. Las filas ya confirmadas permanecen en el catálogo del proyecto después de cerrar. Una nueva importación del mismo origen actualiza su evidencia sin duplicar las filas de procedencia.

## Criterios de diseño de la interfaz

La referencia es la edición consultada el 12 de noviembre de 2026 de las [Human Interface Guidelines de Apple](https://developer.apple.com/design/human-interface-guidelines). Migratool aplica esos criterios con controles nativos de Windows y tipografía Segoe UI.

- **Navegación.** Cinco secciones mantienen dos niveles de jerarquía. El título identifica la vista y el proyecto. Los menús dan acceso por teclado a todas las vistas. En ventanas estrechas, el selector mantiene el acceso a las mismas secciones. [Sidebars](https://developer.apple.com/design/human-interface-guidelines/sidebars), [Layout](https://developer.apple.com/design/human-interface-guidelines/layout), [The menu bar](https://developer.apple.com/design/human-interface-guidelines/the-menu-bar).
- **Acciones.** La operación principal depende de la pantalla y las opciones de diagnóstico se despliegan a petición. Los formularios usan etiquetas visibles, conservan los valores al detectar un error y señalan lo que hace falta. [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars), [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons), [Text fields](https://developer.apple.com/design/human-interface-guidelines/text-fields).
- **Comparación y lectura.** Cada lado tiene una etiqueta propia y el separador admite ajuste con el puntero. Las listas consultan páginas acotadas. Los detalles muestran texto seleccionable y rutas completas. [Split views](https://developer.apple.com/design/human-interface-guidelines/split-views), [Lists and tables](https://developer.apple.com/design/human-interface-guidelines/lists-and-tables).
- **Accesibilidad.** El tamaño de texto se puede ampliar, las acciones conservan nombres de accesibilidad y los estados se expresan con palabras. La paleta consulta los colores de alto contraste del sistema. [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility), [Typography](https://developer.apple.com/design/human-interface-guidelines/typography), [Color](https://developer.apple.com/design/human-interface-guidelines/color).
- **Progreso.** El trabajo del catálogo y las lecturas se ejecutan en segundo plano. Cuando la duración de una lectura todavía se desconoce, aparece un indicador sin porcentaje. Los controles de trabajo reflejan el estado de la tarea seleccionada. [Progress indicators](https://developer.apple.com/design/human-interface-guidelines/progress-indicators).
- **Configuración y ayuda.** Los ajustes habituales se agrupan por tarea, el editor JSON permanece disponible y la ayuda se puede abrir cuando hace falta. [Settings](https://developer.apple.com/design/human-interface-guidelines/settings), [Offering help](https://developer.apple.com/design/human-interface-guidelines/offering-help).

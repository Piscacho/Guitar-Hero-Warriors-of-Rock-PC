# Sesión 4 — Jump tables, hilos y sistema de archivos

Fecha: 2026-10-26. Sin commits.

## Resultado

`project/build/GH6.exe` (desde `Recomp\run\`: `..\project\build\GH6.exe ..\xex\basefile.bin ..\xex\default.xex`):
pasa el CRT y el init, crea 8 hilos reales, indexa el ISO (1383 entradas), inicializa el vídeo y lee del disco
`compress.toc.xen`, `engine_params.qb.xen`, `materiallibrary.bin.xen` y `boot_legal_eu.img.xen`.
**Envía su primer frame (`VdSwap #1`)** y empieza a cargar `qb.pak.xen`/`qb.pab.xen`. **Ya no se cae**: a los 40 s sigue vivo,
pero sin más salida (parece esperar a la GPU; ver «Próximos pasos»). Log: `run11.log`.

## 1. Jump tables

- `tools/jt_finder.py`: detector propio que **sigue los registros hacia atrás** desde cada `bctr`, ignorando los `nop`.
  - Tipos: `ABS` (lwzx), `SHRT` (lhzx + base) y `BYTE` (lbzx [+ rlwinm] + base).
  - Busca el límite (`cmplwi` + `bgt`/`ble`) hacia atrás desde el bloque o desde los `b`/`bc` que llegan a él, y tiene en cuenta los alias por `mr`.
  - Segunda pasada para **switches anidados**: si el bloque es un caso de otra tabla con el mismo registro, hereda su límite y su `default`.
    Es el patrón dominante en GH: la tabla queda tras el epílogo y la tabla de datos va inline justo después del `bctr`.
- **118 tablas nuevas** → `config/gh6_switch_tables_extra.toml`. En total hay 474 en `config/gh6_switch_tables_all.toml`
  (la concatenación de XenonAnalyse y las extra, que es la que usa `gh6.toml`).
- Quedan **6 sin resolver** (0x8232251C, 0x82322594, 0x82322664, 0x823226E0, 0x82561380, 0x8257759C): el índice llega por
  varios caminos y no hay comparación cerca. Si se ejecutan, `GuestCallIndirect` lo dirá.
- 3 funciones más en `functions` (26 en total). Empezaban 8 bytes antes de su entrada `.pdata` (son destinos de `bl`) y el analizador las cortaba en el `bctr`.

## 2. Cambios en XenonRecomp (`recompiler.cpp`, copia local)

Cambios hechos a mano con el editor. Todos los cambios del clon están en `xenonrecomp_gh6.diff`:
- `vupkd3d128` tipos 2 (NORMPACKED32 2_10_10_10), 3 (FLOAT16_2) y 5 (FLOAT16_4).
- `vpkd3d128` tipos 2 (NORMPACKED32) y 3 (FLOAT16_2).
- `vcmpbfp128` (comparación de límites, con su forma de registro).
- `blrl` (llamada indirecta a LR).
- `divw/divwu/divd/divdu` → `ppc_div*()`, que devuelven 0 al dividir entre 0 o en `INT_MIN/-1` (en x86 era una excepción 0xC0000094).
- Funciones auxiliares en `compat/ppc_indirect.h` (incluido a la fuerza): `ppc_xenos_half_to_float`, `ppc_float_to_xenos_half` y `ppc_div*`.
- Resultado: **0 `__builtin_debugtrap`** en el código generado, 0 errores.

## 3. Hilos y sincronización (`runtime/threads.cpp`, `runtime/objects.h`)

- Tabla de handles compartida (`KObject`, cada uno con su `HANDLE` de Win32) y mapa puntero de invitado → objeto, para las funciones `Ke*` que reciben punteros.
  Los objetos que el juego inicializa en su propia memoria se crean al vuelo a partir de su cabecera (tipo en el byte 0, estado en +4).
- `ExCreateThread`: hilo nativo con su `PPCContext`, PCR, TLS y pila. Respeta `CREATE_SUSPENDED` y `xapi_startup`
  (llama a `xapi_startup(start_address, start_context)`). Además: `NtResumeThread`, `ExTerminateThread`, prioridad y afinidad (no-op).
- Eventos (`NtCreateEvent`, `NtSet/Clear/PulseEvent`, `KeSet/ResetEvent`), mutex, semáforos y temporizadores (sin APC).
- Esperas: `NtWaitForSingleObjectEx`, `KeWaitForSingleObject`, `NtWaitForMultipleObjectsEx`, `KeWaitForMultipleObjects` (timeouts de 100 ns, relativos o absolutos).
- Secciones críticas y spinlocks **atómicos** (CAS sobre el campo de propietario, en memoria de invitado).
- `NtClose`, `NtDuplicateObject`, `ObReferenceObjectByHandle` (crea un objeto de invitado si hace falta).

## 4. Sistema de archivos (`runtime/fs.cpp`)

- **Lee directamente del ISO**, sin extraerlo (XDVDFS en 0xFD90000; índice en memoria al arrancar).
  `game:`, `d:`, `\Device\Cdrom0` y enlaces de `ObCreateSymbolicLink` → ISO.
- Otros dispositivos (`cache:`, `hdd:`, `\Device\Harddisk0\...`) → carpeta del PC `userdata\<dispositivo>\...` (lectura y escritura).
- Implementado: `NtCreateFile` (create_options en r1+0x54), `NtOpenFile`, `NtReadFile`, `NtReadFileScatter`, `NtWriteFile`,
  `NtQueryInformationFile` (clases 4, 5, 14 y 34), `NtSetInformationFile` (14, 19 y 20), `NtQueryFullAttributesFile`,
  `NtQueryVolumeInformationFile` (1, 3 y 5), `NtQueryDirectoryFile` (una entrada por llamada, con comodines), `NtFlushBuffersFile`, `NtCancelIoFile`.
- Argumentos de `GH6.exe`: `basefile.bin default.xex [ruta_iso] [carpeta_datos]`. El ISO por defecto es el de `Base\Juego\...`.
- Registro de aperturas activo (`[fs] abrir ...`); se desactiva con la variable de entorno `GH6_NOLOGFILES=1`.

## 5. GPU provisional (`runtime/gpu_stub.cpp`)

No dibuja. Acepta `VdInitializeEngines`, la calibración de EDRAM (`VdIsHSIOTrainingSucceeded` = 1, sin ella el juego se para), el modo y la gamma de vídeo, etc.
La "GPU" copia `CP_RB_WPTR` (0x7FC80714) al read pointer de writeback cada 1 ms, y un hilo llama a la rutina de
`VdSetGraphicsInterruptCallback` a 60 Hz (r3 = 0, VBlank). `VdSwap` solo cuenta frames.

## 6. Diagnóstico

- `[crash] funcion de invitado aproximada`: aproximada de verdad (el orden de las funciones en el exe no es fiable).
  Para afinar, desensamblar con `toolchain\...\bin\llvm-objdump.exe -d --start-address=0x14<offset>` sobre `GH6.exe`.

## Próximos pasos

1. **Ver dónde espera** tras `qb.pab.xen`, volcando las pilas de los hilos o registrando las esperas largas. Lo más probable es que espere
   a la GPU: un *fence* o `VdGetSystemCommandBuffer`/EOP en el ring buffer. Con una GPU real, o una que procese los paquetes PM4
   de *event write*/fences, se desbloquearía. Esto ya es la sesión de GPU.
2. Los cambios a XenonRecomp (los de todas las sesiones) están guardados en `xenonrecomp_gh6.diff` (`git -C XenonRecomp apply` sobre un clon limpio). `tools/patch_recompiler.py` solo cubre los de la sesión 2.
3. Las 6 jump tables pendientes, si llegan a ejecutarse.
4. Imports aún en stub que ya aparecen: `XamNotifyCreateListener`/`XNotifyGetNext` (notificaciones del sistema),
   `ExRegisterTitleTerminateNotification`, `KeInitializeDpc`, `VdInitializeScalerCommandBuffer`, `VdPersistDisplay`, `NtYieldExecution`, `RtlFillMemoryUlong`.

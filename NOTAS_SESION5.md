# Sesión 5 — Dónde se bloqueaba y arranque de la GPU

Fecha: 2026-10-26. Sin commits. Los logs de todas las ejecuciones se han movido a `logs/`.

## Resultado

- **El juego renderiza frames sin parar**: unos 35 swaps por segundo, con 1.560 frames, unos 80.000 draws y 165.000 paquetes PM4 en 45 s.
  Hay un procesador de comandos de GPU real, aunque todavía **no dibuja**: los draws solo se cuentan.
- Hay una ventana Win32 que muestra el estado de la GPU (frames, draws, paquetes, interrupciones, frontbuffer). Se desactiva con `GH6_NOWINDOW=1`.
- **Bloqueo actual**: tras `materiallibrary.bin.xen` el hilo principal se queda esperando a que se vacíe la cola de tareas del juego (ver abajo).
  Antes de que existiera el procesador de comandos llegaba a la pantalla legal y a `qb.pak`.

## Bloqueos encontrados y resueltos

1. **Contador de VBlank parado** (el hilo principal hacía `Sleep(1)` en bucle esperando una operación asíncrona temporizada).
   - `sub_8239FDD0` devuelve un contador de 64 bits en 0x8298C1E0 (estructura de tiempo 0x8298BFA8, campo +0x238).
   - Lo incrementa `sub_8239FB20`, que es el callback de VBlank registrado por `sub_8239FCC0` → `sub_822E59D8` (`stw r4,0x4174(dispositivo)`).
   - La rutina de interrupciones de D3D (0x822DB470) solo atiende el VBlank (r3 = 0) si el registro **`D1MODE_VBLANK_STATUS` (0x7FC86544)**
     tiene el bit 0 a 1. **Solución**: el hilo de VBlank lo pone a 1 antes de cada interrupción.
2. **Fence de GPU nunca cumplida**: `sub_822DB898` (BlockOnFence de D3D) espera a que el valor apuntado por dispositivo+0x2A90 alcance la fence.
   **Solución**: el procesador de comandos PM4 (abajo) ejecuta `EVENT_WRITE_SHD` y los registros scratch.
3. `KfAcquireSpinLock`/`KfReleaseSpinLock`/`NtYieldExecution` eran stubs: ahora son reales (spinlock atómico).

## GPU (`runtime/gpu_cp.cpp`, `gpu.h`, `gpu_window.cpp`, `gpu_stub.cpp`)

- **Direcciones físicas**: la vista de CPU de una dirección física P es `0xA0000000 | (P & 0x1FFFFFFF)`. El ring buffer (0x16A10000), el writeback
  (0x16A2003C) y los punteros de los paquetes son físicos.
- **Ring buffer**: `VdInitializeRingBuffer(phys, log2)` → tamaño de `1 << (log2 + 3)` bytes (como Xenia). El write pointer lo escribe la CPU en
  `CP_RB_WPTR` (registro 0x1C5 = 0x7FC80714). Tras procesar se actualizan `CP_RB_RPTR` y el writeback (BE).
- **Registros**: archivo de registros en 0x7FC80000 + índice·4 (BE). Efectos implementados: scratch REG0..7 (0x578+) se copian a
  `SCRATCH_ADDR` (0x1DD) según `SCRATCH_UMSK` (0x1DC); `COHER_STATUS_HOST` (0xA2F) se completa al instante.
- **Paquetes**: tipo 0/1/2 y tipo 3 con `INDIRECT_BUFFER(_PFD)`, `WAIT_REG_MEM`, `REG_RMW`, `COND_WRITE`, `MEM_WRITE`, `REG_TO_MEM`,
  `EVENT_WRITE_SHD`, `EVENT_WRITE_EXT`, `INTERRUPT` (callback con r3 = 1 en cada CPU de la máscara, poniendo KPCR+0x10C) y `DRAW_INDX(_2)` (solo se cuentan).
  Las escrituras de la GPU aplican `GpuSwap` según los 2 bits bajos de la dirección (8in16/8in32/16in32).
- **VdSwap** escribe en el hueco de 64 dwords que reserva el juego un paquete propio `XE_SWAP` (opcode 0x64: 'SWAP', frontbuffer, ancho, alto),
  como Xenia. El procesador lo cuenta y avisa a la ventana.
- **VBlank**: hilo a 60 Hz → `D1MODE_VBLANK_STATUS` = 1 → interrupción source 0 en la CPU 2. Las interrupciones se serializan con un mutex.
- `GH6_PM4TRACE=1` registra los primeros 400 paquetes (`[pm4] ...`). Cada 2 s se registra `[gpu] rptr= wptr= paquetes= draws= irq= swaps=`.
  La traza muestra la secuencia normal de D3D: `ME_INIT`, IB, `WAIT_REG_MEM` (0xA31), `REG_RMW`, `INVALIDATE_STATE`, `IM_LOAD_IMMEDIATE` (shaders),
  `DRAW_INDX_2` y `EVENT_WRITE_SHD` (fences).

## Diagnóstico nuevo (`runtime/diag.cpp`, cambios en `threads.cpp`)

- El ejecutable lleva **símbolos**: `/Z7` + `-gline-tables-only` + `/DEBUG` → `project/build/GH6.pdb` (unos 100 MB).
- `GH6_SAMPLE=<s>`: a los s segundos vuelca 3 veces la pila host y el `lr` de invitado de cada hilo.
  Para traducir: `toolchain/.../bin/llvm-symbolizer.exe --obj=project/build/GH6.exe 0x14xxxxxxx` (devuelve `__imp__sub_XXXXXXXX` y la línea de `ppc/`).
- Las esperas de más de 3 s se registran (`[wait] tid ... lr=...`).
- `tools/ppcdis.py`: desensamblador PPC mínimo para inspeccionar `basefile.bin` (en las sesiones se usó en línea desde Python).

## Bloqueo pendiente: la cola de tareas del juego

- El hilo principal: `_xstart` → `sub_82101948` → `sub_82551028`, que espera a que `*(0x8286B210 + 248) + *(… + 252) == 0` (tareas pendientes),
  mientras ayuda a ejecutar tareas con `sub_82646CC0`. Los 3 trabajadores (`0x825FF2B8`) también están en `sub_82646CC0` sin encontrar nada.
- `sub_82646CC0` recorre 3 colas; `sub_82646B00` solo toma una tarea si su grupo (tarea+12) está libre: `sub_826469B8` comprueba que todos
  los grupos de los que depende tienen sus contadores (+0 y +28 de cada entrada de 32 bytes en `*(base+260)`) a 0.
  → **Algún grupo de dependencia no llega nunca a 0.**
- Hipótesis a comprobar:
  1. Una tarea que espera a otro subsistema todavía en stub (`XamNotifyCreateListener`/`XNotifyGetNext`, audio XAudio/XMA, `KeInitializeDpc`/DPC...).
  2. Los hilos `0x824247B8` y `0x8241D050` llevan desde el arranque en bucles de `KeDelayExecutionThread`: podrían ser streaming/IO esperando algo.
  3. Diferencias de orden respecto a antes del procesador de comandos (antes sí llegaba a la pantalla legal).
- Siguiente paso concreto: instrumentar con un *mid-asm hook* o un `GUEST_IMPL` que reemplace `sub_82646B00`/`sub_826469B8` (las funciones
  son *weak*, así que se pueden sobrescribir) para volcar qué grupo bloquea y qué tarea (puntero de función) está pendiente.

## Próximos pasos (GPU, sesión siguiente)

1. Resolver el bloqueo de la cola de tareas (arriba).
2. Backend de dibujo: estado de registros → pipeline (D3D12 o Vulkan), traducción de shaders Xenos (microcódigo de `IM_LOAD(_IMMEDIATE)`), texturas
   (destilado y formatos), EDRAM/resolves y, por último, presentar el frontbuffer (`XE_SWAP` → frontbuffer físico, 1280x720 típico).
   Reutilizar Xenia (BSD): `gpu/shader_translator`, `texture_info`, `draw_util`, `render_target_cache`.

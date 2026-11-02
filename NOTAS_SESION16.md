# Sesión 16 — Reloj del kernel: el juego sale de la pantalla de título

Fecha: 2026-10-27. Sin commits.

## Causa del bloqueo en el título

`KeTimeStampBundle` (variable importada del kernel) **nunca se actualizaba**. El kernel real la refresca cada milisegundo y el juego
(y FMOD) leen de ella el contador de ticks directamente (equivalente a `GetTickCount` en línea). Con el valor fijo, el tiempo del juego
estaba congelado: las animaciones de la interfaz no avanzaban (texto en x ≈ 2821) y el *stream* de FMOD no volvía a pedir datos.

## Cambios

- `runtime/kernel.cpp`: `StartTimeStampBundle(obj)` — un hilo nativo (`timeBeginPeriod(1)` + `Sleep(1)`) escribe en big-endian
  `+0x00` interrupt_time (u64, unidades de 100 ns desde el arranque), `+0x08` system_time (FILETIME), `+0x10` tick_count (ms).
- `runtime/xam.cpp`: `XamAvatarInitialize` devuelve error (0xFFFFFFFF), como Xenia; el juego toma la ruta sin avatares y llama a `XamAvatarShutdown`.
- `runtime/diag.cpp`: la pila del muestreo (`GH6_SAMPLE=<s>`) llega a 28 marcos.
- `tools/symsample.py`: traduce las líneas `[sample]` de un log a nombres de función con `llvm-symbolizer` (`python tools\symsample.py logs\runNN.log [ronda]`).
- `runtime/gpu_window.cpp`: `GH6_DUMPFRAME` acepta una lista (`800,1200,1600`) y vuelca al alcanzar **o pasar** cada swap (antes exigía el valor exacto y a menudo no volcaba).

## Resultado

- FMOD lee de forma continua (más de 3000 lecturas del códec en 60 s, antes 1).
- La interfaz entra en pantalla (x ≈ 177).
- Sin pulsar nada: tras el título aparece una escena 3D oscura (escenario, `run/fb_1500.png` de run99).
- Con `GH6_AUTOSTART=1`: aparece el **fondo animado del menú principal** (arte de WoR, `run/fb_1200.png` y `run/fb_1600.png` de run102).
- Unos 27 swaps/s. Faltan el texto y los elementos del menú.

## Diagnóstico del hilo principal

El hilo principal pasa casi todo el tiempo en `sub_8242ABE8`: es una **espera activa del fin de frame** (gira hasta que el contador +24 alcanza el +20
del objeto 0x829BB2F0). Es la sincronía normal del bucle de frames, no un bloqueo.

## Pendiente (siguiente sesión)

- Draws omitidos que se ven en el log de D3D12 (`omitidos prim=…` y `mem=…`): tipos de primitiva sin soporte y draws sin memoria válida.
  Probablemente aquí está el texto del menú (fuentes) que falta.
- La escena 3D sale muy oscura: revisar depth/stencil, formatos de resolve que no son 8888 y la iluminación (constantes).
- Salida de audio real (WASAPI) con las muestras de `XAudioSubmitRenderDriverFrame`.

# Sesión 17 — Menús completos con texto y partidas guardadas

Fecha: 2026-10-27. Sin commits.

## Resultado

Con `GH6_AUTOSTART=1` el juego llega a «Getting Started», al lobby de la banda (selección de dificultad, Join, Quest,
personajes Echo Tesla / Pandora / Lars Umlaut) y a los avisos, **con todo el texto y la interfaz** (`run/fb_1600.png`, `fb_2000.png`, `fb_2400.png`).
Crea, guarda y vuelve a cargar la partida (`run/userdata/content/00000001_progress`).

## Cambios en la GPU (`runtime/gpu_d3d12.cpp`, `gpu_shader.cpp`, `gpu_window.cpp`)

- **Vertex buffers deduplicados por frame** (`g_vbCache`, clave dirección + tamaño): antes cada draw copiaba su buffer entero
  y el anillo de 64 MB se agotaba → unos 133 000 draws omitidos («mem»). Ahora 2.
- **`kill` del traductor**: generaba `float4(any(...))` (HLSL inválido, X3014). Ahora es `(any(...) ? 1.0 : 0.0).xxxx`.
  Afectaba a 5 pixel shaders con prueba alfa (fuentes): era el texto del menú.
- **`RB_COLOR_MASK` (0x2104)** respetado en el PSO. Las pasadas con máscara 0 (solo profundidad/stencil) se omiten mientras no haya depth buffer
  (antes pintaban la pantalla de blanco).
- Los HLSL que no compilan se guardan en `run/hlsl_err/<nombre>.hlsl`.
- `GH6_DUMPFRAME` admite una lista y vuelca al alcanzar o pasar cada swap.

## Cambios en XAM/kernel (`runtime/xam.cpp`, `fs.cpp`, `kernel.cpp`)

- `XamShowDeviceSelectorUI`: elige el disco duro (ID 1) y emite XN_SYS_UI on/off a todos los listeners (`BroadcastNotification`).
- `XamContentGetDeviceData`: disco duro de 20 GB con 16 GB libres.
- Partidas guardadas: `XamContentCreateEx` monta `<raiz>:` en `userdata\content\<tipo>_<archivo>` (`FsMountContent`) y guarda
  el XCONTENT_DATA en `<carpeta>.xcd`; `XamContentCreateEnumerator`/`XamEnumerate` lo devuelven; `XamContentClose`, `XamContentSetThumbnail`,
  `XamContentGetCreator` (siempre del usuario 0; sin él el juego borraba y recreaba la partida en bucle) y `XamContentDelete`.
- `RtlTimeToTimeFields`.

## Pendiente

- Navegar los menús con el teclado hasta empezar una canción (la pulsación automática de Start no elige dificultad).
- Personajes 3D sin dibujar en el lobby; escena 3D oscura → depth buffer, formatos de resolve que no son 8888 y puntos (`omitidos prim`, tipo 1).
- `XamReadTileToTexture` (imagen del jugador), `VdPersistDisplay`, `XamBackgroundDownloadSetMode` siguen como stubs.
- Salida de audio (WASAPI).

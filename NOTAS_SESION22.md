# Sesión 22 — Rendimiento (resolves por GPU), cube maps y presentación directa

Fecha: 2026-10-28. Sin commits. Avance estimado: **~80 %** (punto de parada pedido por el usuario).

## Rendimiento

Medido con las líneas `[gpu] rptr ... swaps=` (cada 2 s):

| Situación | Antes | Ahora |
|---|---|---|
| Menús / cinemáticas | 20-27 fps | **60 fps** (vsync) |
| Durante la canción | ~10 fps | **42-61 fps** |

Cambios (`runtime/gpu_d3d12.cpp`, `gpu_window.cpp`):
- **Resolves de color por GPU**: 8888→8888 sin MSAA se copian a una textura de la GPU asociada a (dirección, ancho, alto)
  (`g_resolved`, `g_resolvedLast`). `BindTexture` la usa directamente. Si se lee con otro tamaño, se copia la región en la GPU (clave derivada).
  Las diferencias de endian/swapRB se resuelven en el SRV (permutación de bytes por XOR: 8in16 = 1, 8in32 = 3, 16in32 = 2).
- **Resolves de profundidad por GPU**: una pasada de pantalla completa (`kDepthCopyHlsl`) lee el plano de profundidad y escribe
  R32_FLOAT (lectura como k_24_8) y RGBA8 con los bits 24_8 (lectura como 8888). Heap RTV propio (`g_rtvHeap2`, 32 entradas dobles).
- **Presentación directa**: `D3D12PresentResolved` dibuja en la ventana la textura resuelta del frontbuffer, sin leer la memoria.
  Las capturas (`DUMP`, `GH6_DUMPFRAME`) leen esa textura a la CPU solo cuando se piden. `GH6_FBMEM=1` vuelve al camino por memoria.
- Rectángulo de resolve **inclusivo** (+1; antes faltaba la última fila y columna) y recortado al destino.
- Liberación diferida de recursos por frame (`g_deferredRelease`); antes, liberar una textura en uso colgaba la GPU (`DEVICE_HUNG`).
- `GH6_CPURESOLVE=1` fuerza los resolves por CPU (diagnóstico). Estadística cada 120 presentaciones: texturas subidas y MB de vértices por frame.
- `Map` del readback comprobado (si el dispositivo se pierde se registra el motivo en vez de fallar).

## Gráficos

- **Texturas cubo**: `CUBE` (vop 18) implementado como el intérprete de Xenia y guarda la dirección en `cubeDir`. El `tfetch` de cubo
  muestrea `TextureCube XTexC[16]` (t33). 48 descriptores por draw. `BindTextureCube`: 6 caras 2D consecutivas alineadas a 4 KB (8888/DXT/16F).
  Quitó las siluetas negras de los paneles del lobby.
- `tfetch` 1D/cubo sin soporte se avisa en el log.

## Diagnóstico

- El procesador de comandos se registra para `GH6_SAMPLE` (entrada ficticia 0xC0C0C0C0).
- `GH6_D3D12_DEBUG=1` vuelca los mensajes de la capa de depuración si está instalada (en este equipo no lo está).

## Pendiente (el 20 % restante)

- **Desenfoque fuerte** durante la canción: comprobar si es la profundidad de campo real del local o un fallo (comparar con `GH6_CPURESOLVE=1` en la misma cámara).
- **Personajes del lobby**: ya no salen negros, pero siguen sin verse sobre los paneles.
- Copias de vértices: unos 17 MB por frame (se copia el buffer entero de cada stream). Caché persistente en la GPU.
- Los guiones de entrada dependen del tiempo y se desfasan al cambiar el rendimiento: hacerlos robustos (esperar a pantallas o repetir pulsaciones).
- Avisos de FFmpeg con XMA ("Reserved bit", "skip -171 bits") en algunos streams.
- Mando real (XInput), DLC, lanzador y empaquetado, limpieza y estabilidad en sesiones largas.

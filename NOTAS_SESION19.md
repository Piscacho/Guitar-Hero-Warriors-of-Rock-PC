# Sesión 19 — Depth buffer, culling, resolves de profundidad y más formatos de textura

Fecha: 2026-10-27. Sin commits.

## Cambios (`runtime/gpu_d3d12.cpp`)

- **Superficies de profundidad**: una D32_FLOAT_S8X24 por (base de `RB_DEPTH_INFO` 0x2002, pitch de `RB_SURFACE_INFO`), heap DSV de 64 entradas (`GetDepthRt`).
- **Estado depth/stencil** desde `RB_DEPTHCONTROL` (0x2200): z, escritura z, función z, stencil de cara frontal y trasera; ref/máscaras de `RB_STENCILREFMASK` (0x210C).
  Forma parte de la clave del PSO. Las pasadas con máscara de color 0 se dibujan si usan depth/stencil (antes se omitían).
- **Culling** desde `PA_SU_SC_MODE_CNTL` (0x2205): bits 0/1 cull frontal/trasero, bit 2 cara frontal horaria. No se aplica a rect lists ni a quads expandidos.
- **Clear de profundidad** en el resolve (`RB_COPY_CONTROL` bit 9, valor de `RB_DEPTH_CLEAR` 0x231D; `DecodeDepth24` maneja 24 bits unorm y 20e4).
- **Resolve de profundidad** (src 4): copia el plano de profundidad (R32) y lo escribe en memoria como 24_8 tiled con el endian del destino.
- **Formatos de textura nuevos**: 1_5_5_5, 5_6_5, 4_4_4_4, 8_8, 16, 16_FLOAT, 2_10_10_10, 16_16, 16_16_FLOAT, 32_FLOAT, 24_8 y 24_8_FLOAT (convertidos a R32_FLOAT),
  16_16_16_16 (unorm/float), 32_32_FLOAT, 32_32_32_32_FLOAT, DXN (BC5, normal maps) y DXT5A (BC4). Los texels de 16 bits extraen su mitad de la palabra de 32 bits ya con el swap aplicado.
- Logs únicos por combinación: formatos de textura sin soporte (con base y frame) y resolves (src, formato, pitch, MSAA).
- Interruptores de diagnóstico: `GH6_NODEPTH=1` (sin profundidad) y `GH6_NOCULL=1` (sin culling).

## Resultado

- Sin regresiones en el arranque, el menú principal (PLAY / BEGIN QUEST / QUICKPLAY+ / PARTY PLAY), el lobby y el Mic Check (run123, `run/montage4.png`).
- Ya no aparece ningún formato de textura sin soporte en el log. La escena 3D del menú (lava) se ve con más detalle.

## Pendiente

- **Personajes del lobby**: siguen sin verse. En run120 se vio el render del retrato (superficie de 224×415 resuelta en 8888 a 0x1A550000, más copias a media resolución
  en 32_FLOAT, 16_16 y 8), pero la superficie EDRAM correspondiente no se ha podido volcar porque el flujo automático de menús cambia entre ejecuciones.
  Siguiente paso: `GH6_DRAWLOG=<frame>` en un frame del lobby para ver qué draws van a esa superficie, o navegar con el teclado hasta la selección de personaje.
- Las superficies EDRAM son siempre BGRA8: los render targets float (16_16, 32_FLOAT) pierden precisión. Falta crear la superficie con el formato de `RB_COLOR_INFO` (bits 16-19).
- MSAA (`RB_SURFACE_INFO` bits 16-17 = 2 en algunos resolves) no se emula.
- Puntos (point sprites, tipo 1) siguen omitidos.

# Sesión 18 — Parpadeo arreglado

Fecha: 2026-10-27. Sin commits.

## Síntoma

En uno de cada dos frames la interfaz salía con recuadros blancos donde van las texturas (iconos, retratos, paneles) y con el texto corrupto.

## Causa

`runtime/gpu_d3d12.cpp`: cada frame tenía 8192 descriptores SRV y cada draw reserva 16, así que **solo cabían 512 draws**.
A partir del 512 todos los draws usaban la textura blanca (`baseSlot = 1`). La interfaz se dibuja al final del frame, y los frames que pasaban
de 512 draws (hasta unos 590 en el lobby) la perdían.

## Cambio

- `kDescPerFrame = 16 * 8192` (8192 draws por frame); el heap mide `32 + kFrames * kDescPerFrame`. Los desbordamientos se cuentan en `g_skipped[5]`.
- `tools/montage.py`: junta varios `fb_N.bmp` en una cuadrícula PNG (`python tools\montage.py salida.png 4 fb_1900.bmp ...`).
  Con `GH6_DUMPFRAME=1900,1901,...` sirve para comprobar el parpadeo frame a frame.

## Verificación

8 frames consecutivos del lobby (run117) y 8 del vídeo de arranque (run118) son coherentes, sin recuadros blancos alternos.

## Pendiente (sin cambios respecto a la sesión 17)

Depth buffer y personajes 3D; navegar con teclado hasta una canción; audio WASAPI.

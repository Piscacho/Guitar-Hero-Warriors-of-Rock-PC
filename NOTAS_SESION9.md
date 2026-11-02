# Sesión 9 — EDRAM y resolves reales

Fecha: 2026-10-26. Sin commits.

## Resultado

Cadena completa como en la consola: **draws → superficie EDRAM → resolve a memoria física (tiled) → frontbuffer (0x17DF0000) → VdSwap → pantalla**.
La ventana presenta ya el **frontbuffer de memoria**, compuesto por el propio juego. Captura: `run/fb_150.png` (colores correctos).
Unos 2 resolves por frame en el vídeo de arranque, en 4 superficies de EDRAM.

## Implementación (`runtime/gpu_d3d12.cpp`, aplicada con `tools/patch_s9_edram.py`)

- **Superficies EDRAM**: un render target D3D12 (BGRA8, alto 1024) por clave (base en tiles `RB_COLOR_INFO & 0xFFF`, pitch `RB_SURFACE_INFO & 0x3FFF`).
  Se crean al vuelo (máximo 60) y se limpian a negro al crearse. Ya no se limpia nada al empezar cada frame (el juego limpia con sus resolves).
  La conversión a NDC usa el pitch de la superficie y el alto de 1024.
- **Resolve** (draw con `RB_MODECONTROL & 7 == 6`):
  - Rectángulo: los 3 vértices float2 del stream vf0 del propio draw (BE).
  - Si el origen es color (`RB_COPY_CONTROL & 7 < 4`), el comando no es *null* y el destino es k_8_8_8_8:
    copia de la región a un buffer de *readback*, **espera a la GPU** (`FlushAndWait`, que cierra y vuelve a abrir la lista del frame)
    y escritura en `RB_COPY_DEST_BASE` en formato 2D *tiled* (pitch alineado a 32), con `copy_dest_swap` (R↔B) y endian (`RB_COPY_DEST_INFO`).
  - Clear de color si `color_clear_enable` (valor de `RB_COLOR_CLEAR`, 0x231E, interpretado como ARGB).
  - Los resolves de **profundidad** (src 4 → 17923000/17CBB000/17DAB000) se omiten: aún no hay buffer de profundidad.
- Ventana: el frontbuffer k_8_8_8_8 con swap se lee como bytes BGRA tal cual.

## Diagnóstico

- `GH6_DUMPFRAME=n` guarda ahora `fb_n.bmp` (frontbuffer real) además de `frame_n.bmp` (superficie EDRAM de 1280).
  `python -I tools/bmp2png.py fb_n.bmp fb_n.png` lo convierte a PNG a la mitad de tamaño.
- `GH6_SHOWEDRAM=1`: presenta el RT de diagnóstico en lugar del frontbuffer.
- Log `[edram]`: superficies nuevas y los 12 primeros resolves (origen, rectángulo, destino, formato, endian, swap, clear).

## Limitaciones y pendientes

- La espera a la GPU en cada resolve es lenta (unos 2 por frame ahora). Más adelante conviene hacer la escritura *tiled* en un compute shader
  sobre un buffer que refleje la memoria física (como la *shared memory* de Xenia) y evitar el viaje de vuelta a la CPU.
- Sin profundidad/stencil, MSAA, formatos de EDRAM distintos de 8888, ni solapamiento entre superficies (en la EDRAM real se pisan).

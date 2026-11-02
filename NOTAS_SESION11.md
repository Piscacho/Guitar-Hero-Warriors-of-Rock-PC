# Sesión 11 — Hardware XMA emulado y decodificación con FFmpeg

Fecha: 2026-10-27. Sin commits.

## Resultado

- **El juego usa el XMA y nuestra decodificación funciona**: contextos 0, 1 y 2 (estéreo, 48 kHz, 2 buffers de 16 paquetes) se decodifican con el `xma2`
  oficial de **FFmpeg 8.1** sin errores (unas 90.000 muestras). Con esto el juego avanza un poco más: abre `streams\sixchannelfiles.fsb/.dat` (51 archivos).
- **Sigue bloqueado** en la pantalla de título: tras unos 70 *kicks* el juego deja de consumir la salida XMA (ver «Pendiente»).

## FFmpeg

- Compilación oficial LGPL de BtbN (`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`, 77 MB), descargada con permiso del usuario en `toolchain/ffmpeg`.
- Se enlaza `avcodec.lib`/`avutil.lib`; el CMake copia `avcodec-62.dll`, `avutil-60.dll` y `swresample-6.dll` junto a `GH6.exe` en cada compilación.
- Xenia usa un códec propio de su *fork* (`XMAFRAMES`, frame a frame). Aquí se usa `AV_CODEC_ID_XMA2` con paquetes completos de 2048 bytes
  y un `XMA2WAVEFORMATEX` mínimo de 34 bytes (**`wNumStreams` = 1 en los bytes 0-1, LE**; máscara de canales en 2-5).

## Emulación (`runtime/xma.cpp`)

- **Array de 320 contextos** (64 bytes, BE) en memoria física. Su dirección física va en el registro MMIO **0x600 `ContextArrayAddress` = 0x7FEA1800**, en
  little-endian porque el juego lo lee con `lwbrx`. `XMACreateContext`/`XMAReleaseContext` reservan y liberan contextos del array.
- **Registros de control** (memoria plana: no se pueden interceptar las escrituras, así que se **sondean** cada ~1 ms con `InterlockedExchange`, que lee y limpia a la vez):
  kick 0x650-0x659 (activar), lock 0x690-0x699 (desactivar), clear 0x6A0-0x6A9 (reiniciar). Cada bit corresponde a un contexto.
- **Una pasada por *kick*** (como Xenia): solo se decodifica mientras haya sitio en el buffer de salida circular (`output_buffer_block_count` × 256 bytes)
  en **subframes de 128 muestras por canal**. PCM de 16 bits BE intercalado. Se actualiza `output_buffer_write_offset`.
  **`output_buffer_valid` = 0 cuando la salida se llena** (escritura alcanza a lectura) o cuando no queda entrada.
- Entrada: paquete actual = `input_buffer_read_offset / 16384` (bits). Al acabar un buffer se limpia su bit de validez, se cambia `current_buffer` y se pone el offset de lectura en 32.
  Los bits del contexto se modifican con CAS sobre el dword BE (el juego puede tocar otros bits a la vez).
- Diagnóstico: `GH6_XMALOG=1` (los primeros 40 *kicks* con el contexto). Cada 5 s: `[xma] kicks, paquetes, muestras, errores` y el estado de los contextos 0-3.

## Pendiente (punto de partida de la sesión 12)

Estado final típico de los contextos 0-2: salida de 16 bloques con escritura = lectura = 2 y válida = 0 (llena), cientos de muestras pendientes, entrada sin agotar.
El juego leyó solo 2 bloques y dejó de consumir. Su cálculo de datos disponibles (`sub_82769300`: estructura de voz de 96 bytes, +64 = puntero al contexto,
+82 = bytes consumidos dentro del bloque) lee bien nuestro contexto. Hipótesis:
1. El consumidor espera un estado concreto (p. ej. que el *read offset* lo escriba el hardware, o `packet_metadata`/`subframe_*`).
2. El ritmo: decodificar mucho de golpe frente a lo que espera FMOD.
3. Los registros «Unknown» 0x610/0x620/0x660 podrían ser estado o finalización que se sondea.

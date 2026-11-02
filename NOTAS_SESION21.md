# Sesión 21 — Audio real (WASAPI) y escenario 3D completo

Fecha: 2026-10-28. Sin commits. Avance estimado: **~70 %**.

## Audio (`runtime/audio_wasapi.cpp`, `runtime/xam.cpp`)

- `XAudioSubmitRenderDriverFrame` → `AudioSubmitXenonFrame`: frames de 256 muestras × 6 canales planares (FL FR FC LFE BL BR, float BE)
  mezclados a estéreo (centro y traseros × 0,707) en un búfer circular.
- Hilo WASAPI compartido, estéreo float 48 kHz con `AUTOCONVERTPCM`, por eventos, búfer de 40 ms.
- El bombeo del callback de XAudio va **marcado por la tarjeta**: genera mientras la cola tenga menos de 2400 frames (~50 ms);
  si el juego no envía frame, espera 5 ms. Resultado: cola estable, sin huecos en menús (solo durante la carga de la canción).
- `GH6_NOAUDIO=1` desactiva la salida (vuelve al ritmo por reloj). Log `[wasapi] pico ...` cada ~5 s.
- CMake: `audio_wasapi.cpp` + `ole32`.

## GPU (`runtime/gpu_d3d12.cpp`, `gpu_shader.cpp/.h`)

Causas del fondo blanco y del 3D invisible, por orden:
1. **MSAA de la EDRAM**: el juego borra la profundidad con un rect list en una superficie de pitch 640 con 4x MSAA, que en la EDRAM ocupa
   las mismas celdas que la escena de 1280 sin MSAA. Ahora las superficies (color y profundidad) se crean en **muestras**
   (`MsaaScale`: 4x = 2×2, 2x = 1×2), el viewport se escala y los resolves/clears leen una muestra por píxel. Alto de superficie 2048.
2. **Superficies con su formato real** (`RB_COLOR_INFO` bits 16-19, `EdramFormat`): 16_16 y 16_16_16_16 → SNORM con la salida del PS
   escalada 1/32 (`XVte.w`), float, 32_FLOAT, 2_10_10_10, etc. El PSO usa el formato de la superficie. Resolves de esas superficies
   con copia directa de bits; 8888 → k_8 / 1_5_5_5 / 5_6_5 / 4_4_4_4.
   Las texturas k_16_16 / 16_16_16_16 con signo o `exp_adjust` se convierten a half.
3. **Texturas 3D** (la tabla de corrección de color final, `tfetch ... tf7` con dimensión 3D): `Texture3D XTex3[16]` en t17,
   32 descriptores por draw, `BindTexture3D` con dimensiones 11/11/10 bits y direccionamiento `Tiled3DOffset` (de `texture_address.h` de Xenia).
   Sin ella el resultado final salía blanco (textura de reserva).
- vfetch: `exp_adjust` (bits 24-29) y formatos 2_10_10_10 / 10_11_11 / 11_11_10. `VGT_INDX_OFFSET` se suma a los índices (el juego no lo usa).
- Diagnóstico nuevo: `GH6_DRAWLOG=<frame>` o `t<segundos>`, con `[drawres]` (dibujado u omitido y por qué) y `[drawst]` (DC, máscara, VTE, culling, tf1-3);
  `GH6_DUMPDRAWS=a,b,c` (vuelca la EDRAM antes de esos draws), `GH6_DUMPPITCH=1280`, `GH6_PEEKTEX=<dir>,<dir>` (vuelca texturas 1280x720 junto a cada captura),
  `GH6_RT8888`, `GH6_NOMSAA`, log de resolves omitidos.
- `scripts/song_shots.txt`: guion hasta la canción con una captura por segundo de 118 a 149 s.

## Resultado

- Introducción de la canción: local iluminado con la banda, amplificadores, carteles y público (`run/lobby3d.png`, fila inferior).
- Partida con el escenario de fondo (oscuro y con desenfoque de profundidad) en vez del blanco (`run/lut2.png`, `run/lut.png`).

## Pendiente

- Paneles de personajes del lobby: ahora negros en vez de niebla. Siguen sin verse los personajes.
- Durante la partida el fondo se ve muy oscuro: revisar gamma (8888_GAMMA tratado como lineal) y el 2_10_10_10_FLOAT aproximado.
- Rendimiento: unos 20 swaps/s con MSAA y resolves por CPU.
- Bucles en shaders (control de flujo) sin soporte. Puntos (tipo 1) omitidos: casi todos son consultas de visibilidad sin efecto.
- Acertar notas: el guion no toca al ritmo; hay que probar con teclado.

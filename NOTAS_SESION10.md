# Sesión 10 — Audio (callback XAudio), mando por teclado y últimas tablas de saltos

Fecha: 2026-10-26/27. Sin commits.

## Resultado

- **El bucle principal del juego corre** (`sub_823F2A88` llama cada frame a sus subsistemas) y se presenta el frame del juego,
  pero la imagen **no cambia**: logo sobre lava (`run/fb_900.png`, igual en los frames 150, 900, 1200 y 1500). Tras `streams\streamall.fsb/.dat` no se cargan más archivos.
- El **motor de audio funciona** a nivel de driver: el juego envía un frame de audio por cada petición (7.500 en 40 s).
- **Las 480 tablas de saltos están resueltas** (0 sin resolver, 0 errores al generar el C++).

## Cambios

1. **Callback de XAudio** (`runtime/xam.cpp`): hilo que llama al callback registrado cada 256 muestras a 48 kHz (unos 5,33 ms, con QPC y `timeBeginPeriod(1)`).
   **Clave**: como en Xenia, el callback recibe un **puntero** a una palabra de invitado que contiene el argumento (`lwz r3,0(r3)` al entrar).
   Con el argumento directo esperaba sobre basura (0x0 / 0x3DF00000) y no enviaba nada.
   `XAudioSubmitRenderDriverFrame` cuenta y **descarta** las muestras (la salida por WASAPI queda pendiente).
   También `XAudioUnregisterRenderDriverClient`, `XAudioOverrideSpeakerConfig`, `XMACreateContext` (reserva el contexto, **sin decodificar**) y `XMAReleaseContext`.
2. **Mando por teclado** (`XamInputGetState`, con la ventana del juego en primer plano): flechas = cruceta, Intro = START, Retroceso = BACK,
   Espacio = A, Escape = B, X/Y, Q/E = gatillos. `packet_number` sube con cada cambio.
   `GH6_AUTOSTART=1` pulsa START y A solos cada pocos segundos (pruebas). El juego consulta los usuarios 0-3 unas 40 veces por segundo cada uno.
3. **Tablas de saltos, plan B** (`tools/jt_finder.py`): para tablas ABS en línea justo detrás del `bctr` sin comparación de límites cercana,
   se cuentan las entradas que apuntan dentro de la misma función. Resuelve las 6 que quedaban (una de ellas, 0x82322554, hacía caer el juego al activarse el audio).
4. Diagnóstico: `GH6_WAITLOG=1` vuelca puntero, tipo y estado de los objetos en `KeWaitForMultipleObjects` (las 3 primeras veces por `lr`).

## Hipótesis del bloqueo actual (por orden)

1. **XMA**: `streamall.fsb` es un banco de FMOD; en 360 FMOD decodifica XMA con el hardware. Los contextos XMA existen, pero nadie los decodifica:
   el streaming de audio no avanza y la pantalla de título/vídeo espera. → Implementar el decodificador XMA (Xenia `apu/xma_decoder` + `xma_context`, que usa ffmpeg xma2).
2. Los ~48 draws de puntos por frame (VS `6F396A6C4CEA6FEF`, 1 vértice, sin streams) se omiten; si fueran la UI («PRESS START»), habría que hacer *point sprites*.
3. Notificaciones del sistema: `XNotifyGetNext` siempre devuelve «nada»; algunos juegos esperan la notificación de sesión iniciada o de almacenamiento.

## Próximos pasos

1. Decodificador XMA (prioritario para salir de esta pantalla) y salida de audio real por WASAPI con las muestras de `XAudioSubmitRenderDriverFrame`.
2. *Point sprites* y profundidad.
3. Guitarra: `XamInputGetCapabilities` con subtipo 6 y botones de traste mapeados (sesión de entrada).

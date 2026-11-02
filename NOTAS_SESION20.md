# Sesión 20 — Primera canción en marcha

Fecha: 2026-10-27. Sin commits.

## Resultado

Con `GH6_INPUTSCRIPT=..\scripts\quickplay3.txt` el juego va solo del arranque a una canción:
título → aviso de autoguardado → PLAY → QUICKPLAY+ → Select Rocker (Johnny Napalm) → lobby (guitarra, Standard, dificultad Medium)
→ lista de canciones (GH: Warriors of Rock, 130 canciones) → «Again» (Flyleaf) → Play Song → tutorial de Quickplay+ → carga → **partida**.
Se ven el mástil, las notas bajando, los trastes, el medidor de rock y el marcador (`run/shot_134.png`, `run/qp3.png`). La canción empieza hacia los 125 s.

## Cambios

- `runtime/xam.cpp`:
  - **Guitarra**: `XamInputGetCapabilities` devuelve el subtipo 6 (guitarra; `GH6_PAD=1` vuelve al mando normal). Con el mando normal el lobby elegía voz y pedía micrófono.
  - Teclado: 1-5 = trastes verde/rojo/amarillo/azul/naranja (A, B, Y, X, LB); flechas arriba/abajo = púa; Enter = START; Retroceso = BACK.
  - **Guion de entrada** `GH6_INPUTSCRIPT=<archivo>`: líneas `<segundos> <BOTON>[+BOTON] [ms]` o `<segundos> DUMP`.
    Botones: UP DOWN LEFT RIGHT START BACK A B X Y LB RB GREEN RED YELLOW BLUE ORANGE. Tiempos desde el arranque del proceso.
  - `CurrentButtons()` une teclado, guion y `GH6_AUTOSTART` para `XamInputGetState` y `XamInputGetKeystrokeEx` (que ahora incluye LB/RB).
- `runtime/gpu_window.cpp`: `g_dumpRequest` → la orden `DUMP` guarda `shot_<segundos>.bmp`.
- `scripts/explore1.txt`, `quickplay1-3.txt` (el 3 es el que funciona).

## Notas de navegación

- El menú principal no acepta entrada hasta unos 60 s; un verde antes se pierde y la flecha abajo cae en CREATE.
- En Select Rocker el cursor empieza en CREATE ROCKER: hay que subir una posición.

## Problemas vistos (siguientes sesiones)

- **Fondo blanco durante la canción**: no se ve el escenario ni la banda (3D). Es el mismo problema que el de los personajes del lobby.
- **Editor de personajes** (CREATE → Create Rocker): el modelo no aparece y el fondo alterna entre gris y negro cada frame. Es un buen banco de pruebas para el 3D.
- Sin audio (falta WASAPI). Aún no se ha probado a tocar notas con los trastes y la púa del guion.

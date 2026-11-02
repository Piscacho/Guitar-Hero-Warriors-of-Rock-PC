# Sesión 14 — Reloj `mftb` a 50 MHz, notificaciones del sistema e interfaz fuera de pantalla

Fecha: 2026-10-27. Sin commits.

## Resultado

- **Arreglo general de tiempos**: XenonRecomp traducía `mftb` como `__rdtsc()` (GHz), pero el juego compara con `KeQueryPerformanceFrequency` (50 MHz).
  Todas las esperas y *timeouts* medidos con el *time base* duraban 60–80 veces menos. Ahora `mftb` → `ppc_mftb()` = QPC escalado a 50 MHz
  (`runtime/atomics.cpp`, sin división de 128 bits porque `__udivti3` no existe con las librerías de MSVC). Efecto: la carga del menú es unas 4 veces más rápida (la interfaz aparece en el frame ~390 en vez de ~1440).
- **Notificaciones del sistema** (`runtime/xam.cpp`): *listeners* con cola, filtrado por máscara y evento.
  Al crear cada *listener* de sistema (máscara bit 0) se encolan las de arranque como en Xenia: XN_SYS_UI (1, 0), SIGNINCHANGED ×2, INPUTDEVICESCHANGED ×2,
  INPUTDEVICECONFIGCHANGED ×2. Xenia solo las manda al primero; aquí se envían a todos porque el juego consulta otros (`GH6_NOTIFY_FIRSTONLY=1` vuelve al comportamiento de Xenia).
  `XNotifyGetNext` respeta `match_id`. El juego las recibe.
- En pantalla sigue el logo sobre el vídeo.

## Diagnóstico de la interfaz

- Los quads de texto (VS `56A07B413E52351A`, sombra negra + texto en color, DXT5 en 0x0776F000) están en **x ≈ 2820–2875 píxeles** (fuera de una pantalla de 1280), y = 560–582,
  **sin moverse nunca**: una animación de entrada (o un elemento oculto a propósito) que no avanza. Con `GH6_NOTEXTURES=1` tampoco se ven: no es la textura.
- `GH6_UIDEBUG=1` registra cada 3 s la posición del primer vértice de los draws de interfaz de 40 vértices o más (y el primer draw con todos sus vértices y colores).

## XMA

- Todos los contextos (0-5) acaban en el mismo estado: el consumidor leyó **exactamente 2 bloques de cada uno** y paró; `output_buffer_valid` = 0 (lleno).
- `sub_82769088` (sincronización de voces de FMOD): si `CurrentContextIndex ^ 0x200` coincide con el ID de una voz (+80 de la estructura de 96 bytes),
  devuelve 0 y se reintenta; si no, **copia el contexto del hardware** (48 bytes) a la voz y ajusta su copia según los bits de validez. El ID de la voz lleva el bit 0x200.
  Su llamador (`sub_82769760`) mide un *timeout* con `mftb` y, si se agota, escribe en el registro 0x601 (0x7FEA1804) y reinicializa los contextos.
- El registro 0x601 **alterna entre 2 y 3** (registro de las primeras 20 escrituras). Queda por confirmar si es el reinicio o un protocolo normal (bloqueo/desbloqueo).

## Pendiente (sesión 15)

1. Averiguar qué espera el consumidor de FMOD tras leer 2 bloques (seguir el estado de la voz +82 = bytes consumidos, y quién actualiza la lectura en el dword 9).
2. Ver si la animación de la interfaz depende del audio (música del título) o de otro estado del *script* QB.
3. Salida de audio por WASAPI (cuando haya audio que reproducir).

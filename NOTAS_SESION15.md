# Sesión 15 — Protocolo XMA de FMOD (sync/commit) y kicks síncronos

Fecha: 2026-10-27. Sin commits.

## Protocolo de FMOD/XAudio2 con el XMA (reconstruido)

Conjunto de voces (puntero `set`): `+0` número de voces, `+4` indicadores, `+8` puntero a las entradas (96 bytes por voz).
Entrada de voz: `+0..+0x30` **copia** del contexto XMA, `+64` puntero al contexto del hardware, `+80` ID del contexto, `+82` bytes consumidos.

Cada pasada del códec de lectura (`sub_8233A9D0`):
1. **Sync** `sub_82769760` → `sub_82769088`: si `CurrentContextIndex ^ 0x200` == ID de alguna voz (hardware procesando ese contexto), reintenta;
   si no, **copia el contexto del hardware a la copia de cada voz** y pone el indicador 0x20000 («sincronizado»). Si se agota el *timeout* (medido con `mftb`),
   escribe en el registro 0x601 y reinicializa (`sub_82769540`).
2. Pregunta si cada voz necesita entrada (`sub_82768F30`); si es así, lee del *stream* (`sub_82312820`) y entrega paquetes (`sub_82769680`).
3. Consume muestras (`sub_82769390`, escribe +82 y el offset de lectura en la copia). Disponibles: `sub_82769300`.
4. **Commit** `sub_827691D8`: **escribe la copia de vuelta en el contexto del hardware**, hace *kick* de cada voz con `stwbrx` de un solo bit en
   el registro kick (0x7FEA1940 + 4 × (id >> 5)) y borra los indicadores 0x30000.

## Cambios

- `runtime/hooks.cpp`: **gancho sobre el commit `sub_827691D8`** → tras la función original llama a `XmaKickContext(id)` para cada voz.
  Motivos: (a) los *kicks* MMIO de un solo bit se pisan entre voces del mismo grupo antes de que el sondeo los vea; (b) el hardware real
  responde al instante, y con 1 ms de retraso el siguiente sync copiaba el estado antiguo y el commit lo escribía encima.
- `runtime/xma.cpp`: `XmaKickContext` **decodifica de forma síncrona en el hilo del juego** (con `CurrentContextIndex = id | 0x200` mientras procesa y 0x1FF después).
  El sondeo de registros sigue para los *kicks* MMIO directos. El procesamiento implícito queda desactivado (`GH6_XMA_IMPLICIT=1` lo activa) porque rompe el protocolo.
- Diagnóstico: `GH6_XMAVOICELOG=1` (estado de cada voz desde `sub_82769300`), `GH6_FMODLOG=1` (contadores del códec: lecturas, entrada, consumo, lecturas de archivo),
  `GH6_EVENTLOG=1` (creación de *handles* con su tipo, `NtCreateEvent`/`NtSetEvent` con `lr`).

## Estado

- Las voces de FMOD ya ven **1024 muestras disponibles** con copias coherentes, pero el códec de lectura `sub_8233A9D0` **solo se llama una vez**:
  el mezclador o *stream* de FMOD no vuelve a pedir datos. Los hilos de FMOD (entrada 0x82314A80) esperan en semáforos (colas de trabajo) y uno hace `Sleep` en bucle (`sub_8230FD48`).
- Hipótesis: FMOD reproduce a través del XAudio2 del propio juego (hilo 0x8272AF78 + nuestro driver). Hay que comprobar si las *source voices* de FMOD
  consumen sus buffers (OnBufferEnd/GetState) y por qué FMOD no vuelve a mezclar. Siguiente paso: localizar el hilo de salida de FMOD (`sub_8230FD48`) y qué condición evalúa en su bucle.
- La interfaz del título sigue fuera de pantalla (x ≈ 2821) sin moverse; probablemente depende de esta cadena de audio (música del título) o de otro estado de los *scripts*.

## Nota de entorno

En esta parte de la sesión la herramienta Bash dejó de funcionar (no encuentra `bash.exe` de Git). Se usa PowerShell:
compilar con `cmd /c tools\build_game.bat`, ejecutar con `Start-Process ... -RedirectStandardError logs\runNN.log` y parar con `Stop-Process`.

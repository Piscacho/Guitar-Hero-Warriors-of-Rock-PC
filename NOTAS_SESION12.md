# Sesión 12 — XMA: protocolo, CurrentContextIndex y avance hasta el bucle principal

Fecha: 2026-10-27. Sin commits.

## Resultado

El juego **pasa de la pantalla fija al bucle principal con lógica de juego**. Carga `zones\global_sfx.pak`, lee ajustes del perfil y crea *listeners*
de notificaciones y el chat de voz. Ahora **cae** en el deserializador de datos/scripts `sub_8239A0C0` al leer la dirección −1 (ver «Pendiente»).

## XMA (`runtime/xma.cpp`)

1. **Una pasada por *kick*** (como Xenia), en **subframes de 128 muestras por canal**; `output_buffer_valid = 0` al llenarse la salida o al quedarse sin entrada.
2. **`CurrentContextIndex` (0x7FEA1818) rota** de 0 a 319 en cada pasada del hilo, nunca con el bit 0x200. El juego (`sub_82769088`/`sub_82769760`) lo compara con
   el contexto de cada voz para no chocar con el que procesa el hardware; si siempre coincide, se agota la espera y **reinicia el XMA** (escribe en 0x7FEA1804).
3. **Procesamiento implícito** (desactivable con `GH6_XMA_NOIMPLICIT=1`): cada 5 ms se procesan los contextos **reservados o lanzados alguna vez**
   con entrada y salida válidas, aunque no haya *kick*. El contexto 3 lo reserva `XMACreateContext` y nunca recibe *kick*, y el consumidor
   (`sub_8233A9D0`, bucle `while (disponible(voz) == 0)`) lo esperaba para siempre. FFmpeg avisa a veces (*fill bits*, *spectral RLE*): revisar.
4. Diagnóstico: `GH6_XMALOG=1` muestra los 3 primeros *kicks* de cada contexto; el estado de los contextos 0-3 incluye si están reservados o lanzados.

## XAM nuevo (`runtime/xam.cpp`)

- `XamUserReadProfileSettings`: tamaño necesario → `ERROR_INSUFFICIENT_BUFFER` (0x7A); después cabecera + N × `X_USER_PROFILE_SETTING` (0x28)
  con `from = 0` (no establecido) y `X_USER_DATA.type` según el nibble alto del ID. Pide ajustes int32 estándar (0x1004000C, 0x10040002...).
- `XamUserCheckPrivilege` (permitido), `XMsgCancelIORequest`.
- `XamNotifyCreateListener` → *handle* real (antes 0 = fallo, y una fábrica dejaba un objeto nulo en +0x1460 → llamada a 0). `XNotifyGetNext` = sin notificaciones.
- `XamVoiceCreate` → sin chat de voz (puntero 0 + `ERROR_ACCESS_DENIED`), como Xenia.
- FS: abrir la raíz de un dispositivo del PC (`\Device\Harddisk0\partition0`) funciona como directorio (el juego comprueba así si hay disco duro).

## Diagnóstico nuevo

- `[crash] pila:` con la pila host completa (desenrollado x64). Para traducirla:
  `llvm-symbolizer --obj=project/build/GH6.exe --functions=short 0x14...` → funciones `__imp__sub_XXXXXXXX`.
- Gancho de ejemplo en `runtime/hooks.cpp` sobre `sub_8239A0C0` (`GH6_PARSELOG=1`: argumentos y bytes de los datos).

## Pendiente (inicio de la sesión 13)

Fallo en `sub_8239A0C0` (deserializador con `switch` de 28 tipos sobre palabras de 4 bytes): r4 = buffer de *bytecode* QB (0xB4842028, cabecera 0x1BF/0x1B7) y un puntero −1.
Pila: `sub_823F2A88` (bucle principal) → `sub_825B4F60` → `sub_82151FE0` → `sub_823A4FC0` → `sub_822B0648` → `sub_822A83B0` → `sub_8239A0C0`.
Ideas: un recurso o símbolo no encontrado devuelve −1 (¿algo que depende del audio XMA, de FMOD o de las notificaciones?), datos de perfil o partida inexistentes,
o memoria pisada por el XMA implícito (comparar con `GH6_XMA_NOIMPLICIT=1`).

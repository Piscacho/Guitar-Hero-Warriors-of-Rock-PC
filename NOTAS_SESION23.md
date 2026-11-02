# Sesión 23 — XInput, DLC, lanzador y distribución

Fecha: 2026-10-28/29. Sin commits. Avance estimado: **~90 %**.

## Mando real (XInput) — `runtime/xam.cpp`

- `PollPad()`: mando 1 por XInput (se reintenta cada 2 s si no hay; `GH6_NOXINPUT=1` lo desactiva).
  Los bits de XInput son los de la 360: se combinan con el teclado y el guion. Sticks y gatillos pasan tal cual
  (en la guitarra: trémolo = stick derecho X, inclinación = stick derecho Y). `XamInputGetCapabilities` informa el subtipo real.
- CMake: `xinput`.

## DLC — `tools/stfs_extract.py`, `runtime/launcher.cpp`, `xam.cpp`, `kernel.cpp`, `fs.cpp`

- El juego busca el DLC con **`XamContentAggregateCreateEnumerator` (ordinal 633 de xam.xex)**, que obtiene con
  `XexGetModuleHandle("xam.xex")` + `XexGetProcedureAddress`. Ahora `XexGetProcedureAddress` resuelve los ordinales importados
  (`runtime/xam_ordinals.inc`, generado de `xex\imports_named.txt`) y el 633 apunta a una función propia registrada en una
  dirección de relleno del código (`0x8242ABE4`, `RegisterExtraXamFunctions`).
- Elementos de 0x138 bytes (XCONTENT_DATA + title_id). La enumeración del juego va por mensajes de XAM (`sub_826C5BD0`):
  se sustituye por una lectura directa del enumerador.
- Cada paquete se monta como `CONT<n>:` en el orden de la enumeración (el juego lo monta por mensajes, no con `XamContentCreate`).
- Licencia completa para el tipo 2. Notificaciones de arranque `XN_SYS_STORAGEDEVICESCHANGED` y `XN_LIVE_CONTENT_INSTALLED`.
- Resultado: «Downloadable content scan», la lista pasa de **110 a 120 canciones** (February Mega Pack).
- Extracción STFS (LIVE/PIRS/CON) en Python (`tools/stfs_extract.py`) y en C++ (lanzador).

## Lanzador y distribución

- `runtime/launcher.cpp`: `gh6.ini` junto al exe (`iso=`, `datos=`, `dlc=`), diálogo para elegir el ISO si falta,
  lectura de `default.xex` desde el ISO y **descifrado en memoria** (AES con BCrypt + compresión básica): ya no hace falta `basefile.bin`.
  Importación automática de los paquetes STFS de la carpeta `dlc` (una vez por paquete).
- `main.cpp`: modo desarrollo (`GH6.exe basefile.bin default.xex`) o modo usuario (sin argumentos; log en `gh6.log`).
- Ejecutable sin consola (`/SUBSYSTEM:WINDOWS`). Pantalla completa con **F11 o Alt+Enter**.
- `tools/make_dist.bat` → `dist\GH6WoR\` con `GH6WoR.exe`, DLL de FFmpeg, `gh6.ini`, `LEEME.txt` (plantillas en `dist_files\`).
- Probado desde cero: importa el DLC y arranca.

## Robustez de la GPU

- Clave de la caché de texturas con el formato DXGI final (una `k_16_16` puede subirse como UNORM o como half).
- Si `Close` de la lista falla, la lista se descarta en vez de ejecutarla (antes se perdía el dispositivo para siempre).
- Copias de región y transiciones comprobadas (`SafeCopyRegion`, `Transition` evita estados iguales).
- Cada copia de profundidad por GPU usa su propio descriptor (antes las tres copias del frame compartían hueco y leían
  la última profundidad: estropeaba el desenfoque de profundidad).
- Diagnóstico: traza de operaciones de la lista (`CmdTrace`) que se vuelca si `Close` falla; `GH6_FLUSHEACH=1` cierra la lista
  antes de cada operación para señalar la culpable; `GH6_NOCUBE`, `GH6_CPUDEPTH`, `GH6_NODSUB`, `GH6_FBMEM`.

## Fallo intermitente de `Close` (escenario punk, «What Do I Get»)

- Solo el primer fallo es real (`E_INVALIDARG`, una vez por partida, durante la carga). Los demás (`E_FAIL`, de 30 a 9000)
  eran consecuencia: la lista queda en error aunque se reinicie. Ahora, si `Close` falla, se crea una lista nueva → como mucho
  **un frame perdido** durante la carga. Prueba de 6 minutos: sin caídas, 59 fps de media.
- La causa del primer `E_INVALIDARG` sigue sin identificar (la lista que falla tiene subidas BC5/DXT, el cubo, tres copias de
  profundidad y un draw sobre R32_FLOAT); `GH6_FLUSHEACH=1` lo señalaría, pero ralentiza tanto que no se llega a esa canción.
- Con el descriptor propio de cada copia de profundidad, los planos de la banda salen nítidos (antes, desenfoque excesivo).

## Última tanda (hasta el ~97 %)

- **Exportación de profundidad del PS** (`oDepth` → `SV_Depth`, `TranslatedShader::writesDepth`). Escribirla con z desactivado
  (lo que espera la reducción de profundidad de la profundidad de campo) deja todo más desenfocado: queda tras `GH6_PSDEPTH=1`
  hasta poder compararlo con la consola o con Xenia. Por defecto, comportamiento anterior (banda nítida).
- **Interfaces del sistema sin interfaz** que podían dejar al juego esperando: `XamShowKeyboardUI` (texto por defecto),
  `XamShowMessageBoxUI`/`Ex` (botón activo), `XamUserWriteProfileSettings`, `XamContentGetDeviceState`, `XamGetOverlappedResult`,
  `XamReadTileToTexture` (imagen transparente), `XamShowDirtyDiscErrorUI` (solo log).
- Siluetas negras del lobby: son el diseño del juego (el panel dice «Choose Rocker» hasta elegir personaje).
- El `E_INVALIDARG` puntual ya no se reproduce en cuatro pruebas seguidas; si ocurre, solo se pierde un frame.
- Prueba final en modo usuario desde cero (`dist\GH6WoR`): importa el DLC, sin caídas ni listas inválidas.

## Pruebas del usuario (sesión 23, tarde)

- **Parpadeo**: el juego hace dos swaps por frame en las canciones (el primero apunta a la escena sin postproceso, sin la autopista).
  En la consola solo se ve el frontbuffer de cada vsync; ahora `gpu_cp.cpp` presenta solo el último swap antes de cada vsync emulado
  (`g_vblankCount` en `gpu_stub.cpp`). `GH6_SWAPALL=1` vuelve al comportamiento anterior. Con 16 frames seguidos: sin alternancia.
- **Audio cortado**: prioridades de hilo reales (`KeSetBasePriorityThread`, hilos de audio y WASAPI en alta) y más margen (cola ~120 ms,
  búfer WASAPI 60 ms). Diagnóstico: `[wasapi] ... cortes en la mezcla` y `[xma] ... sin entrada`. Sin validar del todo.
- **Mando de PS4/PS5** por HID (`runtime/dualsense.cpp`, USB y Bluetooth): Cruz = verde, Círculo = rojo, Triángulo = amarillo,
  Cuadrado = azul, L1 = naranja, cruceta = púa, Options = Start, Create = Back. Sin probar con mando real.
- Texturas resueltas por GPU que se leen con otro formato se vuelcan a memoria (`WriteBackResolvedForDraw`).
- F12 = captura; `DUMP8` en guiones = 8 frames seguidos (`seq_*.bmp`); `GH6_SWAPLOG=<swap>` registra frontbuffer y resolves por swap.

## Comparación con Xenia (retratos del lobby y «nube negra»)

Xenia (Canary, carpeta de la otra sesión; `hid = "sdl"` para el DualSense, copia en `*.antes_mando_ps5.bak`) muestra los
personajes en los marcos del lobby. Un mando normal en Xenia sale como micrófono (no hay opción de guitarra).

Arreglos (`gpu_d3d12.cpp`, `gpu_shader.cpp`):
- **`RB_MODECONTROL = 5` es solo profundidad**: ya no escribe color. Pintaba negro sobre la imagen antes de copiarla
  (retratos y desenfoque de profundidad de las canciones). Resultado: la «nube negra» de las canciones desaparece.
- **`PA_SC_WINDOW_OFFSET`** aplicado a vértices (`XVport1.zw`) y al rectángulo de los resolves si `PA_SU_SC_MODE_CNTL` bit 16.
- **Resolves a texturas 3D** (`RB_COPY_DEST_INFO` bit 3 + rebanada bits 4-6) con direccionamiento tiled 3D, por CPU
  (la tabla 3D dinámica `17083000` estaba a cero). `GH6_NO3DRESOLVE=1` lo desactiva.
- Copias por GPU sin límite de antigüedad: se invalidan si cambia la memoria de destino (`MemHash`); las copias derivadas
  se rehacen por número de resolve (`gen`), no por frame (`GH6_DSUBFRAME=1` = criterio antiguo).
- Diagnóstico: `GH6_DRAWLOG=s<RB_SURFACE_INFO>` (primer frame que dibuja en esa superficie), `[drawtex]` (textura de cada hueco),
  volcado de superficies R32F y RG16 (`_r32f`, `_rg16`), valores de las tablas 3D.

Pendiente — retratos del lobby: cada retrato pasa por una AO (draws 62-73 del frame): profundidad reducida → `32_FLOAT` →
`16_16` → máscara `k_8` → multiplica el retrato. La máscara sale casi a 0: la superficie `16_16` guarda AO×1/32 (rango -32..32)
y la textura `k_16_16` sin signo ni exp_adjust la lee como ~1/64. Hay que confirmar con Xenia cómo empaqueta los RT `16_16`
y su lectura como textura. Lo mismo afecta probablemente al desenfoque restante de las canciones.

## Pendiente (requiere validación externa)

- Probar con un mando y una guitarra físicos de Xbox 360.
- Comparar con la consola o Xenia la profundidad de campo (`GH6_PSDEPTH`) y la iluminación general.
- Siluetas negras en el lobby (puede ser el diseño: huecos sin jugador).

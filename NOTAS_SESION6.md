# Sesión 6 — Bloqueo del planificador de tareas resuelto, XAM mínimo y más GPU

Fecha: 2026-10-26. Sin commits.

## Resultado

El juego ya pasa el arranque y **carga el menú**: abre 48 archivos (antes 4–7). Entre ellos `qs.pak`, `global_textures`, `cas_globalarchive`,
`menu_ad_tex`, `global_skeletons`, `songstat(_fcc)`, `zones\global.pak` y el audio en streaming `streams\streamall.fsb/.dat`.
Registra su motor de audio (callback 0x8272E8C0) y crea contextos XMA. La GPU procesa unos 35 frames/s
(frontbuffer físico 0x17DF0000, 1280x720), con 9 VS y 7 PS distintos.

## Causa del bloqueo (resuelto)

- El hilo principal esperaba (`sub_82551028`) a que `pendientes(+248) + en_curso(+252)` de la estructura de tareas 0x8286B210 fuera 0, y estaba en **−1**:
  se habían sacado más tareas de las que se metieron.
- La lista de tareas se protege con un lock recursivo propio (`sub_82550300`) cuyo dueño es `sub_822CAD08()` = **KTHREAD+0x14C (thread_id)**.
  Nuestros KTHREAD estaban a ceros → todos los hilos tenían ID 0 → un lock libre (dueño 0) "ya era suyo" → varios hilos dentro a la vez → lista corrupta.
- **Solución**: `KTHREAD+0x14C` = TID del hilo nativo (en `InitGuestThread` y en `GuestThreadProc`).

## Otras mejoras de concurrencia (por el mismo análisis)

- **`lwarx`/`stwcx.` y `ldarx`/`stdcx.` con reserva versionada** (`runtime/atomics.cpp`, `compat/ppc_indirect.h`, parche `tools/patch_atomics.py`):
  un `stwcx.` correcto incrementa la versión de su cubeta, y los `stwcx.` de otros hilos que reservaron antes fallan aunque el valor sea el mismo (evita ABA).
  Las escrituras normales (`stw`) siguen sin invalidar reservas (como antes).
- **Número de CPU por hilo** (KPCR+0x10C): reparto rotatorio de 0 a 5 y `KeSetAffinityThread` real. Si se llama con el hilo aún suspendido,
  se aplica al arrancar. El juego reparte así: streaming 2, 5; render 1; trabajadores 3, 5, 2...

## XAM mínimo (`runtime/xam.cpp`)

- `XamAlloc`/`XamFree`.
- Perfil local con sesión iniciada (usuario 0, «Player», XUID fijo).
- Mando 1 conectado y en reposo (`XamInputGetState/GetCapabilities/SetState`, teclas vacías).
- Audio: `XAudioRegisterRenderDriverClient` (registra, **aún no llama al callback**), configuración de altavoces y volumen.
- Enumeraciones vacías (`XamCreateEnumeratorHandle`, `XamEnumerate` → `NO_MORE_FILES`).
- `XMsgStartIORequest(Ex)` completados al momento vía `XOVERLAPPED`.
- Red sin conexión (`socket`/`bind` fallan, sin enlace).

## GPU (`runtime/gpu_cp.cpp`, `gpu_window.cpp`)

- **Constantes**: `SET_CONSTANT` (ALU 0x4000, fetch 0x4800, bool 0x4900, loop 0x4908, regs 0x2000), `SET_CONSTANT2`, `SET_SHADER_CONSTANTS` y `LOAD_ALU_CONSTANT` → archivo de registros.
- **Shaders**: `IM_LOAD` e `IM_LOAD_IMMEDIATE` → se guarda cada microcódigo distinto en `run/shaders/{vs,ps}_<hash>.bin` (BE). Se desactiva con `GH6_NODUMPSHADERS=1`.
- **Estadísticas de draws** por tipo de primitiva (log `[gpu] primitivas:`): en el arranque y el menú hay sobre todo tipo 1 (point list en el enum de Xenos;
  posible uso para *quads*/sprites, revisar) y 8 (rectangle list), además de 6 (triangle strip) y 13 (quad list).
- **Ventana**: muestra el **frontbuffer real** del último `XE_SWAP`, desentrelazado (fórmula 2D *tiled* de Xenos, 32 bpp) con GDI. Se desactiva con `GH6_NOFRONTBUFFER=1`.
  Se verá vacío o con basura hasta que se emulen los draws y los resolves de la EDRAM a memoria.

## Diagnóstico nuevo

- `runtime/hooks.cpp`: ganchos sobre funciones del juego (las traducidas son *weak*; definir `PPC_FUNC(sub_XXXX)` las reemplaza y se puede
  llamar a la original como `__imp__sub_XXXX`). Gancho en `sub_826469B8` (dependencias de grupos de tareas).
- `GH6_JOBLOG=1`: vuelca los contadores del planificador (cada 100 ms al principio y luego cada 2 s) y los grupos bloqueados.

## Próximos pasos

1. **Dibujo real** (lo más grande):
   a) Backend D3D12 o Vulkan con su propia ventana o swapchain (sustituye a GDI).
   b) Traductor de microcódigo Xenos → DXIL/SPIR-V, partiendo de los `.bin` capturados (portar `ShaderTranslator`/`DxbcShaderTranslator` de Xenia, BSD).
   c) Estado de draw desde registros (VGT, PA, RB, SQ) + fetch de vértices (constantes de fetch en 0x4800) + texturas (des-tiling, formatos).
   d) EDRAM + resolves → memoria; así el frontbuffer que ya mostramos tendrá la imagen.
2. **Audio**: llamar al callback de XAudio cada 256 muestras desde un hilo y decodificar XMA (`XMACreateContext` está en stub).
3. **Entrada**: mapear teclado/XInput y la guitarra (subtipo 6) en `XamInputGetState`.
4. Revisar los stubs que siguen apareciendo: `XNotifyGetNext` (notificaciones del sistema), `XCustomRegisterDynamicActions`, `NetDll_XNetGetTitleXnAddr`, `XMACreateContext`.

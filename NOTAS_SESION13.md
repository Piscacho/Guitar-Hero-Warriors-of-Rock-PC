# Sesión 13 — Fallo de XenonRecomp en los `switch`, menú cargado y diagnóstico de la interfaz

Fecha: 2026-10-27. Sin commits.

## Resultado

- **Sin fallos**. El juego carga el menú: `zones\z_riothouse.pak` o `z_punkstage.pak` (escenario al azar), `pak\frontend_anims.pak` y `pak\band_lobby.pak`.
  Corre su bucle principal con unos 21 draws, unos 38 shaders y 8 superficies EDRAM por frame, con **doble buffer** de frontbuffer (0x17E00000 / 0x181A0000).
- En pantalla sigue el logo sobre el vídeo de fondo; la interfaz (texto) no se ve (ver «Pendiente»).

## Arreglos

1. **Fallo de XenonRecomp en los `switch`** (`recompiler.cpp`, en `xenonrecomp_gh6.diff`): el índice de la tabla se tomaba de 64 bits (`switch (rN.u64)`).
   Un `rlwinm` con máscara envolvente (p. ej. `rlwinm r11,r29,0,26,23`) deja **basura en los 32 bits altos** (correcto en PPC64), el juego compara solo los 32 bajos (`cmplwi`)
   y la tabla usa los 32 bajos; con el `default` inalcanzable, Clang no comprueba el rango → salto a −1. **Ahora `switch (rN.u32)`.** Era el fallo del deserializador `sub_8239A0C0`.
2. `XamContentCreateEnumerator` (tamaño = 0x134 × elementos, enumerador vacío), `XNotifyDelayUI`, `XexGetModuleHandle` (NULL → módulo principal; otros → módulo ficticio).
3. `XamInputGetKeystrokeEx`: eventos de pulsar/soltar (`VK_PAD_START` 0x5814, `VK_PAD_A` 0x5800...) a partir del estado de los botones (teclado + `GH6_AUTOSTART`).
4. GPU: `XE_SWAP` registra los cambios de frontbuffer; `GH6_DUMPFRAME=n` vuelca **todas** las superficies EDRAM (`edram_<n>_b<base>_p<pitch>.bmp`);
   `GH6_UIDEBUG=1` registra los datos del VS de interfaz `56A07B413E52351A`.

## Estructura del frame del menú o carga (de `GH6_DRAWLOG=2500`, 122 draws)

1. Unos 48 draws de 1 punto (internos, omitidos).
2. Cadena de profundidad/posproceso: resolves de profundidad (src 4) a 17933000/17CCB000/17DBB000 y resolves de color en **formatos distintos de 8888**
   (`COPY_DEST_INFO` 0xF202, 0xC82, 0x100 → 16E70000), que **todavía no se escriben**.
3. Copia a pantalla completa (*quad list* de 4, textura = resolve 1759B000), vídeo de fondo (*strip* con 3 texturas `k_8`).
4. **Quads de interfaz** (VS `56A07B413E52351A`, PS `768B7F0CBB1A7B74`, *quad lists* de 4, 44, 88 y 120 vértices, textura DXT5 en 0x0776F000 = fuente).
5. Resolve al frontbuffer.

## Pendiente (inicio de la sesión 14)

- Los quads de interfaz **no se ven ni con texturas blancas** (`GH6_NOTEXTURES=1`): la geometría o el alfa del vértice los oculta.
  Posiciones verificadas en el primer vértice: (0,0) en píxeles, `c154.zw` = 1/640 y 1/360, VTE 0x43F, viewport 640/640/−360/360 → correcto.
  Siguiente: volcar todos los vértices y colores de un draw de 88 vértices, revisar la conversión *quad list* → triángulos y el alfa del color (¿fundido o parpadeo de «PRESS START»?).
- Resolves en formatos distintos de 8888 (16_16, float, 2_10_10_10...) y profundidad.
- Audio por WASAPI (las muestras de `XAudioSubmitRenderDriverFrame` se siguen descartando).

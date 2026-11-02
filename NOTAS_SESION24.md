# Sesión 24 — retratos del lobby (investigación) y diagnóstico

Fecha: 2026-10-30. Sin commits.

## Qué pasa con los retratos del lobby (estado)

- Los retratos se renderizan bien en sus texturas (`1A550000`/`1A4F0000`/`1A490000`/`1A430000`, 220×415) y la interfaz
  los dibuja en su sitio (draws con PS `768B7F`, quads en x≈127/396/666/936).
- Justo después el juego dibuja, con color de vértice `FFFFFFFF`, unas **puertas opacas** (textura `139CF000`, DXT5,
  decodificada fuera: es una puerta con emblema, alfa 255) encima de los cuatro retratos, luego el pergamino (`1390A000`, con
  huecos en los paneles), un humo con resta inversa (PS `00EE8B`) y **un quad a pantalla completa con el retrato 4**
  (`1A430000`, color `FF808080`). Eso es lo que se ve borroso dentro de los marcos.
- En Xenia nunca se ven las puertas (en la transición el pergamino se funde sobre los retratos). Conclusión: **es estado del juego**
  (cree que los retratos/personajes no están listos), no un fallo de dibujo. Descartado: caché de vértices, fetch constants,
  lectura por CPU de los resolves (`GH6_CPURESOLVE`), swaps por vsync (`GH6_SWAPALL`), `exp_adjust`, modo Quest vs Quickplay+,
  consultas de oclusión, archivos que fallen al abrir.
- Pendiente: averiguar qué condición abre las puertas (probablemente en los scripts QB de la interfaz o una señal asíncrona).

## Cambios

- `gpu_cp.cpp`: `EVENT_WRITE_ZPD` (consultas de oclusión, 1000 muestras como Xenia; `GH6_ZPDSAMPLES`) y `VIZ_QUERY`.
  Este juego no las usa en el lobby, pero ahora son correctas.
- `kernel.cpp`: `RtlFillMemoryUlong` y `RtlCompareMemoryUlong` reales (eran stubs vacíos). `tools/gen_import_stubs.py` regenerado.
- `gpu_d3d12.cpp`: la caché de vertex buffers del frame comprueba una huella del contenido (`GH6_VBNOCHECK=1` = antes).
- Diagnóstico nuevo:
  - `GH6_DUMPTEXDRAW=<hex>` vuelca la EDRAM antes del primer draw que lee esa textura en tf0 y del siguiente;
    `GH6_DUMPREL=a,b` vuelca a/b draws después.
  - `[drawvtx]` en el frame de `GH6_DRAWLOG`: 4 vértices crudos del primer stream y C111/C154.
  - `GH6_HLSLDUMP=1` guarda todos los shaders traducidos en `hlsl_dump/`.
  - `GH6_RAWDUMP=<hex>:<bytes>,...` guarda memoria cruda con cada captura (`raw<k>_shot_NNN.bmp.bin`).
  - `GH6_RAWRESOLVE=1`: resolves no-8888 como copia bit a bit (comparación con la conversión de la sesión 23).
- Scratchpad: `tools/dxt5.py` decodifica DXT5 tiled de un volcado crudo.

## Canciones

- Siguen muy desenfocadas / con resplandor. Con `GH6_RAWRESOLVE` cambia el aspecto (más oscuro y morado) pero tampoco queda
  bien: el problema está en la cadena de profundidad de campo/bloom, no solo en el empaquetado `16_16`.

## Comparación en vivo con Xenia (30 min)

- Xenia se puede manejar por teclado sin tocar su config: `--hid=winkey --keyboard_mode=1` (A = `;`, Start = `X`, stick = WASD).
  Guiones en el scratchpad: `xkey.ps1` (teclas + captura con DPI real 1920×1080) y `dual_run.ps1`.
  La intro de Xenia dura ~75 s (se salta con A). Config de Xenia sin cambios (hash comprobado).
- Referencia útil: pantalla de **Party Play** (menú principal → amarillo). Xenia: banda nítida con desenfoque suave en primer plano.
  Nuestro: todo emborronado. Guion nuestro: `scripts/party_view.txt`.
- **Arreglo**: Xenia replica canales en los formatos de 1-2 canales (RRRR para k_8/k_16/k_24_8/k_32_FLOAT/DXT5A…, RGGG para
  k_8_8/k_16_16/DXN…, RGBB para 10_11_11…). Nosotros dábamos 0/1 en los canales que faltan: leer `.w` de la profundidad `k_24_8`
  daba "lejos" y el desenfoque cubría todo. `HostRep()` en `gpu_d3d12.cpp` (`GH6_NOHOSTREP=1` lo desactiva).
  Resultado: los fondos de Party Play salen nítidos; los personajes siguen con demasiado brillo (bloom) y las canciones aún se ven
  más desenfocadas que el frame sin postproceso.

## Brillo excesivo / desenfoque (Party Play)

- Cadena de la escena: escena en `14000500` → resolve nítido `1759B000` (PS `20502380`) → velocidades (PS `DF75D5`, 320×180)
  → reducción reinterpretando la EDRAM como 640×360 MSAA 4× (`0A020280`, `COPY_CTRL=00100060` = media de 4 muestras; nosotros
  tomamos 1 muestra) → bloom/DOF en superficies de 320 (`9EB773` matriz de color C4-C6, `8A7C3A` caja ×C111, sumas ONE+ONE)
  → composición `C59174`: lerp(nítido tf4, desenfocado tf3, f) + bloom tf2 → LUT 3D tf7.
  f = saturate(max(2·alfa(tf3), C9.w·(dist − C9.z)))·C10.x, dist = 1/(10 − 9,998·z).
- Comprobado: la entrada nítida (`1759B000`) y la copia de profundidad son correctas; el alfa del buffer desenfocado es ~0.
  Visualizando los términos (`GH6_PSOUT`): la profundidad y el alfa estaban bien; el **bloom (tf2) contenía la escena entera**.
- **Causa: aliasing de EDRAM profundidad→color.** Antes del 2.º resolve de la escena reducida, el juego dibuja un rectángulo de
  solo profundidad (modo 5, z = 0, stencil siempre + reemplazar) con base de profundidad = base de color (0). En la consola eso
  sobrescribe el color con D24S8 = 0 (negro) y el resolve siguiente solo lleva los brillos. Arreglo en `D3D12DrawImpl`: ese caso
  borra el color con el valor equivalente (`GH6_NODEPTHALIAS=1` lo desactiva). Resultado A/B en la canción: sin el lavado en blanco.
- Resolves MSAA con media de muestras (`copy_sample_select` 4/5/6) en 8888 (`GH6_NOSAMPLEAVG=1` lo desactiva).
- `GH6_PSOUT=<hash>:<expresión HLSL>` sustituye la salida de un pixel shader (diagnóstico).
- Pendiente: el alias solo cubre stencil «siempre + reemplazar»; con stencil sin escritura el canal R (byte de stencil) se
  conservaría. Comprobar los retratos del lobby (usan el mismo truco) — los guiones por tiempo se desincronizan a veces.
- Diagnóstico nuevo: `GH6_DRAWLOGN=n` (n frames seguidos), `[drawps]` (constantes C0-C6, C9, C10, C20, C111, C152 del PS).

## Proyecto externo

- El usuario pidió (2026-10-31) **no usar nada** del repositorio de otro port del juego en GitHub. La copia de lectura se
  borró y no se aplicó nada de él. Referencias permitidas: Xenia (`ref/xenia`) y el propio juego.

## Lobby tras el arreglo de aliasing

- Guion fiable hasta el lobby (Quickplay+): `scripts/lobby_qp.txt` (el de Quest a veces pierde una pulsación).
- El fondo del lobby ya no sale quemado. El quad a pantalla completa con la escena (`1759B000`) funciona como debe (alfa ~0).
- Sigue visible el quad a pantalla completa del retrato (personaje ampliado detrás de los paneles) y no se ven los retratos en
  los marcos. Alfa del retrato = C3.w del grano (`BD097B`: oC0.w = C3.w; mezcla 06010601) × (1 + 2·alfa del desenfoque), así que
  en la consola también sería visible si se dibujara: el juego decide dibujarlo (estado de la interfaz), no es un fallo de dibujo.
- `GH6_DUMPPS=<hash>` filtra `GH6_DUMPTEXDRAW` por pixel shader (solo draws de color, modo 4).
- Se detecta el DualSense del usuario por HID ("mando de PlayStation conectado: DualSense").

## Retratos: rastreo del estado del juego

- Tabla de CFuncs (funciones de script) en 0x8280C000-0x82830000: entradas {nombre, función, 0}. Enganches de diagnóstico de
  las 1551 en `runtime/cfunc_hooks.inc` (generado; incluido al final de `xam.cpp`), activos con `GH6_CFUNCLOG=1`: resumen cada
  10 s (`[cfunc-resumen]`: función=último valor xllamadas).
- En el lobby estable se consultan cada frame: `CheckForSignIn`=0 (usa `XamUserGetSigninState`), 
  `get_local_in_game_player_num_from_controller`=0, `NewsFeedManager`=0, `CompositeObjectExists` (0/1), `MonitorControllerStates`…
  No se llaman `IsPendingPlayer`, `AllPlayersAreReady`, `IsAssetLoaded`, `GamerPicTextureGetStatus`, `IsTextureLoaded`.
- Ninguna señal clara todavía de qué abre las puertas. Siguiente idea: comparar `XamUserGetSigninState`/perfil con Xenia
  (Xenia puede devolver «sesión en Live» = 2) y buscar el script de las puertas en `band_lobby.pak` (comprimido).

## Retratos: causa acotada (scripts del juego descomprimidos)

- Herramientas (scratchpad/tools): `xiso_get.py` (archivo del ISO), `chnk.py` (paks CHNK/zlib), `lzss.py` + `qbdis.py`
  (scripts QB: el código va comprimido con LZSS; checksums = CRC32 sin xor final, en little endian dentro del bytecode),
  `qbdict.py` (diccionario checksum→nombre con los textos del juego). Scripts globales en `data/compressed/pak/qb.pab`.
- **Quest (Carrera): las puertas son diseño.** Script 0BFFF564 (bucle por marco): si `$B604FFDC == Career` (script C668B8F0)
  pone `Player_Window_Fog_Cont_alpha = 1` (niebla con ojos de dragón sobre el retrato). En Quickplay+ no hay puertas.
- **Quickplay+:** sin el quad a pantalla completa del retrato 1 (`GH6_SKIPFSQUAD=1A430000`, diagnóstico) el lobby queda
  idéntico a Xenia (pergamino + 4 personajes). Ese quad tiene prueba de alfa (RB_COLORCONTROL=0x8700000C: mayor que,
  RB_ALPHA_REF = 1/255): en la consola el fondo del retrato debe tener alfa 0 (solo se vería la silueta). En la nuestra el
  fondo del retrato tiene alfa 0,8-1 → pendiente: averiguar de dónde sale ese alfa (borrado por aliasing con z=0,996 →
  dword FF000000 = A 0xFF, pase de grano con C3.w = 0,92, o el render del personaje).
- Implementada la **prueba de alfa** (como Xenia; `GH6_NOALPHATEST=1` la desactiva). `[drawst]` incluye CC y AREF.
- `GH6_SIGNIN=<n>` (estado de sesión del usuario 0; 2 = Live provoca «Connection lost»: dejar 1).

## Retratos: estado al cerrar la tanda

- Alfa del retrato paso a paso (volcados de 038000F0): correcto (fondo 0, personaje 255) hasta el pase de grano `BD097B`, que
  deja el fondo en 235 (C3.w = 0,92 con mezcla 06010601: outA = srcA + dstA·srcA). Mismos factores que Xenia.
- El quad a pantalla completa es el elemento **`paper_bg_fade`** (1280×720, textura propia `AF0AD04E`, imagen normal de
  `band_lobby.pak`), con su textura **sustituida por la de un viewport** (`CreateViewportTextureOverride`): en un frame de
  transición lee la escena (`1759B000`, alfa ~0 → invisible) y en el lobby estable lee el retrato del jugador 1 (`1A430000`).
- Comprobado con el mismo camino que Xenia (sin perfil: `GH6_SIGNIN=0`, guiado por teclado global con `scratchpad/rkey.ps1`):
  Xenia no muestra el quad; nosotros sí. No es estado de perfil.
- Síntoma relacionado: en «Select Rocker» Xenia muestra el personaje en 3D en la ventana de vista previa y nosotros humo.
  Siguiente paso: averiguar qué textura de viewport asigna el juego a `paper_bg_fade` y a la vista previa (registrar las CFuncs
  `CreateViewportTextureOverride` 8251C230 / `SetMaterialTextureFromViewport` 8250B000 y sus argumentos).
- Las CFuncs de viewport solo se llaman al arrancar (`CreateViewport` y `CreateViewportTextureOverride` una vez cada una, mismo
  CStruct); en el lobby no se reasigna nada. La disposición de CStruct en memoria aún no está descifrada (volcado crudo con
  `GH6_CFUNCLOG`).
- Razonamiento: los retratos de los marcos y `paper_bg_fade` usan la misma textura (`1A430000`) y la misma mezcla; en Xenia los
  marcos se ven y el quad no, así que allí `paper_bg_fade` **no** lee el retrato del jugador 1. Hipótesis principal: el
  render target del viewport asociado a `paper_bg_fade` y el hueco del retrato del jugador 1 comparten memoria en nuestra
  versión (asignador de memoria física distinto al de la consola/Xenia). Siguiente paso: registrar las direcciones que
  devuelven `MmAllocatePhysicalMemoryEx`/`XPhysicalAlloc` y comprobar solapes con 1A430000 y 1759B000.
- Asignador: `MmAllocatePhysicalMemoryEx` es una pila que nunca libera (0xA0000000+), ignora protección/rango; no puede solapar
  reservas por sí mismo. No hay alias de vistas 0xC…/0xE… (Xenia aplica +0x1000 en 0xE…): pendiente de revisar si el juego usa
  esas vistas. Xenia no trae visor de trazas de GPU (solo `trace_gpu_stream`), así que no hay volcado directo de referencia.
- Opciones para seguir: (a) leer en el C++ generado cómo el motor resuelve la textura sustituida de `paper_bg_fade`
  (CreateViewportTextureOverride 8251C230 y el dibujo de sprites); (b) parche dirigido (no dibujar sprites de interfaz a
  pantalla completa cuya textura es un viewport de retrato), descartado por ahora por frágil.

## Retratos: el motor (C++ generado)

- `CreateViewportTextureOverride` (8251C230) lee `texture`, `iD`, `TexDict` y otro parámetro y llama a `sub_824E88E0`
  (enganchado: `[vptex]`). Al arrancar se crean 6 sustituciones textura←viewport:
  `bg_viewport`→`viewport0`, `band_viewport_0`→`viewport0` (¡la misma textura!), `band_viewport_1..3`→`viewport1..3`,
  y 8E3F33BA→B8563E00 (sin nombre).
- El quad a pantalla completa es la textura `viewport0` (fondo del lobby = `bg_viewport`, escena con alfa ~0, invisible con la
  prueba de alfa). El marco del jugador 1 también lee `viewport0`. En un frame de transición `viewport0` apuntaba a la escena
  (`1759B000`); en el lobby estable apunta al retrato del jugador 1 (`1A430000`), y lo leen los dos.
- En Xenia el marco del jugador 1 muestra el retrato y el fondo no: allí la textura que lee el fondo apunta a la escena cuando se
  dibuja. Siguiente paso: averiguar quién actualiza la dirección base del objeto de textura `viewport0` (por viewport y por
  frame) y en qué orden respecto a los dibujos de la interfaz; posible carrera de orden (nuestras cargas son síncronas/rápidas)
  o una actualización que en la consola llega por un callback de la GPU.

## Retratos: hora autónoma (13:00-14:00)

- Herramientas nuevas: `GH6_FINDTEX=<base>` (+`GH6_FINDTEXSIZE`) busca cabeceras de textura en la memoria asignada (una pasada en
  la captura ≥ 90 s); `GH6_WATCH=<dir>` y `GH6_WATCHSIZE`/`GH6_WATCHBASES` vigilan cambios de dwords/cabeceras con nº de draw y
  swap; `GuestAllocTops()` expone los topes de los asignadores.
- Objetos de textura: retratos fijos en el montón virtual (p. ej. 400A3xxx, base virtual BA43…/BA49…/BA4F…/BA55…, nunca
  cambian); escena del fondo `A18587F4` (base B759B086, 1280 de pitch, nunca cambia). 40042600… = copia de D3D de los
  registros de textura de la GPU (cambia con cada draw), 40D05B24/40D25B24/40D45B24 = estado por frame (triple buffer).
- El quad del fondo usa la constante del objeto del **retrato del jugador 1** (pitch 220), no la del objeto de la escena.
- Registro de sustituciones (`sub_824E88E0`): nodo de 48 bytes en una lista global (cola en 0x82868404+16):
  +24 id, +28 texdict, +32 checksum de textura, +36 id de viewport, +40 objeto de textura (buscado por nombre),
  +44 objeto de viewport. `bg_viewport` y `band_viewport_0` comparten el **mismo objeto de textura** (`viewport0`).
- Con esta lógica, en cualquier orden el fondo y el marco del jugador 1 leerían el mismo hueco; en Xenia el marco se ve y el
  fondo no. Contradicción sin resolver: o el elemento de fondo no usa `viewport0` en la consola (otra resolución del material de
  `paper_bg_fade`), o el alfa del hueco difiere. Siguiente: localizar dónde la interfaz resuelve la textura de `paper_bg_fade`
  (búsqueda por nombre AF0AD04E y sustituciones por viewport) y registrar qué objeto devuelve.
- **Experimento clave** (`GH6_SKIPVPTEX=<id>` omite el registro de una sustitución):
  - sin `band_viewport_0` (C46678E5): fondo = pergamino (como Xenia) pero el marco del jugador 1 muestra la escena del fondo;
  - sin `bg_viewport` (071F4183): sin cambios (retrato gigante).
  Con las dos, `viewport0` se resuelve siempre a `band_viewport_0` (último nodo de la lista). En Xenia cada elemento recibe la
  suya (fondo → escena, marco → retrato): la resolución debe depender del contexto (viewport/árbol de interfaz del elemento,
  o del orden en que se procesa la lista frente a los dibujos). Siguiente: leer quién recorre la lista de 0x82868404
  (buscar lecturas de esa dirección en el C++ generado) y qué compara para elegir nodo.
- Lista de sustituciones: `sub_824E86A0` la recorre cada frame (contador en +22) y llama a `sub_8253F028` para aplicar cada
  nodo (3 = aplicada → otra lista en 0x828683E8; 2 = descartada). Enganchado (`[vptex] aplicar nodo`): solo se aplican
  `bg_viewport`→`viewport0` y 8E3F33BA→B8563E00 (estado 3); los nodos `band_viewport_0..3` **nunca pasan por ahí**, pero el
  retrato del jugador 1 acaba igualmente en `viewport0`. Siguiente: ver por qué camino se aplican los de `band_viewport_*`
  (sub_824E85A0 recorre las dos listas por id) y si su contador (+22) o su estado depende de algo que emulamos distinto.

## ARREGLO: scissor (retratos del lobby resueltos)

- El quad a pantalla completa es el elemento `bg_viewport` (tipo viewport, textura `viewport0`, gris 128) que crea el script
  AA6F2A17 dentro de una ventana (tipo A13ADBB7) colocada en **x = 2000, fuera de pantalla**: la ventana recorta a sus hijos
  con el **scissor** de la GPU. No aplicábamos scissor (siempre la superficie entera).
- `gpu_d3d12.cpp`: scissor como `GetScissor` de Xenia (PA_SC_WINDOW_SCISSOR_TL/BR + PA_SC_WINDOW_OFFSET salvo
  window_offset_disable, ∩ PA_SC_SCREEN_SCISSOR, escalado por MSAA). `GH6_NOSCISSOR=1` lo quita, `GH6_SCISSORBIG=1` solo en
  superficies ≥ 640. Resultado: lobby de Quickplay+ idéntico a Xenia (pergamino + 4 retratos); el texto del menú del jugador ya
  no se sale del marco.
- Canciones: los destellos (PS 205023) y las pruebas de 16×16 quedan recortados como en el hardware; la imagen sale más oscura
  y sin los "fantasmas" verdes enormes. Party Play: sin lavado en blanco. Sigue el exceso de desenfoque.
- `[drawst]` incluye WSC/SSC (scissor). Scripts de prueba: `scripts/menu_view.txt`, `scripts/lobby_qp.txt`.

## ARREGLO: bucles y direccionamiento relativo en shaders (canciones oscuras)

- La oscuridad de las canciones tras el scissor NO era el scissor: el shader D6FBC6 (luminancia/destello 16×16) fallaba al
  traducirse por el **CF op 7 (LOOP_START)** y sus draws se saltaban.
- `gpu_shader.cpp`: LOOP_START (op 7, loop_id = bits 16-20) / LOOP_END (op 8, break predicado bit 21, condición c.b bit 10)
  → `[loop] for` con `aL` = start, paso con signo; constantes de bucle 0x4908+ (cuenta bits 0-7, inicio 8-15, paso 16-23)
  en el cbuffer como `uint4 XLoop[8]` (offset 4320, kCbSize = 4608).
- Direccionamiento relativo ALU: temporales `r[aL+n]` (bit 0x40), constantes `C[n + a0|aL]` (w1 bits 31/30/29, regla de
  Xenia para src 1/2/3), destinos relativos (w0 bit 6 vector, bit 14 escalar).
- Resultado: canciones con escena visible, sin errores "no traducido". Lobby de Quickplay+ sin regresión (igual a Xenia).
- El scissor queda activo en todas las superficies por defecto (`GH6_SCISSORBIG=1` es opcional).

## ARREGLO: Select Rocker / Edit Rocker (personaje invisible en la ventana)

Síntoma: la ventana de previsualización mostraba solo humo; el personaje no salía (Lars Umlaut, Johnny Napalm...).

Cómo dibuja el juego esa pantalla (se alternan dos tipos de frame):
- **Frame B**: escena base (backstage del CBGB + personaje) en la superficie principal → SSAO (profundidad lineal R32F, AO 16_16,
  desenfoque, resolve a k_8 en 16E70000) → PS 87FF4B14 multiplica el color y pone alfa = AO → resolve a 1759B000.
- **Frame A**: el PS **C316F389 restaura la escena de B con MRT**: oC0 = profundidad empaquetada (al color en base 0x2D0 = alias de la
  profundidad) y **oC1 = color de 1759B000 en la superficie principal (RB_COLOR1_INFO)**. Las coordenadas son de píxel: |r0|.xy
  sale del **PsParamGen** (la GPU escribe la posición del píxel en r[param_gen_pos]) y el tfetch es **no normalizado**. Luego se
  suman las luces con sombras (4 mapas en canales de 400×400), DOF + bloom (C591749F) y la interfaz la compone con alfa.

Faltaba (todo en `gpu_shader.cpp` / `gpu_d3d12.cpp`):
1. **MRT**: oC1-oC3 → SV_Target1-3 (`ts.mrtMask`); RT1 = superficie de RB_COLOR1_INFO (0x2003), máscara RB_COLOR_MASK bits 4-7,
   blend RB_BLENDCONTROL1 (0x2209). `GetEdramRt(infoReg)`. `GH6_NOMRT=1` lo quita.
2. **PsParamGen**: SQ_PROGRAM_CNTL (0x2180) bit 18 y SQ_CONTEXT_MISC (0x2181) bits 8-15; en el prólogo del PS
   r[pos] = (±x, y, 0, 0) con x,y = píxel entero (signo de X = cara trasera), como Xenia. Va en XAlpha.z/w. `GH6_NOPARAMGEN=1`.
3. **tfetch**: tx_coord_denorm (w0 bit 25, se divide por GetDimensions), offset_x/offset_y (w2 bits 16-25, medios texeles) y
   predicado por instrucción (w1 bit 31, condición w2 bit 31). `GH6_NODENORM`, `GH6_NOFETCHPRED`.
4. **Caché de texturas**: el hash solo miraba 64 dwords de los primeros 16 KB; una k_8 de 64×64 que escribe la CPU (03C3F000)
   no se volvía a subir. Ahora las texturas ≤ 64 KB se hashean enteras y las grandes con 256 muestras repartidas
   (`GH6_TEXHASHOLD=1` = antes). Además, generación de resolves por CPU por dirección (`GH6_NORESOLVEGEN`).

Resultado: Select Rocker muestra a cada personaje enfocado con el fondo desenfocado (como en la consola). Edit Rocker también.
Las canciones salen bastante más nítidas (la restauración MRT también se usa ahí). Lobby de Quickplay+ sin regresión.

Pendiente: en Edit Rocker a veces faltan la **cabeza** y el **mástil de la guitarra** (estable entre frames seguidos, no parpadeo).

Diagnóstico nuevo: `[drawresolve]` (resolves del frame de GH6_DRAWLOG), `GH6_TEXDUMP=<base>` (+ `GH6_TEXDUMPFMT`,
`GH6_TEXDUMPMIN=<frame>|t<s>`) vuelca la textura destileada, `GH6_TEXNOCACHE=1|<fmt>` resube siempre, `GH6_DUMPPITCH`.
Scripts: `scripts/cas_lars.txt` (Select Rocker → Lars → Johnny), `cas_create.txt` / `cas_create8.txt` (Edit Rocker),
`song_view.txt` (canción, capturas a 132/140/148 s). Los scripts a veces se desincronizan (Quest o menú PLAY): repetir.

## Edit Rocker: cabeza/torso ausentes (investigación, sin arreglar)

- En la EDRAM el personaje de Edit Rocker sale con triángulos estirados hasta un punto y partes ausentes (cabeza, torso,
  mástil). No hay draws omitidos (los "otro motivo" son resolves) ni el anillo por frame se llena en esa pantalla
  (el anillo de 64 MB solo se llena en unos pocos frames de carga: `[d3d12] anillo lleno ...`).
- Causa acotada: **78 de 493 VS** capturados (`run\shaders\vs_*.bin`) están casi vacíos, p. ej. vs_EC3FD89B: 261 dwords
  con solo los dwords 27-47 (las vfetch) no nulos. Llegan por **IM_LOAD_IMMEDIATE** (el microcódigo va dentro del búfer de
  comandos: D3D parchea las vfetch según la declaración de vértices).
- No es una carrera: releyendo durante 200 ms no cambia (`GH6_IMWAIT=1`). No hay escrituras por los alias físicos
  0xC0000000/0xE0000000 (`GH6_ALIASSCAN=1`).
- `GH6_MEMSEARCH=401011d1@0,401011d1@12,4010108f@24` encuentra el **shader original en memoria física (0xB684xxxx...,
  varias copias)** y también está a ceros salvo el bloque de vfetch (sin parchear: 002033D8/00000006). Es decir, el
  microcódigo ya está vacío en la memoria del juego: algo del lado de la CPU no escribe CF/ALU (¿shader montado en
  tiempo de ejecución para CAS? ¿lectura/descompresión que no hacemos?). Las LDI* (xcompress) no se llaman.
- Siguiente paso: localizar quién escribe esos objetos (vigilar 0xB68452F0 con GH6_WATCH desde la carga de Edit Rocker,
  o buscar el objeto D3D de VS que apunta a esa dirección) y comparar con Xenia.

Otros pendientes vistos: el fondo de Select Rocker varía entre ejecuciones (backstage nítido o escenario con llamas
verdes más quemado), según qué tipo de frame se presenta.

Diagnóstico añadido: `[shader] IM_LOAD(_IMMEDIATE) casi vacio`, `GH6_IMWAIT`, `GH6_ALIASSCAN`, `GH6_MEMSEARCH`, registro de
anillo lleno. Script `scripts/cas_create8.txt` (Edit Rocker con 8 capturas seguidas).

### Edit Rocker: rastreo del escritor (sin cerrar)

- `GH6_WRITEWATCH=<dir>:<bytes>` (diag.cpp): protege esas páginas y registra cada escritura con pila host + lr de invitado,
  deduplicado por (instrucción, lr); se traduce con `python tools\symsample.py logs\runNN.log 1`. `GH6_WRITEWATCHMAX`,
  `GH6_WRITEWATCHDELAY=<s>`.
- Problema: la dirección de los objetos de shader cambia en cada ejecución (B684535C, B684F9CC, B6844620, B682DE90,
  B6919380...) y vigilar 2 MB de memoria física frena el juego hasta no llegar a Edit Rocker. La zona 0xB684-0xB690 la
  escribe sobre todo el gestor de memoria/objetos del motor (sub_822D1D38, sub_822D2170, sub_822E4538, sub_826AC5F0 = memcpy).
- Dato nuevo: los vfetch del objeto original en memoria van SIN parchear (002033D8/00000006) y D3D los parchea en la copia
  del búfer de comandos (003931D1/00000008). Así que el motor escribe él mismo los vfetch y el resto (CF/ALU) le llega a
  cero: muy probablemente el motor **compone** el shader en tiempo de ejecución copiando fragmentos de un sitio vacío.
- Idea para la próxima tanda: un hilo que sondee 0xB680-0xB6A0 buscando el patrón de vfetch y, al aparecer, vuelque la
  pila de ese hilo (GH6_SAMPLE) o proteja la página antes de que termine la composición; o buscar en el código generado
  las funciones que escriben instrucciones vfetch (constantes 0x...5000 / 0x401011D1).

## ARREGLO: espejo de memoria física 0xC0000000 (Edit Rocker sin cabeza, pelo, luces)

- Causa: D3D (`sub_822DE6A8`) mete en el búfer de comandos los shaders que tiene que parchear con IM_LOAD_IMMEDIATE y copia
  el microcódigo leyendo por el **alias físico 0xC0000000** (`addis r28,r11,-16384` sobre la dirección física). En la consola,
  0xA0000000+p, 0xC0000000+p y 0xE0000000+p+0x1000 son la misma memoria; nuestra memoria era plana, así que leía ceros y solo
  quedaban los vfetch que D3D parchea después. Se vio con `GH6_PATWATCH` (los "objetos" de 0xB69… eran paquetes PM4
  `C1062B00` dentro del búfer de comandos).
- `main.cpp` (`ReserveGuestMemory`): 4 GB reservados con placeholders (`VirtualAlloc2`); una sección de 512 MB se mapea
  (`MapViewOfFile3`) en 0xA0000000 y en 0xC0000000; el resto es memoria privada como antes. Se comprueba al arrancar
  (`[mem] ... (comprobado)`). `GH6_NOMIRROR=1` = memoria plana. El alias 0xE0000000 (+0x1000) no se puede mapear con
  granularidad de 64 KB: sigue sin espejo.
- Resultado: `IM_LOAD_IMMEDIATE: 262144 en total, 0 casi vacios` (antes 78 VS vacíos). Edit Rocker con el personaje completo
  (cabeza, tatuajes, guitarra), Axel Steel con su melena en el lobby, canciones con focos de colores, público y
  personajes nítidos. Lobby y Select Rocker sin regresión.
- Herramientas nuevas: `GH6_PATWATCH=v@off,...` (+ `GH6_PATLO/GH6_PATHI`) vuelca las pilas de todos los hilos y 4 KB
  (`run\patwatch_*.bin`) cuando aparece un patrón en memoria; `GH6_WRITEWATCH` (ver arriba); contador de IM_LOAD_IMMEDIATE.

Pendiente: el fondo de Select Rocker/Edit Rocker a veces sale muy claro (escenario con llamas verdes) y otras como el
backstage nítido; por confirmar contra la consola o Xenia.

## Audio ("no se escucha")

- Con el ejecutable actual el audio sí se genera: `GH6_WAVDUMP=<archivo>` (nuevo) graba la mezcla estéreo enviada a WASAPI.
  En run419 (canción): vídeo, música de menú, silencio en la carga y canción con RMS ~0,12 y pico ~0,9; sin huecos ni NaN.
  Fragmento para el usuario: `Recomp\audio_cancion_20s.wav`.
- El log ahora dice el dispositivo: `[wasapi] dispositivo: OUT 1-2 (BEHRINGER UMC 202HD 192k)` (el predeterminado de Windows).
- El dist anterior (10-30) era viejo; su último log (10-29) tenía la decodificación XMA parada (kicks 315 fijos). dist regenerado.
- El usuario confirma que con el dist regenerado (con el espejo 0xC0000000) el juego suena bien; antes oía la canción
  entrecortada y la guitarra no sonaba al acertar notas (dist viejo).
- Añadido: salto de paquetes XMA (`packet_skip_count`, como Xenia; `GH6_XMA_NOSKIP=1` lo quita): en este juego siempre vale 0.
  Resumen por contexto cada 5 s en el log (`[xma] por contexto (escritas/sin entrada/kicks)`): en canción, ctx 12-20
  (9 pistas estéreo) escriben ~48.000 muestras/s cada uno, sin huecos.

## ARREGLO: personajes y público "transparentes" en canciones (GH6_PSDEPTH por defecto)

- El compositor final (C591749F) hace profundidad de campo + **desenfoque de movimiento**: lee un búfer de velocidades
  (tf9 = 17050000, 320x180) y muestrea la escena en ±velocidad; tf3.a (cantidad de desenfoque) también sale de esa cadena.
- Las velocidades las escriben un rect de velocidad de cámara (PS DF757D71, desde la profundidad) y los personajes/público
  (VS 3DE1C6DA con posiciones actual/anterior vf95/vf94, PS DF75D55E y 99CD25F5), con prueba de Z LEQUAL contra la
  profundidad reducida (DEPTH base 0x5A0) que rellena el PS F7AF58A6 **exportando oDepth con Z desactivado**.
- Ese caso estaba desactivado por defecto desde la sesión 23 (GH6_PSDEPTH): la profundidad reducida quedaba vacía, ningún draw
  de velocidad pasaba, el búfer conservaba una copia reducida de la escena y el compositor la interpretaba como velocidades
  y desenfoque → personajes mezclados con el fondo, estelas, lavado verde.
- Ahora activado por defecto (`GH6_NOPSDEPTH=1` lo quita). Canciones nítidas y oscuras con focos, como el vídeo de la 360.
  Lobby y Select Rocker sin regresión (el fondo de Select Rocker sigue muy claro: pendiente).
- `GH6_PSOUT` acepta varios shaders separados por ';'. dist regenerado.

Pendientes que reporta el usuario: audio "disco rayado" en Bohemian Rhapsody (se le lanzó el dist con GH6_WAVDUMP para
grabarlo), pista de notas con rayas metálicas (falta de mipmaps: siempre SampleLevel 0 y una sola mip), medidor de
puntuación/estrella con bloques blancos.

## Arreglos de shaders tras las pruebas del usuario (pista, colas, multiplicación)

- **Modo de borde y filtro por textura** (pista con rayas metálicas): el sampler único era lineal + CLAMP; las texturas que se
  repiten (la pista que se desplaza, suelos...) estiraban el borde. Ahora XTexMode[16] en el cbuffer (offset 4448, kCbSize 4864)
  con clamp_x/y/z, filtros y color de borde de la constante de fetch; dos samplers estáticos (s0 lineal, s1 punto, ambos WRAP)
  y XAddr() en el shader (repetir, espejo, recortar al borde/medio texel, color de borde). `GH6_NOTEXMODE=1` = antes.
  La pista se ve oscura con su dibujo, como en la 360.
- **0 * x = 0** (como D3D9/Xenia) en mul, mad, dp4/dp3/dp2add, muls/mul_prev y mulsc; logc/rcpc/rsqc saturan ±inf a ±FLT_MAX y
  rcpf/rsqf los anulan. `GH6_NOMULZERO=1` = antes.
- **a0 al final de la instrucción**: maxa/maxas/maxasf calculan el nuevo a0 con las fuentes actuales y lo aplican tras escribir
  los resultados; las fuentes relativas de la misma instrucción leen el a0 anterior. Las colas de las notas largas (tira 2D,
  VS 2EF446DF: "max r4, c[128+a0]" + maxas en la misma instrucción) salían como líneas horizontales por toda la pantalla.
  `GH6_A0EARLY=1` = antes. Pendiente de confirmar por el usuario en una canción con colas.
- Diagnóstico: F9 registra los 2 frames siguientes (en gh6.log); `GH6_DRAWLOGEVERY=<desde>:<cada>` registra y captura
  periódicamente. La pista, gemas y colas se dibujan DESPUÉS del compositor final (como interfaz 2D).
- Pendiente: medidor de puntuación/estrella con bloques blancos (PS 7FE4E1F5 con k_8_8 + ruido 3D; los mapeos parecen bien),
  mipmaps (siempre mip 0), fondo de Select Rocker muy claro, desenfoque/brillo fuertes en algunos planos.

## ARREGLO: medidor con bloques blancos (mips empaquetados)

- La barra clara era un draw de la interfaz (VS 56A07B41 / PS 768B7F0C) con una textura DXT5 de 256x16 (línea fina con brillo);
  los cuadros blancos, otra DXT5 de 32x16. Con **packed_mips** (constante de fetch dword 5 bit 11) y el lado más corto <= 16,
  el nivel base está desplazado 16 texeles dentro del tile de 32x32 bloques (en Y si es más ancha que alta, si no en X), como
  GetPackedMipOffset de Xenia. Leíamos desde la esquina del tile. Ahora se aplica el desplazamiento al destilear (BindTexture 2D).
  `GH6_NOPACKEDMIPS=1` = antes. El medidor se ve como en la 360 (líneas finas con punto brillante). Afecta a todas las texturas
  pequeñas (también de escena).
- Confirmado por el usuario y en capturas: las colas de las notas largas ya salen verticales (arreglo de a0).
- Diagnóstico: `GH6_DUMPVS=<hash>` (+ `GH6_DUMPVSMIN=<s>`, `GH6_DUMPREL=...`) dispara volcados de EDRAM desde el primer draw con
  ese VS sin depender del tiempo; `GH6_TEXDUMP` admite formatos comprimidos.
- Un cierre esporádico sin mensaje (run451, hacia los 105 s) no se reprodujo después; vigilar.
- Pendiente: mipmaps (siempre mip 0: parpadeo de texturas lejanas), fondo de Select Rocker muy claro, desenfoque/brillo fuertes
  en algunos planos.

## Mipmaps (texturas 2D)

- BindTexture 2D sube todos los niveles: dword 5 bits 12-31 = mip_address, dword 4 bits 6-9 = nivel máximo. Disposición de
  GetGuestTextureLayout de Xenia: niveles 1..P seguidos desde mip_address, cada uno con el tamaño de la potencia de dos
  desplazada, alineado a 32x32 bloques (filas lineales a 256 B) y a 4 KB; con packed_mips, desde el nivel P (lado corto <= 16)
  todos en el mismo tile con GetPackedMipOffset (packedOff). El destileado es una lambda (fillLevel) común a todos los niveles.
- PS: SampleGrad con ddx/ddy de la coordenada antes del modo de borde; filtro de mip "basemap" (XTexMode.z bits 2-3 = 2) o
  instrucción con mip_filter 2 → nivel 0. Si un PS no compila con gradientes se retraduce sin ellos (g_xlatNoGrad); en la
  prueba no hizo falta ninguno. VS sigue con nivel 0. `GH6_NOMIPS=1` lo quita.
- Resultado: público y pista sin ruido a distancia; sin texturas rotas. Pendiente: mips en texturas 3D/cubo, sesgo de LOD.
- Los scripts de entrada ahora acaban siempre en una canción (la navegación cambió, probablemente por la partida guardada).

## Brillo/desenfoque excesivo (investigación, sin cerrar)

- La escena base del escenario de la fábrica sale correcta y nítida (`Recomp\escena_base_fabrica.png`, izquierda color,
  derecha alfa). El alfa de la escena es una máscara de emisión (bobinas, brillos del pelo, focos del suelo).
- Cadena de postproceso a 320x180: 8A7C3A63 (media de 4) → 11E2C0F4 (profundidad → CoC) → A38B62A0 → EE1AFD96 (desenfoque
  ponderado por alfa) → 9EB77328 (matriz de color: 0,233·rgb − 0,0686·alfa) → compositor C591749F (sharp/blurred por
  max(2·tex3.a, (z_lineal − C9.z)·C9.w), + tex2 sin escala, LUT 3D). La LUT funciona (sin ella todo sale gris).
- Sin una referencia de la consola del mismo escenario no se puede saber cuánto halo es correcto: pedida al usuario.
- `GH6_DUMPVS=EA26B7141D2133C7` vuelca la escena justo antes de la reducción (entrada del halo).

## Tirones / pantallas negras: caché de shaders y precompilación

- Medición nueva: `[tiron]` en el log cuando un frame tarda más de `GH6_HITCHMS` (40 ms por defecto), con desglose de
  compilación de shaders, PSO, texturas, espera de GPU y vaciados (FlushAndWait).
- Causa principal: cada shader nuevo se compilaba con FXC en el hilo de la GPU (~100 ms cada uno). Al entrar en una canción
  se pedían ~107 a la vez → **12 s congelado** (la "pantalla negra" larga que desincronizaba los scripts de entrada).
- Arreglo 1: caché en disco `shadercache\<hash del HLSL + destino>.cso` (escritura atómica con temporal + MoveFileEx).
  `GH6_NOSHADERCACHE=1` la desactiva. Clave = texto HLSL, así que cualquier cambio del traductor la invalida sola.
- Arreglo 2: precompilación en segundo plano al iniciar (PrewarmShaders): traduce (bajo `g_xlatMutex`, el traductor usa
  estado global) y compila en hasta 8 hilos de baja prioridad todo `shaders\*.bin` (microcódigo visto antes). En frío,
  513 shaders en 8,4 s mientras se pasan los menús; en caliente 0,1 s. `GH6_NOPREWARM=1` la quita.
- make_dist copia `run\shaders` y `run\shadercache` al dist.
- Quedan tirones de 200-600 ms al cargar escenarios sin coste de shaders (texturas 50-100 ms + vaciados 15-40 ms, el resto
  es trabajo del propio juego en la CPU). Pendiente si molestan.

## Desenfoque/estelas en canciones: velocidades a cero

- Referencia de la 360 (fábrica con neón): personajes nítidos con halo, fondo desenfocado, sin estelas.
- Diagnóstico con GH6_PSOUT en el compositor C591749F: el factor de profundidad de campo es correcto (personajes 0, fondo 1);
  la escena nítida (tf4) es correcta; el búfer de velocidades (tf9, resolve fresco en cada frame, base 1584) tiene
  siluetas de personajes con desplazamientos de 16+ píxeles aunque estén casi quietos → estelas en toda la imagen.
  No se usa memexport (ningún VS exporta a eA/eM); vf94/vf95 (posición anterior/actual, 32_32_32_FLOAT) las escribe la CPU.
  Sin sesgo de exponente en RB_COLOR_INFO ni en resolves (comprobado con logs).
- Solución práctica: ApplyShaderOverrides pone oC0 = 0 en los PS de velocidad DF757D71 (cámara), DF75D55E y 99CD25F5
  (personajes/público). F7AF58A6 ya rellena el búfer con la profundidad en los 4 canales (velocidad xy−zw = 0).
  Resultado muy parecido a la 360 (`Recomp\sin_estelas.png`). `GH6_MOTIONBLUR=1` = antes. Pendiente: causa real de las
  velocidades (posible stream anterior incorrecto o matrices c0-c3 vs c224-c227).
- GH6_PSOUT ahora se aplica también en la precompilación. `GH6_TEXAGE=<dir>` registra los resolves y lecturas de esa dirección.
- Sigue un halo verdoso algo más fuerte que la 360 en negros (pendiente de comparar con más capturas).

## Fluidez: personajes a ~15 fps (investigación)

- Referencias del usuario: vídeo de 360 (PhhVaXkJnDM, 30 fps, no sirve para medir) y uno de emulador a 4K y 60 fps
  (DPtZIXRyvMc). Midiendo diferencias entre fotogramas seguidos del de 60 fps: **la escena 3D cambia uno de cada dos
  frames** (el juego la dibuja a 30 Hz y repite la copia en el frame intermedio; la pista va a 60). Coincide con los
  frames A/B que ya conocíamos. En la 360 los personajes van, por tanto, a 30 fps.
- En el port, el juego hace 30-37 frames/s en canción → escena 3D a ~15-18 Hz: eso es lo que ve el usuario.
- Herramientas nuevas: `GH6_FRAMELOG=<desde>:<cuántos>` (ms, draws, tiempo de draws, present, espera de GPU por frame),
  `GH6_VSYNC=0` (Present sin vsync; no cambia nada: Present tarda 0,1 ms), `GH6_PROFILE=<s>:<muestras>` (+
  `GH6_PROFILEALL=1` para todos los hilos) con `python tools/profagg.py logs/runNN.log`, `GH6_VBLANKLOG=<dir>`,
  `GH6_CPURESOLVESTATS=1`, `GH6_WAITLOG=1`.
- Perfil de hilos en canción: el hilo principal (822CB0A0) gira el 90 % en sub_8242ABE8 esperando a otro hilo; el hilo de
  render (824333F8) gira en sub_825840F0 esperando que el contador 0x8298C1F0 avance (no es el vblank: sube ~1 por frame,
  lo mueve la GPU al terminar cada frame). Es decir, **el juego espera a nuestra GPU**: el hilo de comandos tarda ~20 ms por
  frame en los draws.
- Desglose del hilo de comandos: resolves por CPU ~33 % (FlushAndWait ~20 %: cada uno espera a que la GPU acabe todo lo
  anterior; conversión de píxeles ~13 %), BindTexture ~22 %.
- Resolves por CPU por frame (GH6_CPURESOLVESTATS): 640x360 8888 con MSAA 2x2 y media de muestras (reducción de la escena),
  rebanadas 32x32 de dos LUT 3D (destArray), 640x360 16_16_16_16_FLOAT (superficie fmt 14), 16_16 (fmt 4) y k_8.
  **Siguiente paso de rendimiento: hacer esos resolves en la GPU** (media MSAA con shader, formatos no 8888 como textura
  resuelta con su DXGI, LUT como textura 3D en GPU) para quitar las esperas.
- Hecho: huella de texturas memorizada por frame (se invalida con cada resolve por CPU o volcado a memoria;
  `GH6_TEXHASHEVERY=1` = antes).
- Hecho: vblank con temporizador de alta resolución y plazos absolutos (antes Sleep(16) ≈ 31 ms con la resolución por
  defecto de Windows → ~32 vblanks/s). A/B en la misma canción: 30,2 → 31,5 frames/s (no era el límite), pero ahora es
  correcto. `GH6_OLDVBLANK=1` = antes, `GH6_VBLANKHZ=<hz>`.
- Probado y desactivado: resolves por CPU diferidos (una espera por lote). Bajaba los draws de 20 a 10 ms, pero el juego se
  cerraba siempre al cargar un escenario (sub_827CD5D0, salto a dirección basura): las copias tardías chocan con memoria
  que el juego reutiliza. Queda como `GH6_DEFERRESOLVE=1` (experimental). WAIT_REG_MEM: info=13 sondea memoria 16A4xxxx
  (comunicación CPU-GPU), info=3 registros 0xA31/0x1973.

## ARREGLO de rendimiento: resolves en la GPU → 60 fps en canción

- Los resolves que iban por la CPU (cada uno con FlushAndWait + conversión por píxel) ahora se hacen en la GPU y se leen
  como textura resuelta, sin esperas:
  - 8888 con MSAA y media de muestras (reducción 640x360 de la escena): shader kColorResolveHlsl (g_colorResolvePso,
    RTV en g_rtvHeap3), mismas reglas que la CPU (RB_COPY_CONTROL bits 4-6).
  - 8888 → k_8: copia 8888; BindTexture la lee con todos los canales = rojo (HostRep RRRR).
  - k_32_FLOAT → k_32_FLOAT: copia R32_FLOAT.
  - 16_16 (R16G16_SNORM x32) → k_16_16: shader saturate(v·32) a R16G16_UNORM (como PackResolvePixel 25); solo se usa
    directamente si la textura se lee sin signo ni exp_adjust; si no, se vuelca a memoria.
- ResolvedTex tiene fmt (DXGI), texFmt (Xenos) y rtv; la clave lleva el formato en los bits 32-43 (ResolvedFmtTag) para
  que 8888/k_8/32_FLOAT/16_16 en la misma dirección (16E70000 se reutiliza 4 veces por frame) no se pisen.
  WriteBackResolved sabe volcar k_8, k_32_FLOAT y k_16_16; la región derivada (dsub) solo desde copias 8888.
- Resultado en la misma canción (Again): **30 → 60 frames/s**, 0 resolves por CPU y 0 volcados por frame
  (log "[d3d12] por frame: resolves por GPU ..., por CPU ..., volcados ..."). Escena 3D a 30 Hz como en la consola.
  `GH6_OLDGPURESOLVE=1` = solo la ruta anterior (8888 sin MSAA).
- Visto en menús: el icono de rango junto a "10 STARS TO RANK 2" sale como un cuadrado negro (ya pasaba antes; pendiente).
- Confirmado por el usuario: el juego va fluido con los resolves por GPU.

## Efectos verdes (bobinas / anillos de rayos): investigación, sin fallo encontrado

- Estructura del frame en canción (draw log con GH6_DUMPVS=EA26B7141D2133C7): escena con efectos → cadena a 320x180
  (8A7C3A63 reduce, 11E2C0F4 CoC → 0x630, B377A104 difumina el CoC y A38B62A0 lo copia al **alfa** del buffer reducido con
  máscara 8, EE1AFD96 desenfoque ponderado por ese CoC (máscara 7), 9EB77328 halo = 0,2333·rgb − 0,0686·CoC) → quad negro
  50C7E959 sobre la vista MSAA 4x del buffer principal (verificado con volcados: queda en negro) → fuentes de brillo extra
  (65EAEB6A) → resolve con media a 640x360 → suma aditiva al halo (8A7C3A63/B707D498 con ONE,ONE) → compositor C591749F
  → 0261F529 → interfaz. Coincide con el diseño; el alfa de la escena no se usa en el halo.
- Los anillos de rayos (PS C496DFE2 = textura × color de vértice, mezcla SRC_ALPHA/ONE, VS 256616C6) ya salen gruesos y
  brillantes en la escena antes del postproceso. Comprobado: color de vértice 8888 (zyxw) bien decodificado, textura DXT5
  0368E000 (anillo de rayo blanco con alfa suave) bien decodificada, sin texturas gamma (signo 3) en todo el juego,
  sin sesgo de exponente. Las nubes del resultado final son esos anillos desenfocados por la profundidad de campo + halo.
- Sin referencia de la consola de ese momento concreto (ráfaga de rayos) no se puede afirmar que esté mal.
- Diagnóstico nuevo: `GH6_PSCONSTLOG=<hash PS>[,...]` registra c0-c15, mezcla, máscara y superficie de esos draws.
- Referencia 360 de "Again" (YouTube pE3Jb0jwD84, A Wise Moose, 720p): mismo escenario (zona z_riothouse; el escenario de
  Quickplay es aleatorio, a veces sale z_punkstage = CBGB). En la 360 **no aparecen las nubes verdes** en esos momentos y la
  imagen tiene mucho más contraste/saturación (negros profundos); la nuestra tiene neblina.
- Descartado en esta ronda: consultas de oclusión (GH6_ZPDSAMPLES=0 no quita las nubes), LUT del escenario (08E5B000,
  32³, casi neutra; ejes y canales correctos), vupkd3d128/vpkd3d128 (correctos), color de vértice, textura DXT5 de los
  anillos, constantes de linealización (c152 = (0,0,-9.997,10): near 0.1, far 400), traducción de 11E2C0F4 (correcta;
  C[0] es el foco del DOF y cambia por plano de cámara, el CoC cercano 0 es legítimo en esos planos).
- El aviso "resolve OMITIDO" para resolves de profundidad (src=4) es falso cuando la copia por GPU ya se hizo.
- GH6_HLSLDUMP ahora vuelca también los shaders que salen de la caché. GH6_PSCONSTLOG registra además c20, c110, c111,
  c152 y c153.
- Siguiente idea: comparar con Xenia en el mismo momento para saber si las nubes son un fallo nuestro (GPU o CPU) o del
  propio juego (p. ej. dependen del estado de la partida).

## ARREGLO: cuadrado negro junto a "STARS TO RANK" (imagen de jugador)

- Era la imagen de jugador de Xbox (gamerpic, textura 8888 lineal de 64x64 en 142DE000 escrita por la CPU, dibujada con
  VS 56A07B41 / PS 768B7F0C sobre un cuadrado negro DXT1 de 4x4). XamReadTileToTexture la devolvía transparente.
- Ahora, para los tipos de tile 2-5 (imágenes de jugador), escribe en A8R8G8B8 big endian: `gamerpic.bmp` de la carpeta
  del juego si existe (BMP de 24/32 bits, cualquier tamaño, escalado) o una imagen por defecto generada en código
  (púa blanca de triángulo redondeado sobre degradado rojo-naranja). `GH6_NOGAMERPIC=1` = transparente como antes.
  Captura: `Recomp\icono_jugador.png`.

## ARREGLO GRANDE: nubes verdes, neblina y fondo claro de Select Rocker (aliasing de EDRAM color -> profundidad)

- Causa: en los frames de "restauración" el PS C316F389 escribe con MRT la escena guardada (RT1 <- 1759B000) y la
  **profundidad guardada como color** (RT0 <- 17933000, profundidad empaquetada en 8888) en la base EDRAM del buffer de
  profundidad (COLOR0 base == RB_DEPTH_INFO base == 720, modo 4). En la 360 color y profundidad comparten la EDRAM, así que
  eso restaura la profundidad. Aquí eran recursos separados: la profundidad quedaba vieja y las partículas aditivas
  (PS C496DFE2, anillos de rayos y humo con forma de calavera) que se dibujan justo después no quedaban tapadas por la
  escena → nubes verdes/blancas enormes por todas partes, neblina y fondo de Select Rocker muy claro.
- Arreglo: tras un draw 8888 en modo 4 cuya base de color coincide con la de profundidad, un pase en GPU
  (kColorToDepthHlsl, g_colorToDepthPso) convierte el color escrito en profundidad: palabra D24S8 = G | B<<8 | A<<16
  (R = stencil, no se restaura) → SV_Depth = v / 16777215. `GH6_NOCOLOR2DEPTH=1` = antes.
- Resultado: Select Rocker con el fondo de la fábrica desenfocado y cálido como la referencia; canciones en la fábrica sin
  nubes, con negros profundos y el contraste de la 360 (`Recomp\sin_nubes_fabrica.png`).
- Probado GH6_MOTIONBLUR=1 con el arreglo: Select Rocker bien, pero en canción siguen los contornos fantasma en los
  personajes → el desenfoque de movimiento sigue desactivado (otra causa pendiente).
- Ruta nueva al editor: `scripts\cas_route.txt` (START, CONTINUE del aviso de guardado, CREATE, ROCKER CREATOR, CONTINUE).
  `scripts\menu_probe.txt` recorre el menú principal.

## Imagen borrosa en ventana grande: escalado a la ventana

- La cadena de intercambio era fija de 1280x720 con DXGI_SCALING_STRETCH: en una ventana mayor (el usuario juega a ~1920)
  Windows la estiraba con un filtro bilineal (y sin respetar 16:9) → todo borroso, sobre todo personajes y textos.
- Ahora la cadena sigue el tamaño del área cliente (ResizeSwapIfNeeded tras cada Present: espera a la GPU, ResizeBuffers,
  RTV nuevos) y el blit escala con Catmull-Rom (ps_cubic, 9 lecturas bilineales; a 1:1 es exacto) en un rectángulo 16:9
  centrado con bandas negras (PresentViewport). `GH6_UPSCALE=bilineal` = filtro anterior, `GH6_FIXEDSWAP=1` = cadena fija.
  Comparación: `Recomp\escalado_antes_despues.png`.
- Comprobado también que el arreglo de profundidad no desenfoca al personaje: su CoC cercano y lejano son 0 en Select
  Rocker (la profundidad restaurada es estable entre frames seguidos).
- Confirmado por el usuario: con el escalado nuevo se ve mejor (y sin nubes ni neblina).

## Desenfoque de movimiento reactivado (por defecto) + opción en gh6.ini

- Con la profundidad restaurada (aliasing color->profundidad) las velocidades ya son coherentes: personajes quietos ~0,
  fondo con el movimiento de cámara (5-8 px), planos estáticos ~0. La velocidad alta solo queda en el trapecio que luego
  tapa la pista. Las "estelas" antiguas venían de la profundidad vieja.
- En primeros planos con movimiento rápido se ven copias translúcidas (el compositor C591749F y 0261F529 usan solo 3
  muestras a lo largo de la velocidad, con un máximo de ~7 px): es el comportamiento del juego.
- Por defecto activo. `desenfoque_movimiento=0` en gh6.ini (o `GH6_NOMOTIONBLUR=1`) pone las velocidades a cero.
  El launcher acepta además cualquier clave `GH6_...=valor` en gh6.ini como variable de entorno. SaveConfig escribe la
  opción. dist\GH6WoR\gh6.ini y dist_files\gh6.ini la incluyen.
- Preferencia del usuario: desenfoque de movimiento desactivado (desenfoque_movimiento=0 en dist\GH6WoR\gh6.ini; make_dist no sobrescribe el ini existente).

## Retraso de audio, pausas al cargar y DLC

- **Retraso de audio**: antes ~180-190 ms (cola propia de 120 ms en AudioPump + buffer WASAPI de 60 ms lleno entero).
  Ahora cola `GH6_AUDIOMS` (40 ms por defecto) y WASAPI llenado solo hasta `GH6_WASAPIMS` (20 ms, mínimo 2 periodos):
  ~60-70 ms en total. Canción completa con audio: 0 huecos, 0 descartes.
- **Vídeo**: probada la cadena con latencia máxima (`GH6_FRAMELATENCY`): 1 frame → ~48 fps, 2 → ~55 fps, sin límite → 60.
  Por defecto sin límite (con 2 buffers la cola ya es corta).
- **Pausas**: los tirones de ~50 ms a mitad de canción eran volcados GPU→memoria de las texturas k_8/k_32_FLOAT/k_16_16
  de 16E70000 resueltas a 112x207 y leídas a 110x207 (`GH6_FLUSHSTATS=1` localiza las esperas). Ahora copia de región
  derivada en GPU también para esos formatos. 65 → 19 tirones en la misma prueba; los de 0,33 s eran abrir/cerrar la pausa
  del script. Queda la carga al empezar cada canción (texturas nuevas + 129 PSO).
- **DLC**: la lista se agrupa por origen; JUMP (amarillo) → "Downloadable Content". Paquete February Mega Pack importado
  (dlc757-766: Black Sunday, Blooddrunk, Coma White, Crazy Horse, Get Your Gunn, Living Dead Beat, Parade Of The Dead...).
  "Black Sunday" carga de CONT0:\adlc766_*.fsb.xen, suena y se juega sin cierres (`Recomp\dlc_black_sunday.png`).
  Script: `scripts\dlc_play.txt`. Rutas nuevas: `qp_probe.txt`, `songlist_jump.txt` (menú: PLAY → bajar a QUICKPLAY+).

## Arranque atascado con 7 o mas DLC ("Checking for downloadable content")
- Causa: XamContentCreateInternal (ordinal 625, pedido por XexGetProcedureAddress) no existia (devolvia 0). El juego monta cada
  paquete con ella; sin ella, con 7+ paquetes el hilo principal se quedaba en sub_8242C450 esperando a que el contador +136 del
  objeto 0x82A81CF8 volviera a 0 (nunca pasaba).
- Arreglo: gh6_XamContentCreateInternal (xam.cpp) registrada en el relleno 0x8242015C (kernel.cpp), = XamContentCreateEx sin
  user_index. GH6_NOCREATEINTERNAL=1 la quita (diagnostico).
- Tambien: XamGetExecutionId ya no es un stub; escribe el puntero a la cabecera XEX 0x40006 (antes basura de la pila).
- Probado con 14 DLC: arranca, abre 76 archivos CONT sin fallos y toca una cancion DLC.
- La bisección se hizo con enlaces (junction) de los DLC de dist en run\userdata\content (scratchpad dlc_bisect.ps1).

## Saltar la intro de logos
- saltar_intro=1 (por defecto; GH6_SKIPINTRO): al leer la cabecera de data\movies\logo_intro.bik.xen (Bink con palabras de 32 bits
  invertidas: "iKIB") se ponen las palabras 2 y 4 (numero de cuadros, 580) a 1. Ahorra ~14 s. Ocultar el archivo NO sirve (el
  juego se queda esperando).

## "Paneles" y personajes duplicados en el desenfoque (fotos del usuario con flechas)
- Medio pixel: PA_SU_VTX_CNTL.pix_center = 0 (D3D9) no se aplicaba. Ahora +0.5 en pantalla (sumado a XVport1.zw, sin cambiar el
  HLSL), como Xenia. La cadena de desenfoque a 320x180 acumulaba el error y la capa borrosa quedaba corrida (fantasmas).
  GH6_NOHALFPIXEL=1 lo quita; GH6_HALFPIXAT=<s> lo activa desde el segundo s (A/B en una misma partida).
- SSAO (D0724BAB -> FC6CC728 -> 87FF4B14, escenarios punk/fabrica): D0724BAB guarda en .y la distancia/64, que pasa de 1. El resolve
  16_16 -> k_16_16 recortaba a 0..1 (saturate) y el suavizado bilateral mezclaba fondo y primer plano (lineas en el suelo, halos
  en escalera). Ahora se resuelve a R16G16_FLOAT sin recortar (modo 2 del kColorResolveHlsl); en memoria 0x7FFF = 32 como Xenia.
  GH6_1616SAT=1 vuelve a lo anterior.
- Pendiente de confirmar por el usuario. B707D498 (punkstage) copia el frame final anterior (16AD0000) donde su alfa > 1/255: no se
  ha tocado (en la fabrica el mismo shader sube el bloom 160x90).
- Diagnostico nuevo: GH6_PSOUTAT=<s> (las sustituciones de GH6_PSOUT solo desde ese segundo, via XVte.z < 0 en el PS),
  GH6_RESOLVELOG=1 (resolves de profundidad) o =<direccion> (resolves a esa direccion, con el PS anterior).
- Los escenarios cambian entre partidas aunque se use el mismo guion (punkstage / riothouse): comparar dentro de una partida.

## CAUSA REAL de los "paneles" / franjas / personajes duplicados: columnas de 40 muestras de la EDRAM
- El frame que restaura la escena (C316F389, color 8888 con la palabra D24S8 escrito en la base del depth) y luego suma las luces
  de los focos (031FA1CF, segunda pasada LEQUAL sin escritura de z) comparaba contra una profundidad con cada franja de 40 px
  cambiada por la de su vecina: en la EDRAM las baldosas de profundidad tienen las dos columnas de 40 muestras intercambiadas
  respecto a las de color (Xenia lo hace en sus copias color<->profundidad, d3d12_render_target_cache.cc "swap 40-sample columns").
- Sintomas: luz de los focos solo en columnas alineadas a 80/40 px (barras en el suelo, rectangulos sobre los personajes) y
  fantasmas en la escena restaurada.
- Arreglo: kColorToDepthHlsl lee el color en x' = x +-40 dentro de cada baldosa de 80. GH6_NOSWAP40=1 = antes.
- Verificado: manchas de luz ovaladas completas en el suelo (riothouse) y baterista sin duplicado (punkstage, mismo momento).
- Queda un rayado fino de 1 px sobre personajes iluminados que no depende de la prueba de profundidad (contenido de la mascara de
  sombras 400x400, 16EE8000; probablemente sombras finas sin suavizar). Pendiente.
- Descartado con pruebas: sesgo de profundidad en segundas pasadas (GH6_EQBIAS=<n>, GH6_EQSLOPE), VS con IEEE estricto
  (GH6_STRICTVS=1, desactivado por defecto), alfa del pase del SSAO. Diagnostico nuevo: GH6_NOZFORPS=<hash PS> (sin z),
  GH6_C2DOFFAT=<s>. Para reproducir el mismo momento: scripts\ao_toggle2.txt (segundos 115-121) y repetir hasta el escenario
  deseado (scratchpad venue_run.ps1; ojo: en .NET "\z" es fin de cadena en regex).
- Las pruebas de visualizacion con GH6_PSOUT pueden usar i.t[n] (interpolantes originales del PS).

## Rayado fino en los personajes ("tele vieja")
- Causa principal: el grano de pelicula del ultimo pase 0261F529 (lerp(color, ruido*0.5, 0.08); ruido = frac(textura 64x64 +
  668.3*u + 270700*v)). Con coordenadas de pixel exactas el hash varia muy poco entre filas y el grano sale en rayas verticales.
  Probado con GH6_PSOUTAT en la misma partida: sin grano las rayas desaparecen.
- grano_pelicula=0 en gh6.ini (por defecto; GH6_NOGRAIN=1) devuelve el color sin ruido. grano_pelicula=1 lo deja.
- Profundidad ahora D24_UNORM_S8_UINT (como la EDRAM) en vez de D32 float: los personajes se vuelven a dibujar con LEQUAL sobre
  la profundidad restaurada y con D32 la rejilla de redondeo no coincidia (columnas de 1 px sin dibujar). GH6_DEPTH32=1 = antes.
- La textura proyectada tf0 de D25E80D3 (03AC2000) es un degradado redondo de foco: no tiene rayas.

## Sombras rayadas en caras/personajes (ultimo resto de las lineas)
- C316F389 (restaura color y profundidad del frame anterior) lee con coordenadas en texeles enteras (posicion de PsParamGen,
  con su propio intercambio de columnas de 40) y filtro LINEAL: caian en la esquina de 4 texeles y mezclaban la palabra D24S8
  empaquetada -> profundidad rota en pixeles sueltos -> la luz de los focos (031FA1CF, LEQUAL) fallaba a rayas.
- Arreglo en el traductor (gpu_shader.cpp): tfetch no normalizado sin offset lee el centro del texel (uv += 0.5).
  GH6_NODENORMCENTER=1 = antes. Verificado en la fabrica (guitarrista: cara limpia).

## Modo Quest en negro (con la partida del usuario)
- Reproducido copiando dist\GH6WoR\userdata\content\00000001_progress a run (copia; se restauro el de pruebas despues).
- Causa: la llave de la cache de texturas ((base<<20)^(w<<32)^(h<<46)...) se solapaba; dos texturas distintas compartian recurso,
  se subia una de 512x256 a un recurso de 256x256 -> Close E_INVALIDARG -> lista descartada y luego DEVICE_HUNG (0x887A0006).
- Arreglo: TexKey (FNV de base, fmt, w, h, extra, tipo) para 2D, 3D, cubo y la memoria de hashes.
- Nuevo: CheckedCopy envuelve toda CopyTextureRegion (tamanos, alineaciones, formato); registra y omite lo invalido
  ("copia de textura invalida omitida"). GH6_NOCOPYCHECK=1 la quita. Con la llave nueva: 0 avisos.
- La capa de depuracion de D3D12 (GH6_D3D12_DEBUG) no da mensajes: faltan las "Herramientas de graficos" de Windows.
- Ruta a Quest con partida avanzada y saltar_intro: scripts\quest_probe5.txt; sin saltar intro: quest_probe2.txt.

## v1.0 (en curso): mando, bateria, voz/microfono, PSO en disco, estabilidad
- Mando como guitarra (xam.cpp GetGuitarMap/PadToGuitar): un mando XInput o PlayStation se presenta como guitarra (subtipo 6) con
  mapeo mando_<funcion>= en gh6.ini (launcher -> GH6_MANDO_*). mando_modo=mando = mando normal. Guitarra real pasa sin cambios.
- Bateria (subtipo 8): mando_instrumento=bateria o F6. Pads verde=A rojo=B amarillo=Y azul=X naranja=RB bombo=LB; fuerza de golpe
  en los sticks (LY rojo|verde, RX azul|amarillo, RY bombo|naranja; formato de 360GHDrums2Midi). Verificado: pista de bateria.
- Voz: mando_instrumento=voz -> subtipo 1 (mando normal); con microfono presente el juego asigna voz a ese jugador.
- MICROFONO USB emulado (MicDeviceRequest, kernel ordinal 689; sin documentar, Xenia no lo implementa):
  * XamUserGetDeviceContext(usuario, 4, &c) -> c = 0x60000001 (familia 6 = Mic, puerto 1). 5 = RmcDeviceRequest (otra familia).
  * Peticion {u16 tipo, u16 puerto, +4 resultado, +12 contexto, +16 callback, +20 buffer, +24 len/n, +28 setup USB}.
  * tipo 0 estado: 3 hasta abrir (el juego inicializa: sub_82438660 -> B8C8 info, B5E8, B6A0 abrir), 5 despues.
  * tipo 8: frecuencias admitidas (0x140 = 32k|48k). tipo 1: abrir (48 kHz). tipo 4: iniciar (rate, bytes/ms = 96).
  * tipo 2: control USB Audio (SET_CUR / GET_CUR/MIN/MAX/RES de volumen, valores LE).
  * tipo 7: lectura de n=8 paquetes de 1 ms (96 bytes, PCM s16 LE 48 kHz mono) -> se completa desde un hilo propio cada 8 ms
    llamando al callback sub_8276ADE8(peticion, 0, estados u32[n], longitudes u16[n]); el juego hace polling del bloque (997).
  * Las demas se completan llamando al callback (peticion, 0) y devolviendo STATUS_PENDING (la maquina de estados encadena).
  * Captura: mic_capture.cpp (WASAPI compartido, AUTOCONVERTPCM a 48k mono s16, anillo con latencia <= 60 ms).
  * Verificado: sin microfono el juego dice "Mic Check: no valid mics"; con el emulado entra a cantar, y con GH6_MIC_TONO=-1
    (barrido 110-440 Hz) la barra del multiplicador se llena (notas acertadas); en silencio no.
  * gh6.ini: microfono=1, microfono_ganancia=1.0. Diagnostico: GH6_MICTRACE=1, GH6_MIC_TONO=<Hz>|-1.
- PSO en disco: shadercache\pso.lib (ID3D12PipelineLibrary, propio de cada driver; no se copia a dist). 2a partida: 208 PSO cargados.
- Texturas >= 128x128 bloques se destilean en varios hilos (GH6_TEXTHREADS).
- CheckedCopy + TexKey (ver arriba). Registro de terminate/abort/atexit y de excepciones C++ (las 5 primeras).
- Estabilidad: prueba de 30 min en Quest (scripts\long_quest.txt): memoria estable ~1,1 GB de trabajo / 4,2 GB privados, sin
  cierres. Una prueba anterior se cerro sola a los 14,5 min sin rastro (antes del registro nuevo): no se repitio.

## Varios jugadores (hasta 4) con instrumento propio
- xam.cpp: entrada por jugador. Jugador N = mando XInput N (jugadorN_xinput, 0 = ninguno); teclado y mando PS van al jugador
  teclado_jugador / mando_ps_jugador (1 por defecto). jugadorN_instrumento=guitarra|bateria|voz (jugador 1: tambien mando_instrumento);
  F6 recorre los del jugador del teclado. XamInputGetCapabilities/GetState/SetState aceptan usuarios 0-3 (0xFF = jugador 1) y
  devuelven DEVICE_NOT_CONNECTED si el jugador no tiene dispositivo. GetKeystrokeEx con 0xFF recorre los jugadores y escribe el usuario.
- Microfono: MicPlayer() = primer jugador conectado con voz (si no hay, GH6_MIC_USER). XamUserGetDeviceContext lo usa.
- Jugadores 2-4 = invitados (sin perfil, sus partidas no pisan la del jugador 1). XamShowSigninUI (antes stub que dejaba el juego
  esperando) emite XN_SYS_UI abierta/cerrada; en el aviso "not signed in" se elige "Continue without saving".
- Guiones: prefijo "P2:" = jugador 2 (scripts\p2_lobby.txt). Verificado: jugador 2 con bateria entra en el lobby con el icono de
  baquetas (subtipo 8); con voz entra con el micro y XamUserGetDeviceContext(usuario 1) -> 60000001.
- Arreglo: mando_palanca=RS ahora usa el stick derecho (antes miraba el clic del stick).

## Launcher (launcher_ui.cpp)
- Ventana Win32 propia (tema oscuro, controles comunes v6 por manifiesto incrustado, DPI por monitor solo en ese hilo) que se abre
  antes del juego: pestanas Juego (ISO, carpeta DLC, saltar intro), Graficos (pantalla completa, tamano de ventana, escalado, VSync,
  desenfoque, grano), Controles (instrumento y mando de cada jugador con estado en vivo, teclado/PS, mando normal, reasignar botones
  de guitarra/bateria pulsando el boton del mando, prueba en vivo de trastes/rasgueo/palanca) y Microfono (dispositivo WASAPI,
  ganancia, medidor de nivel). JUGAR guarda gh6.ini (conserva comentarios y claves desconocidas) y arranca.
- mostrar_launcher=0 entra directo; Mayus al abrir lo muestra; --jugar lo salta, --launcher lo fuerza; GH6_NOLAUNCHER=1 (pruebas).
- Claves nuevas -> entorno (launcher.cpp LoadConfig): pantalla_completa -> GH6_PANTALLA_COMPLETA, ventana -> GH6_VENTANA (gpu_window),
  vsync -> GH6_VSYNC, escalado=bilineal -> GH6_UPSCALE, microfono_dispositivo -> GH6_MIC_DISPOSITIVO (mic_capture: id de WASAPI).
- Probado: las 4 pestanas, Guardar (diff del ini correcto) y JUGAR -> juego hasta el escenario. Sin probar con botones fisicos:
  la captura de "Asignar..." y la prueba en vivo (necesitan pulsar el mando).

- Cambio de instrumento en caliente (F6, guion P2:INST): el juego solo lee el subtipo al conectarse, asi que SetInstrument
  desconecta al jugador 700 ms y emite XN_SYS_INPUTDEVICESCHANGED (0x12) al empezar y al acabar. Verificado: jugador 2 pasa de
  bajo a bateria dentro del lobby sin salir del hueco (scripts\p2_inst.txt). GH6_NORECONECTAR=1 = antes.

## Sincronia y pulido (v1.0, puntos 3 y 4)
- Medida del retraso de audio en el log ([wasapi] retraso de salida, cada 5 s) y fps ([fps], cada 5 s). En cancion, sin contar
  tarjeta: cola 40 ms -> ~52 ms, 0 huecos (60 fps); cola 25 -> ~38 ms, 1 hueco en una carga; cola 15 -> ~28 ms, 5 huecos.
  GH6_FRAMELATENCY=1 baja a 50-58 fps: no se usa. GetStreamLatency devuelve 0 en modo compartido.
- Launcher (pestana "Imagen y audio"): latencia_audio=baja|normal|segura -> GH6_AUDIOMS 25/40/80 + GH6_WASAPIMS 20/20/30.
  Texto que indica calibrar en el juego: Options > Calibrate Lag > Calibrate (verificado: menu Video/Audio lag y calibracion
  de video con notas funcionan; scripts\calib_probe.txt, options_probe.txt).
- Icono propio (tools\make_icon.py -> project\gh6.ico: pua naranja con rayo, sin logotipos) + VERSIONINFO (project\gh6.rc,
  CMake con lenguaje RC). Ventanas del launcher y del juego usan el icono 1.
- Botones por jugador: jugadorN_<clave> (p. ej. jugador2_mando_verde=LT, jugador2_mando_palanca=RS) tiene prioridad sobre la
  comun (PlayerEnv en xam.cpp; GetGuitarMap(u)/GetDrumMap(u) por jugador). Launcher: combo "Todos los jugadores / Solo jugador N";
  los botones sin valor propio salen como "(comun)"; Restablecer quita los propios. Verificado en el log del juego.
- ARREGLO: en la ronda anterior el comentario de mando_palanca=RS anulo la linea de la palanca (con un mando normal la palanca
  no funcionaba ni con RT). Corregido.
- Mando desconectado: el jugador del teclado se ve desconectado si tenia mando y lo pierde -> el juego pausa con
  "Disconnected controllers: Player 1's Guitar Controller" y al volver "All controllers are connected!" (verificado con
  GH6_DESCONECTAR=<jugador>:<desde>:<hasta>, scripts\disc_test.txt). Una tecla del teclado permite seguir sin mando.
- Titulo de la ventana limpio ("Guitar Hero: Warriors of Rock"); contadores solo en modo desarrollo (GH6_DEVMODE lo pone main.cpp).

## Release para GitHub, partidas y actualizador
- tools\make_release.ps1: compila, prepara Recomp\release_staging y crea en la raiz del proyecto (formato de las releases de
  Persona) la carpeta "Guitar Hero Warriors of Rock\" (exe "Guitar Hero Warriors of Rock.exe", DLL de FFmpeg, gh6.ini sin rutas
  personales con datos=save, README.md, README-Windows.txt, LICENSE MIT, THIRD-PARTY-NOTICES.txt, licenses\, dlc\, save\) y
  Guitar-Hero-Warriors-of-Rock-PC-v<ver>-windows.zip + .sha256. No copia shaders\*.bin ni shadercache (son del juego) y falla si
  encuentra .iso/.xex/.bin/.cso/.log. Al recopiar la carpeta no pisa gh6.ini, save\ ni dlc\. Version: updater.cpp AppVersion() y
  project\gh6.rc deben coincidir (el script lo comprueba). Documentos en dist_files\release\.
- Launcher, pestana Juego -> Partidas: Abrir carpeta, Empezar de cero (mueve content\00000001_* a <datos>\respaldos\partida_<fecha>;
  el DLC importado 00000002/00009000 no se toca), Restaurar respaldo (el mas reciente; antes respalda la actual). Probado en
  run\updtest (scratchpad save_run.ps1).
- Actualizador (updater.cpp): GitHub API releases/latest de actualizar_repo (por defecto Piscacho/Guitar-Hero-Warriors-of-Rock-PC),
  compara tag_name con AppVersion, descarga el primer asset .zip (WinHTTP), lo descomprime con tar.exe, instala junto al exe
  conservando gh6.ini, save, userdata, dlc, shadercache, shaders y la carpeta de datos del ini; exe/DLL en uso -> *.old (se borran
  al arrancar, UpdaterCleanup); reabre el launcher. actualizar_al_abrir=1 comprueba en segundo plano al abrir (sin avisos si falla).
  Probado de principio a fin con un servidor local (GH6_UPDATE_API=http://127.0.0.1:8765/api.json, GH6_UPDATE_AUTOYES=1;
  scratchpad upd_test.ps1 / upd_run.ps1).
- Pestana Ayuda nueva (teclas, varios jugadores, sincronia, archivos). Contador de DLC ignora .txt/.md.
- Para publicar: crear el repo Piscacho/Guitar-Hero-Warriors-of-Rock-PC, release con etiqueta v1.0.0 y subir el .zip como asset.

## Publicado en GitHub (2026-11-01)
- Repo publico https://github.com/Piscacho/Guitar-Hero-Warriors-of-Rock-PC: Recomp\ es ahora un repo git (rama main, primer
  commit cb629d2, 152 archivos). XenonRecomp va como submodulo (hedge-dev/XenonRecomp @ ddd128b) + xenonrecomp_gh6.diff; el
  submodulo sale "modificado" en git status por el parche local (normal).
- .gitignore deja fuera todo lo del juego (xex/, ppc/, *.iso, *.xex, *.bin, *.cso, shaders_dis.txt, capturas) y lo pesado o
  regenerable (toolchain/, ref/, dist/, run/, logs/, project/build/, release_staging/).
- README.md de la raiz = guia de juego + "Compilar desde el codigo". tools\build_game.bat usa rutas relativas.
- Release v1.0.0 publicada con el zip y su .sha256. El actualizador del launcher contra GitHub real responde
  "Tienes la ultima version (v1.0.0)".
- Para la siguiente version: subir AppVersion() (updater.cpp) y FILEVERSION (gh6.rc), tools\make_release.ps1, commit + push,
  gh release create v<ver> <zip> <zip>.sha256.

## Suavizado de bordes (FXAA) y estudio de logros / ultrapanoramico
- gpu_d3d12.cpp: ps_fxaa (FXAA 3.11 de calidad: busqueda de 8 pasos, subpix 0.5, umbrales 0.125/0.0625) en kBlitHlsl. En la
  presentacion directa (D3D12PresentResolved) la imagen resuelta pasa por FXAA a g_aaTex (mismo tamano, RTV propio, SRV fijo 20)
  y despues al escalado Catmull-Rom. Las capturas (DUMP/F12) salen ya suavizadas. GH6_SUAVIZADO=no lo desactiva; launcher
  "Suavizado de bordes" (suavizado=fxaa|no, por defecto fxaa). Coste: 60 fps estables en cancion (una medida dio 55-59: variacion).
  Comparacion: run\aa_compare.png (borde del mastil escalonado sin FXAA, liso con FXAA).
- SMAA: necesita las tablas AreaTex/SearchTex de iryoku/smaa (MIT); pendiente de que el usuario autorice descargarlas.
- Logros: registro [xmsg] de XMsgStartIORequest/InProcessCall (60 lineas). En menus y cancion: app 0xFB (xgi) mensajes 0xB0006
  (XUserSetContext) y 0xB0007 (XUserSetProperty); app 0xFA 0x7001A/0x7001B (XMP); app 0xFC 0x58004/0x58046. Los logros llegarian
  por 0xFB / 0xB0008 (XUserWriteAchievements: {count, ptr -> {user, id}}). Definiciones (nombres, textos, iconos PNG) en el XDBF
  de los recursos del default.xex del ISO del usuario.
- 21:9 / 32:9: el juego solo contempla 4:3 y 16:9 y dibuja a 1280x720 en EDRAM; ultrapanoramico real exige FOV horizontal mayor,
  superficies mas anchas y HUD anclado: depende de la resolucion interna escalable. Estimacion: dificil, varias sesiones.

## Texturas: filtrado anisotropico y nitidez CAS
- Los PS traducidos muestrean con SampleGrad (gradientes reales), asi que el muestreador lineal estatico de los draws (s0) pasa a
  D3D12_FILTER_ANISOTROPIC con MaxAnisotropy = GH6_ANISO (1/2/4/8/16, por defecto 16). El nivel entra en el hash de la cache de
  PSO (la firma raiz cambia): la primera partida tras cambiarlo vuelve a crear los PSO.
- ps_cas (CAS de AMD FidelityFX, MIT) a la resolucion del juego despues del FXAA: cadena de posproceso g_ppTex[0/1] (SRV 20/21,
  PostPass), GH6_NITIDEZ=0..1 (por defecto 0.4, 0 = sin pase). Capturas DUMP/F12 salen del ultimo pase.
- Launcher: "Filtrado de texturas" (anisotropico=16|8|4|1) y "Nitidez" (nitidez=0|0.25|0.4|0.7). 60 fps en los tres modos.
  Comparacion del tramo lejano del mastil: run\tx_highway.png.
- Pendiente: las capturas DUMP no salen en la pantalla de calibracion (Options > Calibrate Lag), con o sin audio; en canciones
  y menus si. Sin investigar.

## Guitarras de terceros (CRKD) y mando de Xbox
- Guitarra real: subtipos XInput 6, 7, 8 y 11 (bajo); al juego se le anuncia 6 (8 si es bateria). Segun resenas, la CRKD Les Paul
  en PC (receptor 2.4 GHz o cable) se presenta como mando XInput; no se sabe si con subtipo de guitarra.
- mando_inclinacion=<botones> (y jugadorN_mando_inclinacion): inclinacion = eje RY de la guitarra de 360 (32767 al levantar) para
  guitarras que llegan como mando normal. En el launcher es la fila "Inclinacion (estrella)" (vacia por defecto).
- Launcher, Controles: "Informe de mandos" graba 15 s (tipo/subtipo/flags de cada mando XInput y cada cambio de botones y ejes)
  en mandos.txt y lo abre: para configurar guitarras/baterias que no tenemos. El estado muestra el subtipo.
- "Restablecer" -> "Preajustes": mando de Xbox clasico (A/B/Y/X/LB, colores de los botones), mando de Xbox gatillos (LT/LB/RB/RT/A,
  palanca RS, estrella BACK|Y|X), valores por defecto y quitar los propios del jugador. Se aplican a todos o al jugador elegido.

## v1.1.0 publicada (2026-11-01)
- Commit 6dbe981 en main; release v1.1.0 con zip + sha256 (SHA-256 20471f9f...). Release v1.0.0 borrada a peticion del usuario
  (la etiqueta git v1.0.0 se conserva).
- Prueba real del actualizador: copia v1.0.0 (run\upd_real) -> "Nueva version v1.1.0 disponible" -> instalar -> en 4 s queda
  v1.1.0 y el launcher se reabre ("Version instalada: v1.1.0"), sin .old.

## Guitarra CRKD Gibson Les Paul (informe de mandos de un amigo del usuario)
- Llega como mando XInput (tipo 1, subtipo 1, flags 0). Trastes A B Y X LB (verde, rojo, amarillo, azul, naranja), rasgueo en la
  cruceta, palanca en LT (analogico 0-255), inclinacion en RT (255 al levantar). Su sensor de movimiento mueve LS y RS al
  inclinarla/sacudirla (con la asignacion por defecto LS rasgueaba solo). Start/Back no aparecen en el informe.
- Preajuste "Guitarra CRKD (Les Paul) en PC" en Controles > Preajustes: A B Y X LB, UP/DOWN/LEFT/RIGHT solo cruceta, START,
  BACK|RB, palanca LT, inclinacion RT. Verificado en el log del juego (rasgueo 1/2, palanca 10000, inclinacion 20000).

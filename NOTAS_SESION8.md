# Sesión 8 — Traductor Xenos → HLSL y primeros draws: ¡primera imagen!

Fecha: 2026-10-26. Sin commits.

## Resultado

**El juego recompilado dibuja**: el vídeo de arranque (Bink, decodificado por la CPU traducida) se ve en el render target D3D12.
Captura: `run/frame_150.png` (logo de *Guitar Hero: Warriors of Rock*). Por frame se dibujan 11 draws y se omiten los puntos y los resolves.

## Traductor (`runtime/gpu_shader.cpp/.h`)

- Microcódigo Xenos → HLSL SM5, compilado con `D3DCompile` (vs_5_0 / ps_5_0). **16/16 shaders capturados compilan.**
- Temporales `float4 r[64]`, `ps` (resultado escalar previo), `p0` (predicado), `a0`.
- Cbuffer `XConsts` (b0): `C[256]` (VS: registros 0x4000; PS: 0x4400), 256 bools (0x4900), `XFetch[8]` (offset/endian de cada stream) y VTE/viewport.
- ALU: casi todos los opcodes vectoriales y escalares, con la semántica de operandos de Xenia (escalar: componentes de los slots W/X de src3;
  `mulsc/addsc/subsc` = constante + temporal), clamp, predicación, exports con la regla de máscaras de Xenia. Pendientes: `cube`, bucles, saltos y llamadas.
- `vfetch`: lectura de bytes crudos (`ByteAddressBuffer` t0) con `XSwap` según el endian; formatos 6 (8888), 25/26 (16), 31/32 (half), 36/37/38/57 (float).
  Índice desde un registro (floor/round); los mini-fetch reutilizan índice y stride.
- `tfetch` 2D con `SampleLevel` (t1..t16, sampler lineal clamp).
- VS: transformación de viewport de Xenos (`PA_CL_VTE_CNTL` 0x2206 + `PA_CL_VPORT_*` 0x210F-0x2114) → NDC de un RT de 1280x720.
- Prueba sin lanzar el juego: `GH6_SHADERTEST=shaders GH6.exe` (desde `run/`) → traduce y compila cada `.bin` y deja el HLSL en `run/shaders_hlsl/`.

## Draws (`runtime/gpu_cp.cpp` → `gpu_draw.h` → `gpu_d3d12.cpp`)

- El procesador de comandos guarda el último VS/PS cargado (IM_LOAD*) y en `DRAW_INDX(_2)` pasa primitiva, índices (DMA 16/32 con endian, inmediatos, auto).
- D3D12: caché de shaders y de PSO (VS/PS/primitiva/`RB_BLENDCONTROL0`), anillo de subida de 64 MB por frame (constantes, streams copiados desde la memoria física,
  índices, texturas), RT "EDRAM" 1280x720 BGRA que se limpia en cada frame, firma raíz: 2 CBV (VS/PS) + SRV crudo + tabla de 16 SRV.
- Primitivas: listas/strips de triángulos y líneas nativas; **rect list** con un geometry shader (diagonal = arista más larga, 4º vértice = a + b − esquina);
  fan y quad list → lista de triángulos en CPU. **Puntos omitidos** (son draws de 1 vértice internos de D3D, unos 48 por frame).
- **Draws con `RB_MODECONTROL & 7 == 6` (copy/resolve) se omiten**: dibujarlos tapaba todo con un color uniforme.
- Texturas (solo mip base): 2 (`k_8` → R8), 6 (8888 → RGBA8), 18/19/20 (DXT1/3/5 → BC1/2/3); desentrelazado por bloques, endian y swizzle vía *component mapping*.
  Caché por dirección/formato/tamaño con hash de una muestra de los datos. Descriptores en un anillo de 8192 por frame. Formato 22 (`k_24_8`, profundidad) sin soporte.
- Presentación: si hubo draws se presenta el RT "EDRAM"; si no, el frontbuffer. `GH6_SHOWFRONTBUFFER=1` fuerza el frontbuffer.

## Diagnóstico nuevo

- `GH6_DUMPFRAME=n`: guarda el RT del frame n en `run/frame_n.bmp` (para verlo se convierte a PNG con un script corto, como `frame_150.png`).
- `GH6_DRAWLOG=n`: registra cada draw del frame n con `RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`, `RB_MODECONTROL`, `RB_COPY_*`, blend y textura 0.
- `GH6_NODRAW=1` (sin draws), `GH6_NOTEXTURES=1` (todo blanco), `GH6_D3D12_DEBUG=1`.

## Estructura de un frame del arranque (de `GH6_DRAWLOG=200`)

1. Draws a EDRAM y **resolves (modo 6)** a 0x17923000, 0x17CBB000 y 0x17DAB000 (cadena de reducción 1280x720 → 640x360 → 320x180, formatos 24_8/16_16_16_16).
2. Draw del vídeo: strip con 3 texturas `k_8` (YUV) y PS `6EF6E0F290D7865C` → color.
3. Resolve final a **0x17DF0000 = frontbuffer** (`COPY_DEST_INFO` 0x01000300).

## Próximos pasos

1. **EDRAM y resolves de verdad**: un RT por base/formato de EDRAM (`RB_COLOR_INFO` base en tiles, `RB_SURFACE_INFO` pitch) y resolve =
   copiar el rectángulo (`RB_COPY_*`, `PA_SC_WINDOW_*`) a la memoria física en formato *tiled* (con clear si `COPY_CTRL` lo pide).
   Así funcionarán el posproceso y el frontbuffer real.
2. Profundidad/stencil (`RB_DEPTHCONTROL`), formato 22 como textura, cull/scissor y alpha test.
3. Puntos (point sprites con `PA_SU_POINT_SIZE`), `cube`, bucles en shaders y mips de texturas.

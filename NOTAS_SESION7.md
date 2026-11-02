# Sesión 7 — Inicio del dibujo real: backend D3D12 y desensamblador de shaders Xenos

Fecha: 2026-10-26. Sin commits.

## Resultado

- **Backend D3D12** (`runtime/gpu_d3d12.cpp`): dispositivo, cola, swapchain *flip discard* 1280x720 sobre la ventana del juego, heaps,
  fences, firma raíz y PSO de *blit*, con HLSL compilado en tiempo de ejecución con `D3DCompile`.
  En cada `XE_SWAP` el frontbuffer físico se desentrelaza y se presenta por D3D12 (subida a una textura + triángulo a pantalla completa).
  Todo D3D12 se usa desde el hilo del procesador de comandos. El estado (backend, frames, draws) va en el título de la ventana.
  - `GH6_GDI=1`: vuelve a la ventana GDI. `GH6_D3D12_DEBUG=1`: activa la capa de depuración. Si D3D12 falla, se pasa solo a GDI.
  - El frontbuffer seguirá vacío hasta emular los draws y los resolves de la EDRAM.
- **Desensamblador de microcódigo Xenos** (`tools/xenos_dis.py`): control de flujo (exec/cexec/loops/alloc...), ALU vectorial y escalar
  emparejadas (swizzles relativos a componente, negación, temporal/constante, predicación, exports) y fetch de vértices y texturas.
  Uso: `python -I tools/xenos_dis.py run/shaders > shaders_dis.txt`. Salida de los 16 shaders del arranque/menú en `shaders_dis.txt`.
  - VS típico: `vfetch` posición float4 (fmt 38, stride 24) + UV float2 (fmt 37, off 16) desde **vf95** → `oPos`, `o0`.
  - PS típico: `tfetch r1, tf0` → `mul oC0, r1, r0` (textura × color), o `max oC0, r0, r0` (= mov). Uno hace un swizzle BGRA (`r0.zyxw`).
  - Por verificar contra Xenia: el bit 7 de los registros temporales (lo mostramos como |abs|), el nombre de los tipos de `alloc`
    y los campos de formato de `vfetch`.
- **Referencia**: `ref/xenia` = clon disperso (solo `src/xenia/gpu`) de Xenia (BSD), para portar el traductor de shaders, los formatos de
  texturas y la EDRAM. No se compila; es solo para consulta.

## Plan para dibujar (sesiones siguientes)

1. **Traductor Xenos → HLSL** (`runtime/gpu_shader.cpp`): a partir del desensamblador (portado a C++), generar HLSL SM5 con
   registros temporales como array, constantes ALU (0x4000), booleanas y de bucle en *constant buffers*, control de flujo
   con un bucle `switch(pc)` (como hace Xenia) y exports → `SV_Position`/`TEXCOORDn`/`SV_Target`. Compilar con `D3DCompile` y cachear por hash.
2. **Vertex fetch**: constantes de fetch (0x4800 + 2·índice: dirección física, tamaño y endianness) → leer los vértices de la memoria de invitado
   (BE, con swap) a un buffer de subida; formatos 37/38 (float) primero.
3. **Estado** desde registros: `PA_SC_*`/`PA_CL_VTE_CNTL` (viewport), `RB_COLOR_INFO` (render target en EDRAM), blend (`RB_BLENDCONTROL0`),
   profundidad (`RB_DEPTHCONTROL`), `VGT_DRAW_INITIATOR` (primitiva: 1 point, 4 tri list, 5 tri fan, 6 tri strip, 8 rect list, 13 quad list).
   Las *rect lists* y *quad lists* se convierten en triángulos.
4. **Texturas**: constantes de fetch de textura (6 dwords), des-tiling y formatos (8888, DXT1/3/5...).
5. **EDRAM**: render target D3D12 por superficie de EDRAM + **resolve** (`RB_COPY_*`) → escribir el resultado en memoria física.
   Así el frontbuffer que ya se presenta mostrará la imagen del juego.

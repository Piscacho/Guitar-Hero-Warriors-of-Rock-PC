// Traductor de microcodigo Xenos -> HLSL (sesion 8).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TranslatedShader
{
    bool ok = false;                 // false: control de flujo no soportado (bucles/saltos) u otro problema
    std::string hlsl;
    std::string error;
    std::vector<uint32_t> vertexFetchSlots; // indices de constante de fetch (vfN) que usa el VS, en orden de slot
    uint32_t textureMask = 0;        // tfN usados como 2D (bit N)
    uint32_t texture3DMask = 0;      // tfN usados como 3D (bit N, sesion 21)
    uint32_t textureCubeMask = 0;    // tfN usados como cubo (bit N, sesion 21)
    bool writesDepth = false;        // el PS exporta profundidad (oDepth -> SV_Depth, sesion 23)
    uint32_t mrtMask = 0;            // bits 1-3: el PS escribe oC1-oC3 (MRT, sesion 24)
};

// type: 0 = vertex, 1 = pixel. words: microcodigo (dwords en orden host).
TranslatedShader TranslateXenosShader(uint32_t type, const uint32_t* words, uint32_t count);

// Estructura comun VS->PS (posicion + 16 interpoladores). El GS de rect lists la reutiliza.
extern const char* kXenosInterfaceHlsl;

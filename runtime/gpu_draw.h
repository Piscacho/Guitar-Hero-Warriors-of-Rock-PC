// Draws de Xenos -> D3D12 (sesion 8).
#pragma once
#include <cstdint>
#include <vector>

struct XenosDraw
{
    uint32_t primType = 0;      // VGT_DRAW_INITIATOR & 0x3F
    uint32_t numIndices = 0;
    uint32_t source = 2;        // 0 DMA, 1 inmediato, 2 auto
    bool index32 = false;
    uint32_t indexAddr = 0;     // fisica (DMA)
    uint32_t indexEndian = 0;
    std::vector<uint32_t> immediate; // indices inmediatos ya desempaquetados
    const uint32_t* vsCode = nullptr; uint32_t vsCount = 0; uint64_t vsHash = 0;
    const uint32_t* psCode = nullptr; uint32_t psCount = 0; uint64_t psHash = 0;
};

void D3D12Draw(const XenosDraw& d);
bool D3D12FlushPendingResolves();   // sesion 24: completa los resolves por CPU diferidos

// Interfaz interna de la GPU (procesador de comandos + ventana).
#pragma once
#include "runtime.h"
#include <atomic>

struct GpuStats
{
    std::atomic<uint64_t> packets{ 0 }, draws{ 0 }, swaps{ 0 }, interrupts{ 0 };
    std::atomic<uint32_t> frontbuffer{ 0 }, width{ 0 }, height{ 0 };
};

void GpuStartCommandProcessor();
void GpuSetRingBuffer(uint32_t phys, uint32_t sizeLog2);
void GpuSetReadPointerWriteback(uint32_t phys);
void GpuSetInterruptCallback(uint32_t cb, uint32_t user);
void GpuDispatchInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu);
const GpuStats& GpuGetStats();
void GpuPresent();          // ventana (gpu_window.cpp)
void GpuStartWindow();

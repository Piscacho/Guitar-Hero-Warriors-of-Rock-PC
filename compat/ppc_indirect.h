// Incluido a la fuerza en el codigo PPC: las llamadas indirectas pasan por una comprobacion
// que avisa si el destino no tiene funcion traducida (en vez de saltar a la direccion 0).
#pragma once
#include <cstdint>
struct PPCContext;
void GuestCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x) GuestCallIndirect(ctx, base, (x))

// Half float de Xenos -> float. Sin Inf/NaN: el exponente 31 es un valor normal (como en Xenia).
static inline float ppc_xenos_half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0)
    {
        if (m == 0) bits = s;
        else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3FF; bits = s | ((e + 112) << 23) | (m << 13); }
    }
    else bits = s | ((e + 112) << 23) | (m << 13);
    float f; __builtin_memcpy(&f, &bits, 4); return f;
}

// float -> half de Xenos (misma logica que el pack FLOAT16_4 de XenonRecomp: trunca, satura a 0x7FFF, sin Inf/NaN)
static inline uint16_t ppc_float_to_xenos_half(float f)
{
    uint32_t b; __builtin_memcpy(&b, &f, 4);
    uint32_t a = b & 0x7FFFFFFF; float af; __builtin_memcpy(&af, &a, 4);
    uint16_t out;
    if (af != af || af > 65504.0f) out = 0x7FFF;
    else
    {
        uint32_t e = (b & 0x7F800000) >> 23; uint16_t m = (uint16_t)((b & 0x7FE000) >> 13);
        out = e > 0x70 ? (uint16_t)(((e - 0x70) << 10) + m) : (0x71 - e > 31 ? 0 : (uint16_t)((0x400 + m) >> (0x71 - e)));
    }
    return out | (uint16_t)((b & 0x80000000) >> 16);
}

// Divisiones de PPC: entre 0 (o INT_MIN / -1) no hay excepcion en Xenon; en x86 si. Devolvemos 0 (como Xenia).
static inline int32_t ppc_divw(int32_t a, int32_t b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a / b; }
static inline uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b == 0 ? 0 : a / b; }
static inline int64_t ppc_divd(int64_t a, int64_t b) { return (b == 0 || (a == INT64_MIN && b == -1)) ? 0 : a / b; }
static inline uint64_t ppc_divdu(uint64_t a, uint64_t b) { return b == 0 ? 0 : a / b; }

// lwarx/stwcx con reserva versionada (sesion 6): un stwcx con exito invalida las reservas de otros hilos
// sobre la misma direccion aunque el valor vuelva a ser el mismo (evita el problema ABA del CAS puro).
void ppc_reserve(uint32_t ea);
bool ppc_stwcx32(uint8_t* base, uint32_t ea, uint32_t expectedRaw, uint32_t newRaw);
bool ppc_stdcx64(uint8_t* base, uint32_t ea, uint64_t expectedRaw, uint64_t newRaw);

// mftb: time base de Xenon a 50 MHz (sesion 14). Implementado en runtime/atomics.cpp con QueryPerformanceCounter.
uint64_t ppc_mftb();

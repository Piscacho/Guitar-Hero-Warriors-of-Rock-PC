// Runtime base de la recompilacion de GH:WoR. Memoria de invitado de 4 GB + utilidades.
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdint>
#include <cstdio>

extern uint8_t* g_base;              // base de la memoria de invitado (4 GB)

// Asignadores "bump" (temporal; sustituir por un heap real en sesiones posteriores)
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align = 0x1000);   // 0x40000000..0x7FFF0000
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align = 0x10000); // 0xA0000000..0xBFFFFFFF
uint32_t GuestAllocKernel(uint32_t size, uint32_t align = 0x10);      // objetos del "kernel"

inline void* GuestToHost(uint32_t addr) { return g_base + addr; }
inline uint32_t Load32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(g_base + a)); }
inline void Store32(uint32_t a, uint32_t v) { *(uint32_t*)(g_base + a) = __builtin_bswap32(v); }
inline void Store16(uint32_t a, uint16_t v) { *(uint16_t*)(g_base + a) = __builtin_bswap16(v); }
inline uint16_t Load16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(g_base + a)); }
inline void Store64(uint32_t a, uint64_t v) { *(uint64_t*)(g_base + a) = __builtin_bswap64(v); }

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)

// Import implementado a mano: GUEST_IMPL(NtClose) { ... } -> define __imp__NtClose
#define GUEST_IMPL(name) PPC_FUNC(__imp__##name)

// Direcciones del XEX (de NOTAS_SESION1.md)
constexpr uint32_t kXexHeaderGuest = 0x82BF0000; // copia de las cabeceras XEX dentro de memoria (zona libre tras .reloc)
constexpr uint32_t kEntryPoint = 0x822CB0A0;

// PCR / hilo actual
struct GuestThreadState
{
    uint32_t pcr;
    uint32_t tls;
    uint32_t thread;
    uint32_t stackBase;
    uint32_t stackLimit;
};
extern thread_local GuestThreadState t_thread;
void InitGuestThread(PPCContext& ctx, uint32_t stackSize);
void InitKernelVariables();
void RegisterGuestThread(PPCContext* ctx, uint32_t entry);
void StartDiagnostics();
void InitXexHeader(const uint8_t* xex, size_t size);
uint32_t XexHeaderField(uint32_t key);

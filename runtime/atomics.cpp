#include <cstdlib>
// Reserva de lwarx/stwcx (ver compat/ppc_indirect.h). Tabla de versiones por cubetas de 8 bytes.
#include <atomic>
#include <cstdint>
#include <windows.h>

namespace
{
constexpr uint32_t kBuckets = 1u << 14;
std::atomic<uint32_t> g_version[kBuckets];
std::atomic<uint8_t> g_lock[kBuckets];
thread_local uint32_t t_ea = 0xFFFFFFFF, t_version = 0;
inline uint32_t Bucket(uint32_t ea) { return (ea >> 3) & (kBuckets - 1); }
inline void Lock(uint32_t b) { for (int n = 0; g_lock[b].exchange(1, std::memory_order_acquire); n++) if (n > 64) SwitchToThread(); else YieldProcessor(); }
inline void Unlock(uint32_t b) { g_lock[b].store(0, std::memory_order_release); }

template<typename T>
bool StoreConditional(uint8_t* base, uint32_t ea, T expected, T value)
{
    if (t_ea != ea) return false;
    t_ea = 0xFFFFFFFF;
    uint32_t b = Bucket(ea);
    Lock(b);
    bool ok = g_version[b].load(std::memory_order_relaxed) == t_version &&
              __sync_bool_compare_and_swap(reinterpret_cast<T*>(base + ea), expected, value);
    if (ok) g_version[b].fetch_add(1, std::memory_order_relaxed);
    Unlock(b);
    return ok;
}
} // namespace

void ppc_reserve(uint32_t ea) { t_ea = ea; t_version = g_version[Bucket(ea)].load(std::memory_order_acquire); }
bool ppc_stwcx32(uint8_t* base, uint32_t ea, uint32_t expectedRaw, uint32_t newRaw) { return StoreConditional<uint32_t>(base, ea, expectedRaw, newRaw); }
bool ppc_stdcx64(uint8_t* base, uint32_t ea, uint64_t expectedRaw, uint64_t newRaw) { return StoreConditional<uint64_t>(base, ea, expectedRaw, newRaw); }

// Time base de la Xbox 360. Sesion 25 (desfase de las notas): en la consola real va a 49,875 MHz aunque KeQueryPerformanceFrequency
// diga 50 MHz, y el juego lo sabe: al arrancar (sub_823A0288) cambia 50000000 por 49875000 y divide el mftb entre eso para el reloj
// de la cancion. Con el mftb a 50 MHz exactos ese reloj iba un 0,25 % rapido: las notas se adelantaban a la musica ~0,6 s al final
// de una cancion de 4 minutos. GH6_TB50=1 = 50 MHz (antes).
uint64_t ppc_mftb()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static const uint64_t tb = getenv("GH6_TB50") ? 50000000ull : 49875000ull;
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t t = (uint64_t)c.QuadPart, f = (uint64_t)freq.QuadPart;
    return (t / f) * tb + (t % f) * tb / f;   // sin division de 128 bits
}

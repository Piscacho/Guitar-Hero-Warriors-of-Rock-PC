// Ganchos sobre funciones del juego (las funciones traducidas son "weak": definirlas aqui las reemplaza).
// Sesion 6: diagnostico del planificador de tareas (job system).
#include "runtime.h"
#include <ppc_recomp_shared.h>
#include <mutex>
#include <windows.h>
#include <atomic>
#include <set>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_826469B8);

namespace
{
std::mutex g_mutex;
std::set<uint64_t> g_seen;
bool g_jobLog = getenv("GH6_JOBLOG") != nullptr;
constexpr uint32_t kJobGlobals = 0x8286B210;
}

// sub_826469B8(grupo) -> 1 si todas las dependencias del grupo estan a 0
PPC_FUNC(sub_826469B8)
{
    uint32_t group = ctx.r3.u32;
    static bool first = true;
    if (first && g_jobLog) { first = false; LOG("[jobs] gancho sub_826469B8 activo"); }
    __imp__sub_826469B8(ctx, base);
    if (!g_jobLog || ctx.r3.u32 != 0) return;
    uint32_t g = Load32(kJobGlobals), table = Load32(g + 260);
    uint32_t e = table + group * 32, ndeps = Load32(e + 4);
    for (uint32_t i = 0; i < ndeps && i < 6; i++)
    {
        uint32_t d = Load32(e + 8 + i * 4), de = table + d * 32;
        uint32_t c0 = Load32(de), c28 = Load32(de + 28);
        if (c0 + c28 == 0) continue;
        std::lock_guard lock(g_mutex);
        if (g_seen.insert((uint64_t(group) << 32) | d).second)
            LOG("[jobs] grupo %u bloqueado por grupo %u (contadores %d / %d; entrada 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X)",
                group, d, (int)c0, (int)c28, de, Load32(de), Load32(de + 4), Load32(de + 8), Load32(de + 12), Load32(de + 16), Load32(de + 20), Load32(de + 24), Load32(de + 28));
    }
}

// Volcado periodico del estado de las colas (GH6_JOBLOG): contadores globales y colas 0..2
void DumpJobState()
{
    uint32_t g = Load32(kJobGlobals);
    if (!g) return;
    LOG("[jobs] pendientes +248=%d +252=%d  colas: %d %d %d  (globals=0x%08X)", (int)Load32(g + 248), (int)Load32(g + 252),
        (int)Load32(g + 232), (int)Load32(g + 236), (int)Load32(g + 240), g);
}

// Sesion 12: diagnostico del deserializador sub_8239A0C0 (GH6_PARSELOG=1)
extern "C" PPC_FUNC(__imp__sub_8239A0C0);
PPC_FUNC(sub_8239A0C0)
{
    static bool on = getenv("GH6_PARSELOG") != nullptr;
    if (on)
    {
        static std::atomic<int> n{ 0 };
        int k = n++;
        if (k < 6 || k % 200 == 0)
        {
            uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
            uint32_t w0 = (a >= 0x10000 && a < 0xF0000000) ? Load32(a) : 0, w1 = (a >= 0x10000 && a < 0xF0000000) ? Load32(a + 4) : 0;
            LOG("[parse] #%d r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X datos %08X %08X", k, a, b, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr, w0, w1);
            if (b >= 0x10000 && b < 0xF0000000) { char t[256]; int q = 0; for (int j = 0; j < 16; j++) q += snprintf(t + q, sizeof(t) - q, " %08X", Load32(b - 8 + j * 4)); LOG("[parse]   r4-8:%s", t); }
        }
    }
    __imp__sub_8239A0C0(ctx, base);
}

// Sesion 15: datos XMA disponibles por voz segun FMOD (GH6_XMAVOICELOG=1)
extern "C" PPC_FUNC(__imp__sub_82769300);
PPC_FUNC(sub_82769300)
{
    uint32_t set = ctx.r3.u32, voice = ctx.r4.u32;
    __imp__sub_82769300(ctx, base);
    static bool on = getenv("GH6_XMAVOICELOG") != nullptr;
    if (!on) return;
    static DWORD last[16] = {};
    if (voice < 16 && GetTickCount() - last[voice] > 2000)
    {
        last[voice] = GetTickCount();
        uint32_t entries = Load32(set + 8), e = entries + voice * 96;
        LOG("[fmodxma] voz %u: disponible %u muestras | id %04X consumido %u | flags %08X | copia d0 %08X d1 %08X d9 %08X | hw %08X",
            voice, ctx.r3.u32, __builtin_bswap16(*(uint16_t*)(g_base + e + 80)), __builtin_bswap16(*(uint16_t*)(g_base + e + 82)),
            Load32(set + 4), Load32(e), Load32(e + 4), Load32(e + 36), Load32(e + 64));
    }
}

// Sesion 15: "commit" de voces XMA de FMOD (copia las voces al hardware y hace kick de cada una).
// Tras la funcion original, marcamos como lanzados los contextos de todas las voces del conjunto (id en +80, 96 bytes por voz).
void XmaKickContext(uint32_t id);
extern "C" PPC_FUNC(__imp__sub_827691D8);
PPC_FUNC(sub_827691D8)
{
    uint32_t set = ctx.r3.u32;
    __imp__sub_827691D8(ctx, base);
    uint32_t count = Load32(set), entries = Load32(set + 8);
    for (uint32_t v = 0; v < count && v < 64; v++)
    {
        uint16_t id = __builtin_bswap16(*(uint16_t*)(g_base + entries + v * 96 + 80));
        XmaKickContext(id & 0x1FF);
    }
}

// Sesion 15: contadores del codec XMA de FMOD (GH6_FMODLOG=1)
namespace
{
std::atomic<uint64_t> g_cRead{ 0 }, g_cNeed{ 0 }, g_cNeedYes{ 0 }, g_cFeed{ 0 }, g_cConsume{ 0 }, g_cFileRead{ 0 };
std::atomic<uint32_t> g_lastReadRet{ 0 }, g_lastFileRet{ 0 };
void FmodLog()
{
    static DWORD last = 0;
    if (!getenv("GH6_FMODLOG") || GetTickCount() - last < 2000) return;
    last = GetTickCount();
    LOG("[fmod] lecturas codec %llu (ultimo ret %u) | necesita-entrada %llu/%llu | entrega %llu | consumo %llu | lecturas archivo %llu (ret %u)",
        (unsigned long long)g_cRead.load(), g_lastReadRet.load(), (unsigned long long)g_cNeedYes.load(), (unsigned long long)g_cNeed.load(),
        (unsigned long long)g_cFeed.load(), (unsigned long long)g_cConsume.load(), (unsigned long long)g_cFileRead.load(), g_lastFileRet.load());
}
}
extern "C" PPC_FUNC(__imp__sub_8233A9D0);
PPC_FUNC(sub_8233A9D0) { g_cRead++; __imp__sub_8233A9D0(ctx, base); g_lastReadRet = ctx.r3.u32; FmodLog(); }
extern "C" PPC_FUNC(__imp__sub_82768F30);
PPC_FUNC(sub_82768F30) { __imp__sub_82768F30(ctx, base); g_cNeed++; if (ctx.r3.u32) g_cNeedYes++; }
extern "C" PPC_FUNC(__imp__sub_82769680);
PPC_FUNC(sub_82769680) { g_cFeed++; __imp__sub_82769680(ctx, base); }
extern "C" PPC_FUNC(__imp__sub_82769390);
PPC_FUNC(sub_82769390) { g_cConsume++; __imp__sub_82769390(ctx, base); }
extern "C" PPC_FUNC(__imp__sub_82312820);
PPC_FUNC(sub_82312820) { g_cFileRead++; __imp__sub_82312820(ctx, base); g_lastFileRet = ctx.r3.u32; }

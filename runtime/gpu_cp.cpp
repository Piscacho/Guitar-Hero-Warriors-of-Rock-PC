// Procesador de comandos de la GPU Xenos (sesion 5).
// Lee el ring buffer (paquetes PM4) y ejecuta lo que afecta a la CPU: registros, escrituras a memoria (fences),
// esperas, interrupciones y swaps. Los draws solo se cuentan (el dibujo real llega en sesiones posteriores).
// Opcodes y registros segun la documentacion de Xenia (BSD) / AMD R6xx.
#include "objects.h"
#include "gpu.h"
#include <string>
extern std::string g_swapResolves;   // gpu_d3d12.cpp
#include <atomic>
extern std::atomic<uint64_t> g_vblankCount;   // gpu_stub.cpp
#include "gpu_draw.h"
#include <ppc_recomp_shared.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <windows.h>

namespace
{
// ---- shaders y draws (sesion 6) ----
std::unordered_set<uint64_t> g_shaderHashes;
std::atomic<uint32_t> g_vsCount{ 0 }, g_psCount{ 0 };
std::atomic<uint64_t> g_primCount[64];
bool g_dumpShaders = getenv("GH6_NODUMPSHADERS") == nullptr;
uint64_t Fnv1a(const uint32_t* d, uint32_t n) { uint64_t h = 1469598103934665603ull; for (uint32_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h; }
std::vector<uint32_t> g_activeCode[2];
std::atomic<uint64_t> g_flushByOp[128];
static const bool g_waitLog = getenv("GH6_WAITLOG") != nullptr;
uint64_t g_activeHash[2] = {};
bool g_drawsEnabled = getenv("GH6_NODRAW") == nullptr;
void CaptureShader(uint32_t type, const uint32_t* words, uint32_t count)
{
    g_activeCode[type & 1].assign(words, words + count);
    g_activeHash[type & 1] = Fnv1a(words, count) ^ (uint64_t(type) << 63);
    uint64_t h = Fnv1a(words, count) ^ (uint64_t(type) << 63);
    if (!g_shaderHashes.insert(h).second) return;
    (type ? g_psCount : g_vsCount)++;
    if (!g_dumpShaders) return;
    CreateDirectoryA("shaders", nullptr);
    char name[128]; snprintf(name, sizeof(name), "shaders/%s_%016llX.bin", type ? "ps" : "vs", (unsigned long long)h);
    if (FILE* f = fopen(name, "wb")) { for (uint32_t i = 0; i < count; i++) { uint32_t be = __builtin_bswap32(words[i]); fwrite(&be, 4, 1, f); } fclose(f); }
}
constexpr uint32_t kRegBase = 0x7FC80000;   // registros MMIO de la GPU (memoria plana de invitado)
enum : uint32_t
{
    REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD, REG_CP_RB_RPTR = 0x01C4, REG_CP_RB_WPTR = 0x01C5,
    REG_SCRATCH_REG0 = 0x0578, REG_COHER_STATUS_HOST = 0x0A2F,
};
enum : uint32_t
{
    PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23, PM4_WAIT_FOR_IDLE = 0x26,
    PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B, PM4_SET_CONSTANT = 0x2D, PM4_LOAD_ALU_CONSTANT = 0x2F,
    PM4_DRAW_INDX_2 = 0x36, PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B, PM4_WAIT_REG_MEM = 0x3C,
    PM4_MEM_WRITE = 0x3D, PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45,
    PM4_EVENT_WRITE = 0x46, PM4_ME_INIT = 0x48, PM4_INTERRUPT = 0x54, PM4_SET_CONSTANT2 = 0x55,
    PM4_SET_SHADER_CONSTANTS = 0x56, PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x59, PM4_EVENT_WRITE_ZPD = 0x5B,
    PM4_CONTEXT_UPDATE = 0x5E, PM4_XE_SWAP = 0x64, // XE_SWAP: paquete propio que escribe VdSwap (como Xenia)
};

std::atomic<uint32_t> g_ringPhys{ 0 }, g_ringDwords{ 0 }, g_writebackPhys{ 0 };

std::atomic<uint32_t> g_irqCallback{ 0 }, g_irqUserData{ 0 };
std::atomic<bool> g_started{ false };
std::mutex g_irqMutex;
GpuStats g_stats;
uint32_t g_rptr = 0;

inline uint32_t Phys(uint32_t p) { return 0xA0000000u | (p & 0x1FFFFFFFu); }
inline uint32_t Reg(uint32_t i) { return Load32(kRegBase + i * 4); }
inline uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); // 8in16
    case 2: return __builtin_bswap32(v);                                // 8in32
    case 3: return (v << 16) | (v >> 16);                               // 16in32
    default: return v;
    }
}
// Escritura de la GPU a memoria: dato en orden "host" tras GpuSwap (igual que Xenia)
inline void GpuStore(uint32_t addrWithEndian, uint32_t v)
{
    *(uint32_t*)(g_base + Phys(addrWithEndian & ~3u)) = GpuSwap(v, addrWithEndian & 3);
}
inline uint32_t GpuLoad(uint32_t addrWithEndian) { return GpuSwap(*(uint32_t*)(g_base + Phys(addrWithEndian & ~3u)), addrWithEndian & 3); }

void WriteRegister(uint32_t index, uint32_t value)
{
    index &= 0xFFFF;
    if (index == REG_COHER_STATUS_HOST) value &= ~0x80000000u; // coherencia completada al instante
    Store32(kRegBase + index * 4, value);
    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
        if (Reg(REG_SCRATCH_UMSK) & (1u << n))
        {
            if (D3D12FlushPendingResolves()) g_flushByOp[0x7F]++;   // valla de D3D: la CPU puede leer ya lo resuelto (sesion 24)
            Store32(Phys(Reg(REG_SCRATCH_ADDR) + n * 4), value); // fences de D3D via registros scratch
        }
    }
}

bool Compare(uint32_t func, uint32_t v, uint32_t ref)
{
    switch (func & 7)
    {
    case 0: return false; case 1: return v < ref; case 2: return v <= ref; case 3: return v == ref;
    case 4: return v != ref; case 5: return v >= ref; case 6: return v > ref; default: return true;
    }
}

PPCContext* g_gpuCtx = nullptr;
bool g_trace = getenv("GH6_PM4TRACE") != nullptr;
} // namespace

// Llama a la rutina de interrupciones del juego (source 0 = VBlank, 1 = command buffer) "en" la CPU indicada
void GpuDispatchInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu)
{
    uint32_t cb = g_irqCallback.load();
    if (!cb) return;
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, cb);
    if (!fn) return;
    std::lock_guard lock(g_irqMutex);
    g_base[t_thread.pcr + 0x10C] = (uint8_t)cpu; // KPCR.current_cpu
    uint64_t r1 = ctx.r1.u64, r13 = ctx.r13.u64;
    ctx.r3.u64 = source; ctx.r4.u64 = g_irqUserData.load();
    fn(ctx, g_base);
    ctx.r1.u64 = r1; ctx.r13.u64 = r13;
}
std::atomic<uint64_t> g_cpWaitUs{ 0 };   // tiempo total del procesador de comandos en WAIT_REG_MEM (para el log de tirones)
void GpuSetInterruptCallback(uint32_t cb, uint32_t user) { g_irqCallback = cb; g_irqUserData = user; }
void GpuSetRingBuffer(uint32_t phys, uint32_t sizeLog2) { g_ringPhys = phys; g_ringDwords = (1u << (sizeLog2 + 3)) / 4; }
void GpuSetReadPointerWriteback(uint32_t phys) { g_writebackPhys = phys; }
const GpuStats& GpuGetStats() { return g_stats; }

namespace
{
struct Reader
{
    uint32_t base, sizeDw, pos; // pos en dwords (circular si sizeDw != 0)
    uint32_t Read() { uint32_t a = base + (sizeDw ? (pos % sizeDw) : pos) * 4; pos++; return Load32(Phys(a)); }
};

void ExecuteRange(Reader& rd, uint32_t endPos, int depth);

bool ExecutePacket(Reader& rd, int depth)
{
    uint32_t h = rd.Read();
    switch (h >> 30)
    {
    case 0:
    {
        uint32_t base = h & 0x7FFF, count = ((h >> 16) & 0x3FFF) + 1; bool one = (h >> 15) & 1;
        for (uint32_t i = 0; i < count; i++) WriteRegister(one ? base : base + i, rd.Read());
        return true;
    }
    case 1:
    {
        uint32_t r1 = h & 0x7FF, r2 = (h >> 11) & 0x7FF;
        WriteRegister(r1, rd.Read()); WriteRegister(r2, rd.Read());
        return true;
    }
    case 2: return true;
    default: break;
    }
    uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
    uint32_t start = rd.pos;
    static int traced = 0;
    if (g_trace && traced < 400) { traced++; LOG("[pm4] d%d op=0x%02X n=%u  %08X %08X %08X %08X", depth, op, count, Load32(Phys(rd.base + (rd.sizeDw ? (start % rd.sizeDw) : start) * 4)), Load32(Phys(rd.base + (rd.sizeDw ? ((start + 1) % rd.sizeDw) : start + 1) * 4)), Load32(Phys(rd.base + (rd.sizeDw ? ((start + 2) % rd.sizeDw) : start + 2) * 4)), Load32(Phys(rd.base + (rd.sizeDw ? ((start + 3) % rd.sizeDw) : start + 3) * 4))); }
    // Sesion 24: paquetes cuyo efecto puede observar la CPU del juego: antes, completar los resolves por CPU pendientes
    switch (op)
    {
    // (WAIT_REG_MEM no: D3D lo emite tras cada resolve para esperar a la propia GPU y la CPU no observa nada)
    case PM4_COND_WRITE: case PM4_MEM_WRITE: case PM4_REG_TO_MEM: case PM4_EVENT_WRITE_SHD:
    case PM4_EVENT_WRITE_EXT: case PM4_INTERRUPT: case PM4_XE_SWAP: case PM4_EVENT_WRITE_ZPD:
        if (D3D12FlushPendingResolves()) g_flushByOp[op & 0x7F]++;
        break;
    default: break;
    }
    switch (op)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
    {
        uint32_t ptr = rd.Read(), len = rd.Read() & 0xFFFFF;
        if (depth < 4) { Reader ib{ ptr & ~3u, 0, 0 }; ExecuteRange(ib, len, depth + 1); }
        break;
    }
    case PM4_WAIT_REG_MEM:
    {
        uint32_t info = rd.Read(), addr = rd.Read(), ref = rd.Read(), mask = rd.Read(); rd.Read(); // wait
        {   // sesion 24: tipos de espera (diagnostico) y, si lee memoria, completar antes los resolves diferidos
            static std::atomic<int> wl{ 0 };
            if (wl < 30 && g_waitLog) { wl++; LOG("[waitregmem] info=%X addr=%08X ref=%X mask=%X", info, addr, ref, mask); }
            if ((info & 0x10) && D3D12FlushPendingResolves()) g_flushByOp[0x3C]++;
        }
        LARGE_INTEGER wt0; QueryPerformanceCounter(&wt0);
        for (int spins = 0;; spins++)
        {
            uint32_t v = (info & 0x10) ? GpuLoad(addr) : Reg(addr & 0xFFFF);
            if (Compare(info, v & mask, ref))
            {
                if (spins > 100) { LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f); g_cpWaitUs += (uint64_t)(1e6 * (t1.QuadPart - wt0.QuadPart) / f.QuadPart); }
                if (spins > 100 && g_waitLog)
                {   // diagnostico: esperas largas (GH6_WAITLOG)
                    LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
                    double ms = 1000.0 * (t1.QuadPart - wt0.QuadPart) / f.QuadPart;
                    static std::atomic<int> ll{ 0 };
                    if (ms > 20.0 && ll++ < 400) LOG("[waitlarga] %.1f ms info=%X addr=%08X ref=%X mask=%X v=%X swap %llu", ms, info, addr, ref, mask, v, (unsigned long long)g_stats.swaps.load());
                }
                break;
            }
            if (spins > 200000) { LOG("[gpu] WAIT_REG_MEM sin cumplirse (info=%X addr=%08X ref=%X mask=%X v=%X)", info, addr, ref, mask, v); break; }
            if (spins > 100) Sleep(0);
        }
        break;
    }
    case PM4_REG_RMW:
    {
        uint32_t info = rd.Read(), andm = rd.Read(), orm = rd.Read();
        uint32_t v = Reg(info & 0x1FFF);
        v &= (info >> 31) ? Reg(andm & 0x1FFF) : andm;
        v |= ((info >> 30) & 1) ? Reg(orm & 0x1FFF) : orm;
        WriteRegister(info & 0x1FFF, v);
        break;
    }
    case PM4_COND_WRITE:
    {
        uint32_t info = rd.Read(), addr = rd.Read(), ref = rd.Read(), mask = rd.Read(), waddr = rd.Read(), wdata = rd.Read();
        uint32_t v = (info & 0x10) ? GpuLoad(addr) : Reg(addr & 0xFFFF);
        if (Compare(info, v & mask, ref)) { if (info & 0x100) GpuStore(waddr, wdata); else WriteRegister(waddr, wdata); }
        break;
    }
    case PM4_MEM_WRITE:
    {
        uint32_t addr = rd.Read();
        for (uint32_t i = 1; i < count; i++) { GpuStore(addr, rd.Read()); addr = ((addr & ~3u) + 4) | (addr & 3); }
        break;
    }
    case PM4_REG_TO_MEM:
    {
        uint32_t reg = rd.Read(), addr = rd.Read();
        GpuStore(addr, Reg(reg & 0xFFFF));
        break;
    }
    case PM4_EVENT_WRITE_SHD:
    {
        uint32_t initiator = rd.Read(), addr = rd.Read(), value = rd.Read();
        GpuStore(addr, (initiator >> 31) ? (uint32_t)g_stats.swaps : value);
        break;
    }
    case PM4_EVENT_WRITE_EXT:
    {
        rd.Read(); uint32_t addr = rd.Read();
        static const uint16_t extents[] = { 0, 0x2000, 0, 0x2000, 0, 0x2000 }; // "todo el viewport" (como Xenia)
        for (int i = 0; i < 6; i++) *(uint16_t*)(g_base + Phys(addr & ~3u) + i * 2) = __builtin_bswap16(extents[i]);
        break;
    }
    case PM4_INTERRUPT:
    {
        uint32_t mask = rd.Read();
        for (uint32_t cpu = 0; cpu < 6; cpu++) if (mask & (1u << cpu)) GpuDispatchInterrupt(*g_gpuCtx, 1, cpu);
        g_stats.interrupts++;
        break;
    }
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
    {
        if (op == PM4_DRAW_INDX) rd.Read(); // viz query
        uint32_t init = rd.Read();
        XenosDraw d;
        d.primType = init & 0x3F; d.source = (init >> 6) & 3; d.index32 = (init >> 11) & 1; d.numIndices = init >> 16;
        if (op == PM4_DRAW_INDX && d.source == 0)
        {
            d.indexAddr = rd.Read(); uint32_t sz = rd.Read();
            d.indexEndian = (sz >> 30) & 3;
        }
        else if (d.source == 1)
        {
            uint32_t remaining = count - 1;
            for (uint32_t i = 0; i < remaining && d.immediate.size() < d.numIndices; i++)
            {
                uint32_t v = rd.Read();
                if (d.index32) d.immediate.push_back(v);
                else { d.immediate.push_back(v & 0xFFFF); if (d.immediate.size() < d.numIndices) d.immediate.push_back(v >> 16); }
            }
        }
        g_primCount[d.primType]++;
        g_stats.draws++;
        if (g_drawsEnabled && !g_activeCode[0].empty() && !g_activeCode[1].empty())
        {
            d.vsCode = g_activeCode[0].data(); d.vsCount = (uint32_t)g_activeCode[0].size(); d.vsHash = g_activeHash[0];
            d.psCode = g_activeCode[1].data(); d.psCount = (uint32_t)g_activeCode[1].size(); d.psHash = g_activeHash[1];
            D3D12Draw(d);
        }
        break;
    }
    // ---- constantes: escritas en el archivo de registros (ALU 0x4000, fetch 0x4800, bool 0x4900, loop 0x4908, regs 0x2000) ----
    case PM4_SET_CONSTANT:
    {
        uint32_t ot = rd.Read(), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
        static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        if (type < 5) for (uint32_t i = 1; i < count; i++) WriteRegister(bases[type] + index + i - 1, rd.Read());
        break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
    {
        uint32_t index = rd.Read() & 0xFFFF;
        for (uint32_t i = 1; i < count; i++) WriteRegister(index + i - 1, rd.Read());
        break;
    }
    case PM4_LOAD_ALU_CONSTANT:
    {
        uint32_t addr = rd.Read() & 0x3FFFFFFF, ot = rd.Read(), size = rd.Read() & 0xFFF;
        uint32_t index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
        static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        if (type < 5) for (uint32_t i = 0; i < size; i++) WriteRegister(bases[type] + index + i, Load32(Phys(addr + i * 4)));
        break;
    }
    // ---- microcodigo de shaders ----
    case PM4_IM_LOAD:
    {
        uint32_t at = rd.Read(), ss = rd.Read();
        uint32_t addr = at & ~3u, type = at & 3, size = ss & 0xFFFF;
        std::vector<uint32_t> w(size);
        for (uint32_t i = 0; i < size; i++) w[i] = Load32(Phys(addr + i * 4));
        {   // Sesion 24: microcodigo casi vacio (posible carrera con la CPU que lo escribe): se registra
            uint32_t nz = 0; for (uint32_t i = 0; i < size; i++) nz += w[i] != 0;
            static int zlog = 0;
            if (size >= 8 && nz * 4 < size && zlog < 30) { zlog++; LOG("[shader] IM_LOAD casi vacio: tipo %u dir %08X %u dwords, %u no nulos (swap %llu)", type, addr, size, nz, (unsigned long long)g_stats.swaps.load()); }
        }
        CaptureShader(type, w.data(), size);
        break;
    }
    case PM4_IM_LOAD_IMMEDIATE:
    {
        uint32_t type = rd.Read() & 3, ss = rd.Read(), size = ss & 0xFFFF;
        std::vector<uint32_t> w(size);
        Reader start = rd;
        for (uint32_t i = 0; i < size; i++) w[i] = rd.Read();
        // Sesion 24: el juego rellena el microcodigo dentro del bufer de comandos despues de enviarlo (la GPU real va con retraso).
        // Diagnostico (GH6_IMWAIT=1): si llega casi vacio, espera y relee. Resultado: los datos no llegan nunca (no es una carrera).
        static bool noImWait = getenv("GH6_IMWAIT") == nullptr;
        auto nonZero = [&]() { uint32_t nz = 0; for (uint32_t i = 0; i < size; i++) nz += w[i] != 0; return nz; };
        if (!noImWait && size >= 8 && nonZero() * 4 < size)
        {
            uint32_t prev = nonZero(), stable = 0;
            for (int t = 0; t < 200 && stable < 3; t++)
            {
                Sleep(1);
                Reader r2 = start; for (uint32_t i = 0; i < size; i++) w[i] = r2.Read();
                uint32_t nz = nonZero();
                if (nz == prev && nz * 4 >= size) stable++; else stable = 0;
                prev = nz;
            }
            static int wlog = 0;
            if (wlog < 20) { wlog++; LOG("[shader] IM_LOAD_IMMEDIATE releido: %u dwords, %u no nulos", size, prev); }
        }
        {
            uint32_t nz = 0; for (uint32_t i = 0; i < size; i++) nz += w[i] != 0;
            static int zlog = 0;
            static uint64_t imTotal = 0, imEmpty = 0; imTotal++; if (size >= 8 && nz * 4 < size) imEmpty++;
            if ((imTotal & (imTotal - 1)) == 0) LOG("[shader] IM_LOAD_IMMEDIATE: %llu en total, %llu casi vacios", (unsigned long long)imTotal, (unsigned long long)imEmpty);
            if (size >= 8 && nz * 4 < size && zlog < 30) { zlog++; LOG("[shader] IM_LOAD_IMMEDIATE casi vacio: tipo %u, %u dwords, %u no nulos, cabecera %08X (swap %llu)", type, size, nz, ss, (unsigned long long)g_stats.swaps.load()); }
        }
        CaptureShader(type, w.data(), size);
        break;
    }
    case PM4_XE_SWAP:
    {
        rd.Read(); // 'SWAP'
        uint32_t fbNew = rd.Read();
        static uint32_t fbLast = 0; static int fbChanges = 0;
        if (fbNew != fbLast && fbChanges < 20) { fbChanges++; LOG("[gpu] XE_SWAP frontbuffer 0x%08X (swap %llu)", fbNew, (unsigned long long)g_stats.swaps.load()); }
        { static long long logFrom = getenv("GH6_SWAPLOG") ? atoll(getenv("GH6_SWAPLOG")) : -1; long long s = (long long)g_stats.swaps.load(); if (logFrom >= 0 && s >= logFrom && s < logFrom + 12) LOG("[swap] %lld fb %08X | resolves: %s", s, fbNew, g_swapResolves.c_str()); g_swapResolves.clear(); }
        fbLast = fbNew;
        uint32_t wNew = rd.Read(), hNew = rd.Read();
        g_stats.swaps++;
        // Sesion 23: en la consola solo se ve lo que hay en el frontbuffer en cada vsync. El juego hace a veces dos swaps por frame
        // (el primero apunta a la escena sin postproceso): se presenta solo el ultimo swap antes de cada vsync emulado.
        static bool swapAll = getenv("GH6_SWAPALL") != nullptr;
        static bool havePending = false; static uint32_t pFb = 0, pW = 0, pH = 0; static uint64_t pVb = 0;
        uint64_t vb = g_vblankCount.load();
        if (swapAll) { g_stats.frontbuffer = fbNew; g_stats.width = wNew; g_stats.height = hNew; GpuPresent(); break; }
        if (havePending && vb != pVb) { g_stats.frontbuffer = pFb; g_stats.width = pW; g_stats.height = pH; GpuPresent(); }
        havePending = true; pFb = fbNew; pW = wNew; pH = hNew; pVb = vb;
        break;
    }
    // Sesion 24: consultas de oclusion (como Xenia). D3D escribe 0xFFFFFEED en ZPass_A/B (o ZFail_A/B) al terminar la consulta
    // y espera el numero de muestras en la estructura de RB_SAMPLE_COUNT_ADDR (0x2325, en little endian).
    // Se informa de un numero fijo de muestras visibles (GH6_ZPDSAMPLES, por defecto 1000; -1 = no tocar).
    case PM4_EVENT_WRITE_ZPD:
    {
        rd.Read(); // initiator
        static int fake = getenv("GH6_ZPDSAMPLES") ? atoi(getenv("GH6_ZPDSAMPLES")) : 1000;
        uint32_t addr = Reg(0x2325);
        if (fake >= 0 && addr)
        {
            uint32_t* sc = (uint32_t*)(g_base + Phys(addr & ~3u));
            const uint32_t kFinished = 0xEDFEFFFFu; // 0xFFFFFEED escrito en big endian, leido en little endian
            bool endZPass = sc[4] == kFinished && sc[5] == kFinished, endZFail = sc[2] == kFinished && sc[3] == kFinished;
            memset(sc, 0, 32);
            if (endZPass || endZFail) { sc[4] = (uint32_t)fake; sc[0] = (uint32_t)fake; }
            static int logged = 0; if (logged < 3) { logged++; LOG("[gpu] EVENT_WRITE_ZPD en %08X (fin %d)", addr, endZPass || endZFail); }
        }
        break;
    }
    case PM4_VIZ_QUERY:
    {
        uint32_t dw0 = rd.Read(), id = dw0 & 0x3F;
        if ((dw0 & 0x100) && id < 32) WriteRegister(0x0C44 /*PA_SC_VIZ_QUERY_STATUS_0*/, Reg(0x0C44) | (1u << id));
        break;
    }
    default:
        break; // NOP, ME_INIT, constantes, estado... se ignoran por ahora
    }
    rd.pos = start + count; // saltar siempre al final del paquete
    g_stats.packets++;
    return true;
}

void ExecuteRange(Reader& rd, uint32_t endPos, int depth)
{
    while (rd.pos != endPos) ExecutePacket(rd, depth);
}

void CommandProcessorThread()
{
    g_gpuCtx = new PPCContext{};
    InitGuestThread(*g_gpuCtx, 0x10000);
    RegisterGuestThread(g_gpuCtx, 0xC0C0C0C0);   // para el muestreo de pilas (GH6_SAMPLE): "entrada" ficticia del procesador de comandos
    DWORD lastLog = GetTickCount();
    for (;;)
    {
        if (GetTickCount() - lastLog > 2000)
        {
            lastLog = GetTickCount();
            LOG("[gpu] rptr=%u wptr=%u paquetes=%llu draws=%llu irq=%llu swaps=%llu shaders vs=%u ps=%u", g_rptr, Reg(REG_CP_RB_WPTR), (unsigned long long)g_stats.packets.load(),
                (unsigned long long)g_stats.draws.load(), (unsigned long long)g_stats.interrupts.load(), (unsigned long long)g_stats.swaps.load(), g_vsCount.load(), g_psCount.load());
            char prims[256]; int k = 0;
            for (int i = 0; i < 64; i++) if (uint64_t c = g_primCount[i].load()) k += snprintf(prims + k, sizeof(prims) - k, " tipo%d=%llu", i, (unsigned long long)c);
            if (k) LOG("[gpu] primitivas:%s", prims);
            {   // sesion 24: que paquetes obligan a completar resolves diferidos
                char fl[256]; int q = 0;
                for (int i = 0; i < 128; i++) if (uint64_t c2 = g_flushByOp[i].exchange(0)) q += snprintf(fl + q, sizeof(fl) - q, " 0x%02X=%llu", i, (unsigned long long)c2);
                if (q) LOG("[gpu] vaciados de resolves por paquete:%s", fl);
            }
        }
        uint32_t ringPhys = g_ringPhys.load(), size = g_ringDwords.load();
        if (!ringPhys || !size) { Sleep(1); continue; }
        uint32_t wptr = Reg(REG_CP_RB_WPTR) % size;
        if (g_rptr == wptr) { Sleep(0); continue; }
        Reader rd{ ringPhys, size, g_rptr };
        uint32_t end = wptr >= g_rptr ? wptr : wptr + size;
        ExecuteRange(rd, end, 0);
        D3D12FlushPendingResolves();   // fin del lote: la memoria queda al dia antes de que la CPU vea el nuevo rptr
        g_rptr = rd.pos % size;
        Store32(kRegBase + REG_CP_RB_RPTR * 4, g_rptr);
        if (uint32_t wb = g_writebackPhys.load()) Store32(Phys(wb), g_rptr);
    }
}
} // namespace

void GpuStartCommandProcessor()
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    // GH6_WATCH=<dir hex>: hilo que sondea ese dword de la memoria del juego y registra cada cambio con el numero de draw de
    // la GPU y de swap (diagnostico, sesion 24)
    // GH6_WATCHSIZE=<dw2 hex>: hilo que recorre el monton virtual buscando cabeceras de textura (dw0 tipo 2, dw2 = ese tamano)
    // y registra cada cambio de su dw1 (base) con el numero de draw/swap
    if (const char* ws = getenv("GH6_WATCHSIZE"))
    {
        uint32_t size = (uint32_t)strtoul(ws, nullptr, 16);
        std::thread([size] {
            extern void GuestAllocTops(uint32_t& virt, uint32_t& phys);
            std::unordered_map<uint32_t, uint32_t> seen; int n = 0; uint64_t passes = 0;
            LOG("[watch] vigilando cabeceras de tamano %08X", size);
            for (;;)
            {
                uint32_t vt, pt; GuestAllocTops(vt, pt);
                if ((passes++ % 2000) == 0) LOG("[watch] pasada %llu hasta %08X", (unsigned long long)passes, vt);
                static std::vector<uint32_t> bases = [] {
                    std::vector<uint32_t> v; const char* s = getenv("GH6_WATCHBASES");
                    while (s && *s) { v.push_back((uint32_t)strtoul(s, nullptr, 16) & 0x1FFFF000u); s = strchr(s, ','); if (s) s++; }
                    return v;
                }();
                for (uint32_t a = 0x40000000u; a + 12 < vt; a += 4)
                {
                    if ((Load32(a) & 3) != 2) continue;
                    uint32_t v = Load32(a + 4), d2 = Load32(a + 8);
                    bool match = size ? d2 == size : false;
                    for (uint32_t b : bases) if ((v & 0x1FFFF000u) == b && (d2 & 0x1FFF) != 0) match = true;
                    auto it = seen.find(a);
                    if (!match) { if (it != seen.end() && it->second != 0xFFFFFFFFu) { if (n < 4000) { n++; LOG("[watch] cabecera %08X ya no coincide: dw1 %08X dw2 %08X (draw %llu, swap %llu)", a, v, d2, (unsigned long long)g_stats.draws.load(), (unsigned long long)g_stats.swaps.load()); } it->second = 0xFFFFFFFFu; } continue; }
                    if ((it == seen.end() || it->second != v) && n < 4000) { n++; LOG("[watch] cabecera %08X dw1 %08X dw2 %08X (draw %llu, swap %llu)", a, v, d2, (unsigned long long)g_stats.draws.load(), (unsigned long long)g_stats.swaps.load()); seen[a] = v; }
                }
            }
        }).detach();
    }
    if (const char* w = getenv("GH6_WATCH"))
    {
        uint32_t addr = (uint32_t)strtoul(w, nullptr, 16);
        std::thread([addr] {
            uint32_t last = 0; int n = 0;
            // GH6_WATCHGAP=<ms>: solo registra los cambios que llegan tras mas de <ms> sin cambiar (contadores que se atascan)
            static int gap = getenv("GH6_WATCHGAP") ? atoi(getenv("GH6_WATCHGAP")) : 0;
            ULONGLONG lastT = GetTickCount64();
            for (;;)
            {
                uint32_t v = Load32(addr);
                if (v != last && gap)
                {
                    ULONGLONG t = GetTickCount64();
                    if (t - lastT > (ULONGLONG)gap && n < 3000) { n++; LOG("[watch] %08X = %08X tras %llu ms (swap %llu)", addr, v, t - lastT, (unsigned long long)g_stats.swaps.load()); }
                    lastT = t; last = v;
                }
                else if (v != last && n < 3000) { n++; LOG("[watch] %08X = %08X (draw %llu, swap %llu)", addr, v, (unsigned long long)g_stats.draws.load(), (unsigned long long)g_stats.swaps.load()); last = v; }
                for (int i = 0; i < 50; i++) _mm_pause();
            }
        }).detach();
    }
    std::thread(CommandProcessorThread).detach();
}

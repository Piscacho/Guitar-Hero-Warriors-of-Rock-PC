// Diagnostico (sesion 5): registro de hilos de invitado y muestreo de pilas.
// GH6_SAMPLE=<segundos>: tras ese tiempo vuelca 3 veces (1 s de separacion) la pila host y el lr de invitado de cada hilo.
// Las direcciones GH6.exe+0x... se traducen con:  llvm-symbolizer --obj=GH6.exe 0x14xxxxxxx
#include "runtime.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <algorithm>
#include <vector>
#include <thread>
#include <cstdlib>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace
{
struct ThreadInfo { HANDLE h; DWORD tid; PPCContext* ctx; uint32_t entry; };
std::mutex g_mutex;
std::vector<ThreadInfo> g_threads;

void DumpThread(const ThreadInfo& t)
{
    uintptr_t mod = (uintptr_t)GetModuleHandleW(nullptr);
    if (SuspendThread(t.h) == (DWORD)-1) return;
    CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t.h, &c))
    {
        char line[1024]; int n = snprintf(line, sizeof(line), "[sample] tid %lu entrada 0x%08X lr=0x%08X pila host:", t.tid, t.entry, t.ctx ? (uint32_t)t.ctx->lr : 0);
        for (int f = 0; f < 28 && c.Rip; f++)
        {
            n += snprintf(line + n, sizeof(line) - n, " %llX", (unsigned long long)(c.Rip - mod + 0x140000000ull));
            DWORD64 imageBase; auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
            void* handlerData; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
        }
        LOG("%s", line);
    }
    ResumeThread(t.h);
}
} // namespace

void RegisterGuestThread(PPCContext* ctx, uint32_t entry)
{
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lock(g_mutex);
    g_threads.push_back({ h, GetCurrentThreadId(), ctx, entry });
}

// Sesion 24: GH6_WRITEWATCH=<dir hex>[:<bytes hex>]: protege contra escritura la(s) pagina(s) de esa memoria de invitado y registra
// cada escritura dentro del rango con la pila host y el lr de invitado (formato [sample], para tools\symsample.py).
// Tras la instruccion (paso a paso con TF) se vuelve a proteger. Maximo GH6_WRITEWATCHMAX registros (por defecto 300).
namespace
{
uint32_t g_wwAddr = 0, g_wwLen = 0; uintptr_t g_wwPageLo = 0, g_wwPageHi = 0; volatile long g_wwLogged = 0; long g_wwMax = 300;
thread_local bool t_wwStep = false;
void WwProtect(bool on)
{
    DWORD old;
    VirtualProtect((void*)g_wwPageLo, g_wwPageHi - g_wwPageLo, on ? PAGE_READONLY : PAGE_READWRITE, &old);
}
LONG CALLBACK WwHandler(EXCEPTION_POINTERS* ep)
{
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_wwStep)
    {
        t_wwStep = false;
        if (g_wwLogged < g_wwMax) WwProtect(true);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t fa = (uintptr_t)er->ExceptionInformation[1];
    if (fa < g_wwPageLo || fa >= g_wwPageHi) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t guest = (uint32_t)(fa - (uintptr_t)g_base);
    bool fresh = false;
    if (guest + 8 > g_wwAddr && guest < g_wwAddr + g_wwLen)
    {
        // Deduplicado por (instruccion host, lr de invitado)
        static std::mutex m; static std::vector<uint64_t> seen;
        PPCContext* g0 = nullptr;
        { std::lock_guard lock(g_mutex); for (auto& t : g_threads) if (t.tid == GetCurrentThreadId()) g0 = t.ctx; }
        uint64_t key = (uint64_t)ep->ContextRecord->Rip * 31 ^ (g0 ? (uint32_t)g0->lr : 0);
        std::lock_guard lock(m);
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) { seen.push_back(key); fresh = true; }
    }
    if (fresh && InterlockedIncrement(&g_wwLogged) <= g_wwMax)
    {
        uintptr_t mod = (uintptr_t)GetModuleHandleW(nullptr);
        PPCContext* gctx = nullptr;
        { std::lock_guard lock(g_mutex); for (auto& t : g_threads) if (t.tid == GetCurrentThreadId()) gctx = t.ctx; }
        CONTEXT c = *ep->ContextRecord;
        char line[1200]; int n = snprintf(line, sizeof(line), "[sample] tid %lu escritura %08X (+%u) lr=0x%08X pila host:", GetCurrentThreadId(), guest, guest - g_wwAddr, gctx ? (uint32_t)gctx->lr : 0);
        for (int f = 0; f < 24 && c.Rip; f++)
        {
            n += snprintf(line + n, sizeof(line) - n, " %llX", (unsigned long long)(c.Rip - mod + 0x140000000ull));
            DWORD64 imageBase; auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
            void* hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &est, nullptr);
        }
        LOG("%s", line);
    }
    WwProtect(false);
    t_wwStep = true;
    ep->ContextRecord->EFlags |= 0x100;   // TF: una instruccion y vuelve a proteger
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

void DumpJobState();
// Sesion 24: GH6_PATWATCH=v@off,v@off,...: hilo que recorre la memoria fisica 0xB6000000-0xB7000000 buscando esa secuencia
// (dwords en orden de la consola) y, en cuanto aparece en una direccion nueva, vuelca la pila de todos los hilos de invitado
// (rondas [sample] para tools\symsample.py) y el contenido del objeto. Para ver quien compone los shaders vacios de CAS.
static void StartPatWatch()
{
    const char* q = getenv("GH6_PATWATCH");
    if (!q) return;
    std::vector<std::pair<uint32_t, uint32_t>> pat;
    while (q && *q)
    {
        char* e; uint32_t v = (uint32_t)strtoul(q, &e, 16), off = 0;
        if (*e == '@') off = (uint32_t)strtoul(e + 1, &e, 10);
        pat.push_back({ v, off }); q = (*e == ',') ? e + 1 : nullptr;
    }
    std::thread([pat] {
        std::vector<uint32_t> seen; int rounds = 0;
        uint32_t lo = getenv("GH6_PATLO") ? (uint32_t)strtoul(getenv("GH6_PATLO"), nullptr, 16) : 0xB6000000u;
        uint32_t hi = getenv("GH6_PATHI") ? (uint32_t)strtoul(getenv("GH6_PATHI"), nullptr, 16) : 0xB7000000u;
        uint32_t first = __builtin_bswap32(pat[0].first), off0 = pat[0].second;
        for (;;)
        {
            for (uint32_t a = lo; a < hi; a += 4)
            {
                if (*(const uint32_t*)(g_base + a + off0) != first) continue;
                bool ok = true;
                for (auto& pv : pat) if (__builtin_bswap32(*(const uint32_t*)(g_base + a + pv.second)) != pv.first) { ok = false; break; }
                if (!ok || std::find(seen.begin(), seen.end(), a) != seen.end()) continue;
                seen.push_back(a);
                if (rounds >= 8) continue;
                rounds++;
                LOG("[sample] ---- muestra %d ---- (patron en %08X)", rounds, a);
                std::vector<ThreadInfo> copy;
                { std::lock_guard lock(g_mutex); copy = g_threads; }
                for (auto& t : copy) DumpThread(t);
                std::string ctx;
                for (int k = -32; k < 4; k++) { char b[12]; snprintf(b, sizeof(b), "%08X ", __builtin_bswap32(*(const uint32_t*)(g_base + a + k * 4))); ctx += b; }
                LOG("[patwatch] %08X: %s", a, ctx.c_str());
                { char pn[64]; snprintf(pn, sizeof(pn), "patwatch_%08X.bin", a - 0x800);
                  if (FILE* f = fopen(pn, "wb")) { fwrite(g_base + a - 0x800, 1, 0x1000, f); fclose(f); } }
            }
            Sleep(0);
        }
    }).detach();
}
void StartDiagnostics()
{
    StartPatWatch();
    if (const char* ww = getenv("GH6_WRITEWATCH"))
    {
        char* e; g_wwAddr = (uint32_t)strtoul(ww, &e, 16); g_wwLen = (*e == ':') ? (uint32_t)strtoul(e + 1, nullptr, 16) : 4;
        if (getenv("GH6_WRITEWATCHMAX")) g_wwMax = atol(getenv("GH6_WRITEWATCHMAX"));
        g_wwPageLo = ((uintptr_t)g_base + g_wwAddr) & ~(uintptr_t)0xFFF;
        g_wwPageHi = ((uintptr_t)g_base + g_wwAddr + g_wwLen + 0xFFF) & ~(uintptr_t)0xFFF;
        AddVectoredExceptionHandler(1, WwHandler);
        int delay = getenv("GH6_WRITEWATCHDELAY") ? atoi(getenv("GH6_WRITEWATCHDELAY")) : 0;
        std::thread([delay] {
            Sleep(delay * 1000);
            LOG("[sample] ---- muestra 1 ---- (vigilando escrituras en %08X, %u bytes)", g_wwAddr, g_wwLen);
            WwProtect(true);
        }).detach();
    }
    if (getenv("GH6_JOBLOG"))
        std::thread([] { for (int i = 0;; i++) { Sleep(i < 60 ? 100 : 2000); DumpJobState(); } }).detach();
    // GH6_PROFILE=<segundos>:<muestras>: perfil del hilo de comandos de la GPU (sesion 24). Cada 2 ms suspende el hilo y
    // registra hasta 12 marcos de su pila (direcciones de GH6.exe como 14xxxxxxx; otros modulos con prefijo "m").
    // Se agrega con: python tools/profagg.py logs/runNN.log
    if (const char* ps = getenv("GH6_PROFILE"))
    {
        int psecs = atoi(ps); const char* pc = strchr(ps, ':'); int pcount = pc ? atoi(pc + 1) : 3000;
        std::thread([psecs, pcount] {
            Sleep(psecs * 1000);
            // GH6_PROFILEALL=1: todos los hilos registrados (lineas "[prof] T<tid>:<entrada> ...")
            static const bool all = getenv("GH6_PROFILEALL") != nullptr;
            std::vector<ThreadInfo> targets;
            { std::lock_guard lock(g_mutex); for (auto& t : g_threads) if (all || t.entry == 0xC0C0C0C0) targets.push_back(t); }
            if (targets.empty()) { LOG("[prof] sin hilo de comandos"); return; }
            uintptr_t mod = (uintptr_t)GetModuleHandleW(nullptr);
            timeBeginPeriod(1);
            for (int i = 0; i < pcount; i++)
            for (auto& cp : targets)
            {
                if (SuspendThread(cp.h) == (DWORD)-1) continue;
                CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
                char line[512]; int n = all ? snprintf(line, sizeof(line), "[prof] T%lu:%08X", cp.tid, cp.entry) : snprintf(line, sizeof(line), "[prof]");
                if (GetThreadContext(cp.h, &c))
                    for (int f = 0; f < 12 && c.Rip; f++)
                    {
                        HMODULE hm = nullptr;
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)c.Rip, &hm);
                        if ((uintptr_t)hm == mod) n += snprintf(line + n, sizeof(line) - n, " %llX", (unsigned long long)(c.Rip - mod + 0x140000000ull));
                        else
                        {
                            wchar_t wn[MAX_PATH] = L"?"; if (hm) GetModuleFileNameW(hm, wn, MAX_PATH);
                            const wchar_t* b = wcsrchr(wn, L'\\'); b = b ? b + 1 : wn;
                            n += snprintf(line + n, sizeof(line) - n, " m%ls", b);
                        }
                        DWORD64 imageBase; auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                        if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
                        void* handlerData; DWORD64 establisher;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
                    }
                ResumeThread(cp.h);
                LOG("%s", line);
                if (&cp == &targets.back()) Sleep(2);
            }
            timeEndPeriod(1);
            LOG("[prof] fin");
        }).detach();
    }
    const char* s = getenv("GH6_SAMPLE");
    if (!s) return;
    int secs = atoi(s);
    std::thread([secs] {
        Sleep(secs * 1000);
        for (int round = 0; round < 3; round++)
        {
            LOG("[sample] ---- muestra %d ----", round + 1);
            std::vector<ThreadInfo> copy;
            { std::lock_guard lock(g_mutex); copy = g_threads; }
            for (auto& t : copy) DumpThread(t);
            Sleep(1000);
        }
    }).detach();
}

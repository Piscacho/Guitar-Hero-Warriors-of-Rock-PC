// Imports implementados a mano (xboxkrnl / xam). El resto son stubs generados en import_stubs.cpp.
// Basado en el comportamiento documentado por Xenia (BSD) y UnleashedRecomp.
#include "runtime.h"
#include <ppc_recomp_shared.h>
#include <windows.h>
#include <timeapi.h>
#include <mutex>
#include <atomic>
#include <cstring>

thread_local GuestThreadState t_thread;
// Hilos, handles y sincronizacion: threads.cpp. Archivos: fs.cpp.

// ---------------- memoria ----------------
static std::mutex g_allocMutex;
static uint32_t g_virtTop = 0x40000000, g_physTop = 0xA0000000, g_kernTop = 0x7FFF0000 - 0x01000000;

static uint32_t Bump(uint32_t& top, uint32_t limit, uint32_t size, uint32_t align)
{
    std::lock_guard lock(g_allocMutex);
    uint32_t a = (top + align - 1) & ~(align - 1);
    if (uint64_t(a) + size > limit) { LOG("[mem] sin memoria de invitado (size=0x%X)", size); return 0; }
    top = a + size;
    memset(g_base + a, 0, size);
    return a;
}
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align) { return Bump(g_virtTop, 0x7EFF0000, size, align); }
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align) { return Bump(g_physTop, 0xBFFF0000, size, align); }
uint32_t GuestAllocKernel(uint32_t size, uint32_t align) { return Bump(g_kernTop, 0x7FFF0000, size, align); }
void GuestAllocTops(uint32_t& virt, uint32_t& phys) { virt = g_virtTop; phys = g_physTop; }   // diagnostico (sesion 24)

// ---------------- cabeceras XEX ----------------
static uint32_t g_xexHeaderSize = 0;
void InitXexHeader(const uint8_t* xex, size_t size)
{
    uint32_t peOff = __builtin_bswap32(*(uint32_t*)(xex + 8));
    g_xexHeaderSize = peOff;
    memcpy(g_base + kXexHeaderGuest, xex, peOff);
}
uint32_t XexHeaderField(uint32_t key)
{
    uint32_t count = Load32(kXexHeaderGuest + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t k = Load32(kXexHeaderGuest + 0x18 + i * 8);
        uint32_t v = Load32(kXexHeaderGuest + 0x18 + i * 8 + 4);
        if (k == key)
        {
            // tamano <= 4 bytes: valor directo; si no, offset desde el inicio de la cabecera
            return ((key & 0xFF) == 0 || (key & 0xFF) == 1) ? v : kXexHeaderGuest + v;
        }
    }
    return 0;
}

// ---------------- hilo / PCR ----------------
void InitGuestThread(PPCContext& ctx, uint32_t stackSize)
{
    t_thread.stackLimit = GuestAllocVirtual(stackSize, 0x10000);
    t_thread.stackBase = t_thread.stackLimit + stackSize;
    t_thread.pcr = GuestAllocKernel(0x2D8 + 0x1000, 0x1000);
    t_thread.tls = GuestAllocKernel(64 * 4 + 0x100, 0x10);   // 64 slots TLS (hdr 0x00020104)
    t_thread.thread = GuestAllocKernel(0x400, 0x10);
    // X_KPCR (Xenia): +0x00 tls_ptr, +0x30 pcr_ptr, +0x70 stack_base, +0x74 stack_end, +0x100 current_thread
    Store32(t_thread.pcr + 0x00, t_thread.tls);
    Store32(t_thread.pcr + 0x30, t_thread.pcr);
    Store32(t_thread.pcr + 0x70, t_thread.stackBase);
    Store32(t_thread.pcr + 0x74, t_thread.stackLimit);
    Store32(t_thread.pcr + 0x100, t_thread.thread);
    // X_KTHREAD (Xenia): +0x14C thread_id. El juego lo usa como dueno de sus locks: debe ser unico y no 0.
    Store32(t_thread.thread + 0x14C, GetCurrentThreadId());
    ctx.r1.u64 = t_thread.stackBase - 0x100;
    ctx.r13.u64 = t_thread.pcr;
    ctx.fpscr.loadFromHost();
}

// ---------------- variables importadas ----------------
// Cada descriptor de import de variable (en .rdata) se sobrescribe con la direccion del objeto.
struct VarImport { uint32_t descriptor; const char* name; uint32_t size; };
static const VarImport kVarImports[] = {
#include "var_imports.inc"
};
static uint32_t g_exeModule = 0;

// KeTimeStampBundle (sesion 16): el kernel real lo actualiza cada ms y el juego lee el contador de ticks directamente
// (GetTickCount en linea). Antes no se actualizaba nunca y el tiempo del juego quedaba congelado.
// Disposicion (Xenia): +0x00 interrupt_time (u64, 100 ns), +0x08 system_time (FILETIME), +0x10 tick_count (u32, ms).
static void StartTimeStampBundle(uint32_t obj)
{
    auto update = [obj] {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        static LARGE_INTEGER start = [] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        uint64_t d = (uint64_t)(c.QuadPart - start.QuadPart), f = (uint64_t)freq.QuadPart;
        uint64_t t100 = (d / f) * 10000000ull + (d % f) * 10000000ull / f;
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        Store64(obj + 0x00, t100);
        Store64(obj + 0x08, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        Store32(obj + 0x10, (uint32_t)(t100 / 10000));
    };
    update();
    CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto& fn = *(decltype(update)*)p;
        timeBeginPeriod(1);
        for (;;) { fn(); Sleep(1); }
    }, new decltype(update)(update), 0, nullptr);
}

void InitKernelVariables()
{
    for (auto& v : kVarImports)
    {
        uint32_t obj = GuestAllocKernel(v.size, 0x10);
        if (!strcmp(v.name, "XboxHardwareInfo")) { Store32(obj, 0x20); *(g_base + obj + 4) = 6; } // flags, num cpus
        else if (!strcmp(v.name, "XboxKrnlVersion")) { Store16(obj, 2); Store16(obj + 2, 0xFFFF); Store16(obj + 4, 0xFFFF); Store16(obj + 6, 0x8000); }
        else if (!strcmp(v.name, "VdGpuClockInMHz")) Store32(obj, 500);
        else if (!strcmp(v.name, "XexExecutableModuleHandle")) { uint32_t m = GuestAllocKernel(0x100); Store32(m + 0x58, kXexHeaderGuest); Store32(obj, m); g_exeModule = m; }
        else if (!strcmp(v.name, "ExLoadedCommandLine")) memcpy(g_base + obj, "\"default.xex\"", 14);
        else if (!strcmp(v.name, "KeTimeStampBundle")) StartTimeStampBundle(obj);
        Store32(v.descriptor, obj);
    }
}


// ---------------- TLS ----------------
static std::atomic<uint32_t> g_tlsNext{ 0 };
GUEST_IMPL(KeTlsAlloc) { uint32_t s = g_tlsNext++; ctx.r3.u64 = s < 64 ? s : 0xFFFFFFFF; }
GUEST_IMPL(KeTlsFree) { ctx.r3.u64 = 1; }
GUEST_IMPL(KeTlsGetValue) { ctx.r3.u64 = ctx.r3.u32 < 64 ? Load32(t_thread.tls + ctx.r3.u32 * 4) : 0; }
GUEST_IMPL(KeTlsSetValue) { if (ctx.r3.u32 < 64) Store32(t_thread.tls + ctx.r3.u32 * 4, ctx.r4.u32); ctx.r3.u64 = 1; }

// ---------------- memoria del kernel ----------------
// NtAllocateVirtualMemory(uint32_t* base_addr_ptr, uint32_t* region_size_ptr, alloc_type, protect, debug)
GUEST_IMPL(NtAllocateVirtualMemory)
{
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t want = Load32(pBase), size = (Load32(pSize) + 0xFFF) & ~0xFFF;
    uint32_t a = want ? want : GuestAllocVirtual(size, (ctx.r5.u32 & 0x20000000) ? 0x10000 : 0x1000);
    if (!a) { ctx.r3.u64 = 0xC0000017; return; } // STATUS_NO_MEMORY
    Store32(pBase, a);
    Store32(pSize, size);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NtFreeVirtualMemory) { ctx.r3.u64 = 0; }
// MmAllocatePhysicalMemoryEx(flags, size, protect, min_addr, max_addr, alignment)
GUEST_IMPL(MmAllocatePhysicalMemoryEx)
{
    uint32_t align = ctx.r8.u32 > 0x10000 ? ctx.r8.u32 : 0x10000;
    ctx.r3.u64 = GuestAllocPhysical((ctx.r4.u32 + 0xFFF) & ~0xFFF, align);
}
GUEST_IMPL(MmFreePhysicalMemory) { ctx.r3.u64 = 0; }
GUEST_IMPL(MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }
GUEST_IMPL(MmQueryAddressProtect) { ctx.r3.u64 = 4; } // PAGE_READWRITE

// ---------------- informacion del sistema / XEX ----------------
GUEST_IMPL(KeGetCurrentProcessType) { ctx.r3.u64 = 1; } // X_PROCTYPE_TITLE
GUEST_IMPL(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }
GUEST_IMPL(RtlImageXexHeaderField) { ctx.r3.u64 = XexHeaderField(ctx.r4.u32); }
GUEST_IMPL(XGetAVPack) { ctx.r3.u64 = 6; }          // HDMI
GUEST_IMPL(XGetGameRegion) { ctx.r3.u64 = 0xFFFF; }
GUEST_IMPL(XGetLanguage) { ctx.r3.u64 = 1; }        // ingles
GUEST_IMPL(XamGetSystemVersion) { ctx.r3.u64 = 0x20096B00; }
GUEST_IMPL(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
GUEST_IMPL(KeQuerySystemTime)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    if (ctx.r3.u32) Store64(ctx.r3.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}
// RtlTimeToTimeFields(time_ptr (u64 FILETIME), fields_ptr): TIME_FIELDS = 8 x u16 (anio, mes, dia, hora, minuto, segundo, ms, dia de la semana)
GUEST_IMPL(RtlTimeToTimeFields)
{
    uint64_t t = __builtin_bswap64(*(uint64_t*)(g_base + ctx.r3.u32));
    FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) }; SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    uint32_t p = ctx.r4.u32;
    Store16(p + 0, st.wYear); Store16(p + 2, st.wMonth); Store16(p + 4, st.wDay); Store16(p + 6, st.wHour);
    Store16(p + 8, st.wMinute); Store16(p + 10, st.wSecond); Store16(p + 12, st.wMilliseconds); Store16(p + 14, st.wDayOfWeek);
}
GUEST_IMPL(DbgPrint) { LOG("[DbgPrint] %s", (const char*)(g_base + ctx.r3.u32)); ctx.r3.u64 = 0; }
GUEST_IMPL(KeBugCheck) { LOG("[kernel] KeBugCheck(0x%X)", ctx.r3.u32); ExitProcess(1); }
GUEST_IMPL(KeBugCheckEx) { LOG("[kernel] KeBugCheckEx(0x%X)", ctx.r3.u32); ExitProcess(1); }
GUEST_IMPL(RtlRaiseException) { LOG("[kernel] RtlRaiseException(0x%X)", Load32(ctx.r3.u32)); }

// ================= Sesion 3: segundo grupo (arranque del juego) =================
#include <unordered_map>

// RtlInitAnsiString(ANSI_STRING* dst, const char* src)  {u16 Length; u16 MaximumLength; u32 Buffer}
GUEST_IMPL(RtlInitAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? (uint16_t)strlen((const char*)(g_base + src)) : 0;
    Store16(dst, len); Store16(dst + 2, src ? len + 1 : 0); Store32(dst + 4, src);
}
// Sesion 24: antes eran stubs vacios (no rellenaban ni comparaban nada)
GUEST_IMPL(RtlFillMemoryUlong)
{
    uint32_t dst = ctx.r3.u32, count = ctx.r4.u32 >> 2, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < count; i++) Store32(dst + i * 4, pattern);
}
GUEST_IMPL(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, count = ctx.r4.u32 >> 2, pattern = ctx.r5.u32, i = 0;
    while (i < count && Load32(src + i * 4) == pattern) i++;
    ctx.r3.u64 = i * 4;
}
GUEST_IMPL(RtlNtStatusToDosError) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 0 : 317; }
// ExGetXConfigSetting(category, setting, buffer, buffer_size, required_size_ptr)
GUEST_IMPL(ExGetXConfigSetting)
{
    uint16_t cat = ctx.r3.u16, set = ctx.r4.u16;
    uint32_t buf = ctx.r5.u32, size = ctx.r6.u16, req = ctx.r7.u32;
    uint32_t value = 0, vsize = 4;
    if (cat == 0x0003 && set == 0x0009) value = 1;               // idioma: ingles
    else if (cat == 0x0003 && set == 0x000A) value = 0x00040000; // video: 16:9
    else if (cat == 0x0002 && set == 0x0002) value = 0x00001000; // region AV: NTSC-M
    else if (cat == 0x0003 && set == 0x000E) { value = 0x67; vsize = 1; } // pais
    else LOG("[kernel] ExGetXConfigSetting(%u, %u) no conocido -> 0", cat, set);
    if (buf && size)
    {
        memset(g_base + buf, 0, size);
        if (vsize == 4 && size >= 4) Store32(buf, value);
        else if (vsize == 1) g_base[buf] = (uint8_t)value;
    }
    if (req) Store16(req, (uint16_t)vsize);
    ctx.r3.u64 = 0;
}
// XGetVideoMode(X_VIDEO_MODE*): ancho, alto, entrelazado, panoramico, HD, refresco(float), estandar
GUEST_IMPL(XGetVideoMode)
{
    uint32_t p = ctx.r3.u32;
    memset(g_base + p, 0, 0x30);
    // Sesion 25 (experimento panoramico): GH6_ANCHO = ancho del modo de video que ve el juego
    static const uint32_t vw = getenv("GH6_ANCHO") ? (uint32_t)atoi(getenv("GH6_ANCHO")) : 1280;
    Store32(p + 0x00, vw); Store32(p + 0x04, 720); Store32(p + 0x08, 0); Store32(p + 0x0C, 1); Store32(p + 0x10, 1);
    float hz = 60.0f; uint32_t hzBits; memcpy(&hzBits, &hz, 4); Store32(p + 0x14, hzBits);
    Store32(p + 0x18, 1);
}
GUEST_IMPL(XexLoadImage) { LOG("[kernel] XexLoadImage(%s) -> no encontrado", (const char*)(g_base + ctx.r3.u32)); ctx.r3.u64 = 0xC0000034; }
// XexGetProcedureAddress(hmodule, ordinal, out_ptr) (sesion 23): si el ordinal de xam.xex corresponde a una funcion que el juego
// ya importa, se devuelve la direccion de su thunk (que el runtime ya implementa). Tabla en xam_ordinals.inc (de xex\imports_named.txt).
struct OrdinalThunk { uint32_t ordinal, thunk; };
static const OrdinalThunk kXamOrdinals[] = {
#include "xam_ordinals.inc"
};
// Funciones de XAM que el juego no importa pero pide por ordinal: se registran en direcciones de relleno del codigo
PPC_FUNC(gh6_XamContentAggregateCreateEnumerator);
PPC_FUNC(gh6_XamContentCreateInternal);
static constexpr uint32_t kThunkAggregateEnum = 0x8242ABE4;   // ".long 0" entre funciones (nunca se ejecuta)
static constexpr uint32_t kThunkContentCreateInternal = 0x8242015C;   // idem
void RegisterExtraXamFunctions()
{
    PPC_LOOKUP_FUNC(g_base, kThunkAggregateEnum) = gh6_XamContentAggregateCreateEnumerator;
    PPC_LOOKUP_FUNC(g_base, kThunkContentCreateInternal) = gh6_XamContentCreateInternal;
}
GUEST_IMPL(XexGetProcedureAddress)
{
    uint32_t ordinal = ctx.r4.u32, thunk = 0;
    for (auto& o : kXamOrdinals) if (o.ordinal == ordinal) { thunk = o.thunk; break; }
    if (ordinal == 633) thunk = kThunkAggregateEnum;   // XamContentAggregateCreateEnumerator
    if (ordinal == 625 && !getenv("GH6_NOCREATEINTERNAL")) thunk = kThunkContentCreateInternal;   // XamContentCreateInternal (sesion 24)
    LOG("[kernel] XexGetProcedureAddress(modulo %08X, ordinal %u) -> %08X", ctx.r3.u32, ordinal, thunk);
    if (ctx.r5.u32) Store32(ctx.r5.u32, thunk);
    ctx.r3.u64 = thunk ? 0 : 0xC0000139;
}
// MmQueryStatistics(X_MM_STATISTICS*): Length, TotalPhysicalPages, KernelPages, TitleAvailablePages, ...
GUEST_IMPL(MmQueryStatistics)
{
    uint32_t p = ctx.r3.u32;
    uint32_t len = Load32(p) ? Load32(p) : 0x68;
    memset(g_base + p, 0, len);
    Store32(p + 0x00, len);
    Store32(p + 0x04, 0x20000); // 512 MB en paginas de 4 KB
    Store32(p + 0x08, 0x2000);
    Store32(p + 0x0C, 0x18000); // disponibles para el titulo
    Store32(p + 0x10, 0x20000000);
    ctx.r3.u64 = 0;
}

// Llamadas indirectas (ver compat/ppc_indirect.h)
void GuestCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    PPCFunc* fn = (target >= PPC_CODE_BASE && target < PPC_CODE_BASE + PPC_CODE_SIZE) ? PPC_LOOKUP_FUNC(base, target) : nullptr;
    if (!fn)
    {
        LOG("[crash] llamada indirecta a 0x%08X sin funcion traducida (lr=0x%08X r3=0x%08X)", target, (uint32_t)ctx.lr, ctx.r3.u32);
        ExitProcess(2);
    }
    fn(ctx, base);
}

// XexGetModuleHandle(module_name, hmodule_ptr): NULL o el propio ejecutable -> modulo principal; otros -> objeto ficticio
GUEST_IMPL(XexGetModuleHandle)
{
    const char* name = ctx.r3.u32 ? (const char*)(g_base + ctx.r3.u32) : nullptr;
    uint32_t h = g_exeModule;
    if (name && _stricmp(name, "default.xex") != 0)
    {
        static uint32_t dummy = 0;
        if (!dummy) { dummy = GuestAllocKernel(0x100); Store32(dummy + 0x58, kXexHeaderGuest); }
        h = dummy;
        LOG("[kernel] XexGetModuleHandle(%s) -> modulo ficticio", name);
    }
    if (ctx.r4.u32) Store32(ctx.r4.u32, h);
    ctx.r3.u64 = h ? 0 : 0xC0000225;
}

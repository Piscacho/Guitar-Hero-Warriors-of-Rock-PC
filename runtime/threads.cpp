// Hilos, sincronizacion y handles (sesion 4). Hilos nativos de Windows, uno por hilo de invitado.
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <cstring>

// ---------------- tabla de handles ----------------
static std::mutex g_hMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_byPtr;
static uint32_t g_nextHandle = 0xF8000000;

uint32_t InsertHandle(std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(g_hMutex);
    uint32_t h = g_nextHandle; g_nextHandle += 4;
    static bool evlog = getenv("GH6_EVENTLOG") != nullptr;
    if (evlog) LOG("[handle] 0x%08X tipo %u", h, obj->type);
    g_handles[h] = std::move(obj);
    return h;
}
std::shared_ptr<KObject> GetHandleObject(uint32_t handle)
{
    std::lock_guard lock(g_hMutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}
bool CloseGuestHandle(uint32_t handle)
{
    std::lock_guard lock(g_hMutex);
    return g_handles.erase(handle) != 0;
}
uint32_t EnsureGuestPointer(const std::shared_ptr<KObject>& obj)
{
    std::lock_guard lock(g_hMutex);
    if (!obj->guestPtr)
    {
        obj->guestPtr = GuestAllocKernel(0x40, 0x10);
        g_base[obj->guestPtr] = obj->type;
        g_byPtr[obj->guestPtr] = obj;
    }
    return obj->guestPtr;
}
static void RegisterPointer(uint32_t ptr, std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(g_hMutex);
    obj->guestPtr = ptr;
    g_byPtr[ptr] = std::move(obj);
}
std::shared_ptr<KObject> GetObjectByPointer(uint32_t ptr)
{
    std::lock_guard lock(g_hMutex);
    auto it = g_byPtr.find(ptr);
    if (it != g_byPtr.end()) return it->second;
    // Objeto inicializado por el juego en su propia memoria (KeInitializeEvent inline, etc.)
    auto obj = std::make_shared<KObject>();
    obj->type = g_base[ptr];
    obj->guestPtr = ptr;
    bool signaled = Load32(ptr + 4) != 0;
    switch (obj->type)
    {
    case kObjNotificationEvent: obj->waitable = CreateEventW(nullptr, TRUE, signaled, nullptr); break;
    case kObjSyncEvent: obj->waitable = CreateEventW(nullptr, FALSE, signaled, nullptr); break;
    case kObjSemaphore: obj->waitable = CreateSemaphoreW(nullptr, Load32(ptr + 4), Load32(ptr + 0x10) ? Load32(ptr + 0x10) : 0x7FFFFFFF, nullptr); break;
    case kObjMutant: obj->waitable = CreateMutexW(nullptr, FALSE, nullptr); break;
    default: LOG("[kernel] objeto por puntero 0x%08X de tipo %u desconocido", ptr, obj->type); obj->waitable = CreateEventW(nullptr, TRUE, FALSE, nullptr); break;
    }
    g_byPtr[ptr] = obj;
    return obj;
}

DWORD GuestTimeoutToMs(uint32_t timeoutPtr)
{
    if (!timeoutPtr) return INFINITE;
    int64_t t = (int64_t)__builtin_bswap64(*(uint64_t*)(g_base + timeoutPtr));
    if (t < 0) return (DWORD)((-t) / 10000);
    if (t == 0) return 0;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    int64_t now = (int64_t)((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return t > now ? (DWORD)((t - now) / 10000) : 0;
}
uint32_t WaitResult(DWORD r, uint32_t count)
{
    if (r == WAIT_TIMEOUT) return XSTATUS_TIMEOUT;
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) return r - WAIT_OBJECT_0;
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) return 0x80 + (r - WAIT_ABANDONED_0);
    return XSTATUS_INVALID_HANDLE;
}

static std::shared_ptr<KObject> MakeWaitable(uint8_t type, HANDLE h)
{
    auto o = std::make_shared<KObject>(); o->type = type; o->waitable = h; return o;
}

// ---------------- hilos ----------------
// KTHREAD de invitado -> KPCR de su hilo (para KeSetAffinityThread). KPCR+0x10C = numero de CPU (0..5).
static std::mutex g_pcrMutex;
static std::unordered_map<uint32_t, uint32_t> g_threadPcr;
static std::atomic<uint32_t> g_nextCpu{ 1 };
static std::unordered_map<uint32_t, uint32_t> g_pendingCpu; // afinidad pedida antes de que el hilo arranque
void RegisterThreadPcr(uint32_t kthread, uint32_t pcr, uint32_t cpu)
{
    std::lock_guard lock(g_pcrMutex);
    auto it = g_pendingCpu.find(kthread);
    if (it != g_pendingCpu.end()) { cpu = it->second; g_pendingCpu.erase(it); }
    g_base[pcr + 0x10C] = (uint8_t)cpu;
    g_threadPcr[kthread] = pcr;
}
struct ThreadStart { uint32_t startAddress, startContext, xapiStartup, stackSize, kthread; HANDLE ready; };

static DWORD WINAPI GuestThreadProc(void* param)
{
    ThreadStart st = *(ThreadStart*)param;
    delete (ThreadStart*)param;
    auto* ctx = new PPCContext{};
    InitGuestThread(*ctx, st.stackSize ? st.stackSize : 0x40000);
    if (st.kthread) { t_thread.thread = st.kthread; Store32(t_thread.pcr + 0x100, st.kthread); Store32(st.kthread + 0x14C, GetCurrentThreadId()); }
    RegisterThreadPcr(t_thread.thread, t_thread.pcr, g_nextCpu++ % 6);
    uint32_t entry;
    if (st.xapiStartup) { entry = st.xapiStartup; ctx->r3.u64 = st.startAddress; ctx->r4.u64 = st.startContext; }
    else { entry = st.startAddress; ctx->r3.u64 = st.startContext; }
    RegisterGuestThread(ctx, st.startAddress);
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, entry);
    if (!fn) { LOG("[thread] entrada 0x%08X sin funcion traducida", entry); return 1; }
    fn(*ctx, g_base);
    return ctx->r3.u32;
}

// ExCreateThread(handle_ptr, stack_size, thread_id_ptr, xapi_startup, start_address, start_context, creation_flags)
GUEST_IMPL(ExCreateThread)
{
    auto* st = new ThreadStart{ ctx.r7.u32, ctx.r8.u32, ctx.r6.u32, ctx.r4.u32, GuestAllocKernel(0x400, 0x10), nullptr };
    bool suspended = (ctx.r9.u32 & 1) != 0;
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 1 << 20, GuestThreadProc, st, CREATE_SUSPENDED, &tid);
    auto obj = MakeWaitable(kObjThread, h);
    g_base[st->kthread] = kObjThread;
    RegisterPointer(st->kthread, obj);
    uint32_t gh = InsertHandle(obj);
    LOG("[thread] ExCreateThread start=0x%08X xapi=0x%08X ctx=0x%08X tid=%lu%s", st->startAddress, st->xapiStartup, st->startContext, tid, suspended ? " (suspendido)" : "");
    if (ctx.r3.u32) Store32(ctx.r3.u32, gh);
    if (ctx.r5.u32) Store32(ctx.r5.u32, tid);
    if (!suspended) ResumeThread(h);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(NtResumeThread)
{
    auto o = GetHandleObject(ctx.r3.u32);
    DWORD prev = o && o->waitable ? ResumeThread(o->waitable) : 0;
    if (ctx.r4.u32) Store32(ctx.r4.u32, prev);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(ExTerminateThread) { ExitThread(ctx.r3.u32); }
// KeSetAffinityThread(thread_ptr, affinity_mask, previous_ptr): cambia el numero de CPU que ve el hilo
GUEST_IMPL(KeSetAffinityThread)
{
    uint32_t thread = ctx.r3.u32, mask = ctx.r4.u32 & 0x3F;
    uint32_t pcr = 0;
    { std::lock_guard lock(g_pcrMutex); auto it = g_threadPcr.find(thread); if (it != g_threadPcr.end()) pcr = it->second; }
    uint32_t prev = pcr ? (1u << g_base[pcr + 0x10C]) : 0x3F;
    if (mask)
    {
        if (pcr) g_base[pcr + 0x10C] = (uint8_t)__builtin_ctz(mask);
        else { std::lock_guard lock(g_pcrMutex); g_pendingCpu[thread] = __builtin_ctz(mask); }
    }
    LOG("[thread] KeSetAffinityThread(0x%08X, 0x%X) -> cpu %d%s", thread, mask, mask ? __builtin_ctz(mask) : -1, pcr ? "" : " (al arrancar)");
    if (ctx.r5.u32) Store32(ctx.r5.u32, prev);
    ctx.r3.u64 = prev;
}
// KeSetBasePriorityThread(kthread, increment) (sesion 23): el juego sube la prioridad de sus hilos de audio. Antes se ignoraba y, con la
// CPU cargada, el mezclador se retrasaba y sacaba silencio (cortes). Incremento de Xbox -> prioridad de Windows.
static std::mutex g_prioMutex;
static std::unordered_map<uint32_t, int> g_guestPrio;
GUEST_IMPL(KeSetBasePriorityThread)
{
    uint32_t kthread = ctx.r3.u32; int inc = (int)ctx.r4.u32;
    int prev = 0;
    { std::lock_guard l(g_prioMutex); prev = g_guestPrio[kthread]; g_guestPrio[kthread] = inc; }
    HANDLE h = nullptr;
    if (kthread == t_thread.thread) h = GetCurrentThread();
    else if (auto o = GetObjectByPointer(kthread)) h = o->waitable;
    int p = inc >= 15 ? THREAD_PRIORITY_TIME_CRITICAL : inc >= 2 ? THREAD_PRIORITY_HIGHEST : inc == 1 ? THREAD_PRIORITY_ABOVE_NORMAL
          : inc == 0 ? THREAD_PRIORITY_NORMAL : inc == -1 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    if (h) SetThreadPriority(h, p);
    static int logged = 0;
    if (logged++ < 32) LOG("[thread] KeSetBasePriorityThread(0x%08X, %d) -> prioridad Windows %d", kthread, inc, p);
    ctx.r3.u64 = (uint32_t)prev;
}
GUEST_IMPL(KeQueryBasePriorityThread)
{
    std::lock_guard l(g_prioMutex);
    auto it = g_guestPrio.find(ctx.r3.u32);
    ctx.r3.u64 = it != g_guestPrio.end() ? (uint32_t)it->second : 0;
}
GUEST_IMPL(KeDelayExecutionThread)
{
    DWORD ms = GuestTimeoutToMs(ctx.r5.u32);
    if (ms == 0) SwitchToThread(); else Sleep(ms);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(KeEnterCriticalRegion) {}
GUEST_IMPL(KeLeaveCriticalRegion) {}

// ---------------- handles genericos ----------------
GUEST_IMPL(NtClose) { ctx.r3.u64 = CloseGuestHandle(ctx.r3.u32) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE; }
// NtDuplicateObject(handle, new_handle_ptr, options)
GUEST_IMPL(NtDuplicateObject)
{
    auto o = GetHandleObject(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    if (ctx.r4.u32) Store32(ctx.r4.u32, InsertHandle(o));
    if (ctx.r5.u32 & 1) CloseGuestHandle(ctx.r3.u32); // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// ObReferenceObjectByHandle(handle, object_type, out_object_ptr)
GUEST_IMPL(ObReferenceObjectByHandle)
{
    uint32_t ptr = 0;
    if (ctx.r3.u32 == kCurrentThreadHandle) ptr = t_thread.thread;
    else if (auto o = GetHandleObject(ctx.r3.u32)) ptr = EnsureGuestPointer(o);
    if (!ptr) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) Store32(ctx.r5.u32, ptr);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(ObReferenceObject) { ctx.r3.u64 = 0; }
GUEST_IMPL(ObDereferenceObject) { ctx.r3.u64 = 0; }
GUEST_IMPL(ObIsTitleObject) { ctx.r3.u64 = 1; }

// ---------------- eventos / mutex / semaforos / temporizadores ----------------
// NtCreateEvent(handle_ptr, obj_attributes, event_type, initial_state)
GUEST_IMPL(NtCreateEvent)
{
    bool manual = ctx.r5.u32 == 0;
    auto o = MakeWaitable(manual ? kObjNotificationEvent : kObjSyncEvent, CreateEventW(nullptr, manual, ctx.r6.u32 != 0, nullptr));
    uint32_t h = InsertHandle(o);
    Store32(ctx.r3.u32, h);
    static bool evlog = getenv("GH6_EVENTLOG") != nullptr;
    if (evlog) LOG("[event] NtCreateEvent -> 0x%08X (%s, inicial %u) lr=0x%08X", h, manual ? "manual" : "auto", ctx.r6.u32, (uint32_t)ctx.lr);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(NtSetEvent)
{
    auto o = GetHandleObject(ctx.r3.u32);
    static bool evlog = getenv("GH6_EVENTLOG") != nullptr;
    if (evlog)
    {
        static std::mutex m; static std::unordered_map<uint32_t, int> seen;
        std::lock_guard l(m);
        if (seen[ctx.r3.u32]++ < 3) LOG("[event] NtSetEvent(0x%08X) lr=0x%08X", ctx.r3.u32, (uint32_t)ctx.lr);
    }
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = o && SetEvent(o->waitable) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE;
}
GUEST_IMPL(NtClearEvent) { auto o = GetHandleObject(ctx.r3.u32); ctx.r3.u64 = o && ResetEvent(o->waitable) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE; }
GUEST_IMPL(NtPulseEvent)
{
    auto o = GetHandleObject(ctx.r3.u32);
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = o && PulseEvent(o->waitable) ? XSTATUS_SUCCESS : XSTATUS_INVALID_HANDLE;
}
GUEST_IMPL(KeSetEvent) { auto o = GetObjectByPointer(ctx.r3.u32); SetEvent(o->waitable); Store32(ctx.r3.u32 + 4, 1); ctx.r3.u64 = 0; }
GUEST_IMPL(KeResetEvent) { auto o = GetObjectByPointer(ctx.r3.u32); ResetEvent(o->waitable); Store32(ctx.r3.u32 + 4, 0); ctx.r3.u64 = 0; }
// NtCreateMutant(handle_ptr, obj_attributes, initial_owner)
GUEST_IMPL(NtCreateMutant)
{
    auto o = MakeWaitable(kObjMutant, CreateMutexW(nullptr, ctx.r5.u32 != 0, nullptr));
    Store32(ctx.r3.u32, InsertHandle(o));
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(NtReleaseMutant)
{
    auto o = GetHandleObject(ctx.r3.u32);
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = o && ReleaseMutex(o->waitable) ? XSTATUS_SUCCESS : 0xC0000046; // XSTATUS_MUTANT_NOT_OWNED
}
// NtCreateSemaphore(handle_ptr, obj_attributes, initial_count, maximum_count)
GUEST_IMPL(NtCreateSemaphore)
{
    auto o = MakeWaitable(kObjSemaphore, CreateSemaphoreW(nullptr, ctx.r5.s32, ctx.r6.s32 > 0 ? ctx.r6.s32 : 0x7FFFFFFF, nullptr));
    Store32(ctx.r3.u32, InsertHandle(o));
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtReleaseSemaphore(handle, release_count, previous_count_ptr)
GUEST_IMPL(NtReleaseSemaphore)
{
    auto o = GetHandleObject(ctx.r3.u32);
    static bool evlog = getenv("GH6_EVENTLOG") != nullptr;
    if (evlog)
    {
        static std::mutex m; static std::unordered_map<uint32_t, int> seen;
        std::lock_guard l(m);
        int k = seen[ctx.r3.u32]++;
        if (k < 3 || k % 500 == 0) LOG("[event] NtReleaseSemaphore(0x%08X, %d) #%d lr=0x%08X", ctx.r3.u32, ctx.r4.s32, k, (uint32_t)ctx.lr);
    }
    LONG prev = 0;
    BOOL ok = o && ReleaseSemaphore(o->waitable, ctx.r4.s32, &prev);
    if (ctx.r5.u32) Store32(ctx.r5.u32, prev);
    ctx.r3.u64 = ok ? XSTATUS_SUCCESS : 0xC0000047; // XSTATUS_SEMAPHORE_LIMIT_EXCEEDED
}
// NtCreateTimer(handle_ptr, obj_attributes, timer_type)
GUEST_IMPL(NtCreateTimer)
{
    bool manual = ctx.r5.u32 == 0;
    auto o = MakeWaitable(manual ? kObjTimerNotif : kObjTimerSync, CreateWaitableTimerW(nullptr, manual, nullptr));
    Store32(ctx.r3.u32, InsertHandle(o));
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtSetTimerEx(handle, due_time_ptr, apc_routine, apc_mode, apc_context, resume, period_ms, prev_state_ptr)
GUEST_IMPL(NtSetTimerEx)
{
    auto o = GetHandleObject(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) LOG("[kernel] NtSetTimerEx con APC 0x%08X (no soportado aun)", ctx.r5.u32);
    LARGE_INTEGER due; due.QuadPart = (int64_t)__builtin_bswap64(*(uint64_t*)(g_base + ctx.r4.u32));
    SetWaitableTimer(o->waitable, &due, ctx.r9.s32, nullptr, nullptr, FALSE);
    if (ctx.r10.u32) Store32(ctx.r10.u32, 0);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(NtCancelTimer) { auto o = GetHandleObject(ctx.r3.u32); if (o) CancelWaitableTimer(o->waitable); if (ctx.r4.u32) Store32(ctx.r4.u32, 0); ctx.r3.u64 = XSTATUS_SUCCESS; }

// ---------------- esperas ----------------
// Espera que avisa si dura mas de 3 s (para localizar bloqueos)
static DWORD WaitLogged(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alertable, const char* what, uint32_t id, uint64_t lr)
{
    if (ms <= 3000) return WaitForMultipleObjectsEx(n, hs, all, ms, alertable);
    DWORD r = WaitForMultipleObjectsEx(n, hs, all, 3000, alertable);
    if (r != WAIT_TIMEOUT) return r;
    LOG("[wait] tid %lu lleva >3 s en %s(0x%08X, n=%lu) lr=0x%08X", GetCurrentThreadId(), what, id, n, (uint32_t)lr);
    r = WaitForMultipleObjectsEx(n, hs, all, ms == INFINITE ? INFINITE : ms - 3000, alertable);
    LOG("[wait] tid %lu sale de %s(0x%08X)", GetCurrentThreadId(), what, id);
    return r;
}

static HANDLE HandleFor(uint32_t h)
{
    if (h == kCurrentThreadHandle) return GetCurrentThread();
    auto o = GetHandleObject(h);
    return o ? o->waitable : nullptr;
}
// NtWaitForSingleObjectEx(handle, wait_mode, alertable, timeout_ptr)
GUEST_IMPL(NtWaitForSingleObjectEx)
{
    HANDLE h = HandleFor(ctx.r3.u32);
    if (!h) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = WaitResult(WaitLogged(1, &h, FALSE, GuestTimeoutToMs(ctx.r6.u32), ctx.r5.u32 != 0, "NtWaitForSingleObjectEx", ctx.r3.u32, ctx.lr));
}
// KeWaitForSingleObject(object_ptr, reason, mode, alertable, timeout_ptr)
GUEST_IMPL(KeWaitForSingleObject)
{
    auto o = GetObjectByPointer(ctx.r3.u32);
    ctx.r3.u64 = WaitResult(WaitLogged(1, &o->waitable, FALSE, GuestTimeoutToMs(ctx.r7.u32), ctx.r6.u32 != 0, "KeWaitForSingleObject", ctx.r3.u32, ctx.lr));
}
// NtWaitForMultipleObjectsEx(count, handles_ptr, wait_type(0=all,1=any), wait_mode, alertable, timeout_ptr)
GUEST_IMPL(NtWaitForMultipleObjectsEx)
{
    uint32_t n = ctx.r3.u32 > MAXIMUM_WAIT_OBJECTS ? MAXIMUM_WAIT_OBJECTS : ctx.r3.u32;
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    for (uint32_t i = 0; i < n; i++) if (!(hs[i] = HandleFor(Load32(ctx.r4.u32 + i * 4)))) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = WaitResult(WaitLogged(n, hs, ctx.r5.u32 == 0, GuestTimeoutToMs(ctx.r8.u32), ctx.r7.u32 != 0, "NtWaitForMultipleObjectsEx", Load32(ctx.r4.u32), ctx.lr), n);
}
// KeWaitForMultipleObjects(count, objects_ptr, wait_type, reason, mode, alertable, timeout_ptr, wait_blocks)
GUEST_IMPL(KeWaitForMultipleObjects)
{
    uint32_t n = ctx.r3.u32 > MAXIMUM_WAIT_OBJECTS ? MAXIMUM_WAIT_OBJECTS : ctx.r3.u32;
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    for (uint32_t i = 0; i < n; i++) hs[i] = GetObjectByPointer(Load32(ctx.r4.u32 + i * 4))->waitable;
    static bool waitLog = getenv("GH6_WAITLOG") != nullptr;
    if (waitLog)
    {
        static std::mutex m; static std::unordered_map<uint32_t, int> seen;
        std::lock_guard lock(m);
        if (seen[(uint32_t)ctx.lr]++ < 3)
        {
            char b[512]; int k = snprintf(b, sizeof(b), "[waitlog] lr=0x%08X n=%u tipo_espera=%u timeout=%s:", (uint32_t)ctx.lr, n, ctx.r5.u32, ctx.r9.u32 ? "si" : "infinito");
            for (uint32_t i = 0; i < n; i++) { uint32_t p = Load32(ctx.r4.u32 + i * 4); k += snprintf(b + k, sizeof(b) - k, " [%08X tipo %u estado %d]", p, g_base[p], (int)Load32(p + 4)); }
            LOG("%s", b);
        }
    }
    ctx.r3.u64 = WaitResult(WaitLogged(n, hs, ctx.r5.u32 == 0, GuestTimeoutToMs(ctx.r9.u32), ctx.r8.u32 != 0, "KeWaitForMultipleObjects", Load32(ctx.r4.u32), ctx.lr), n);
}

// ---------------- spinlocks y secciones criticas (atomicas sobre memoria de invitado) ----------------
static inline volatile LONG* Raw32(uint32_t a) { return (volatile LONG*)(g_base + a); }
static inline LONG MeBE() { return (LONG)__builtin_bswap32(t_thread.thread); }

GUEST_IMPL(KeAcquireSpinLockAtRaisedIrql)
{
    LONG me = MeBE();
    for (uint32_t spins = 0; InterlockedCompareExchange(Raw32(ctx.r3.u32), me, 0) != 0; spins++)
        if (spins > 64) SwitchToThread(); else YieldProcessor();
}
GUEST_IMPL(KeReleaseSpinLockFromRaisedIrql) { InterlockedExchange(Raw32(ctx.r3.u32), 0); }

// X_RTL_CRITICAL_SECTION: +0x00 cabecera, +0x10 lock_count, +0x14 recursion_count, +0x18 owning_thread
GUEST_IMPL(RtlInitializeCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    memset(g_base + cs, 0, 0x1C);
    Store32(cs + 0x10, 0xFFFFFFFF);
}
GUEST_IMPL(RtlInitializeCriticalSectionAndSpinCount) { __imp__RtlInitializeCriticalSection(ctx, base); ctx.r3.u64 = 0; }
static bool TryEnter(uint32_t cs)
{
    LONG me = MeBE();
    LONG owner = *Raw32(cs + 0x18);
    if (owner == me) { Store32(cs + 0x14, Load32(cs + 0x14) + 1); return true; }
    if (owner == 0 && InterlockedCompareExchange(Raw32(cs + 0x18), me, 0) == 0) { Store32(cs + 0x14, 1); return true; }
    return false;
}
GUEST_IMPL(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    for (uint32_t spins = 0; !TryEnter(cs); spins++)
        if (spins > 128) Sleep(spins > 4096 ? 1 : 0); else YieldProcessor();
}
GUEST_IMPL(RtlTryEnterCriticalSection) { ctx.r3.u64 = TryEnter(ctx.r3.u32) ? 1 : 0; }
GUEST_IMPL(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t rec = Load32(cs + 0x14) - 1;
    Store32(cs + 0x14, rec);
    if (rec == 0) InterlockedExchange(Raw32(cs + 0x18), 0);
}

// KfAcquireSpinLock(lock_ptr) -> irql anterior ; KfReleaseSpinLock(lock_ptr, old_irql)
static thread_local uint8_t t_irql = 0;
GUEST_IMPL(KfAcquireSpinLock)
{
    LONG me = MeBE();
    for (uint32_t spins = 0; InterlockedCompareExchange(Raw32(ctx.r3.u32), me, 0) != 0; spins++)
        if (spins > 64) SwitchToThread(); else YieldProcessor();
    uint8_t old = t_irql; t_irql = 2; // DISPATCH_LEVEL
    ctx.r3.u64 = old;
}
GUEST_IMPL(KfReleaseSpinLock) { InterlockedExchange(Raw32(ctx.r3.u32), 0); t_irql = (uint8_t)ctx.r4.u32; }
GUEST_IMPL(NtYieldExecution) { SwitchToThread(); ctx.r3.u64 = 0; }

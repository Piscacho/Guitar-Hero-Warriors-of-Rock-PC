// GPU provisional (sesion 4, antesala de la sesion de GPU).
// No dibuja nada: acepta la inicializacion de video, "consume" el ring buffer al instante
// (copia el write pointer al read pointer) y genera interrupciones de VBlank a 60 Hz.
// Valores segun la documentacion de Xenia (BSD).
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <windows.h>
float GH6PanoAspect();   // gpu_d3d12.cpp (sesion 25): proporcion panoramica, 0 = 16:9
float GH6PanoCameraAspect();   // sesion 28: 16:9 en los menus con la escena 3D comprimida
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "gpu.h"
namespace
{
constexpr uint32_t kD1ModeVblankStatus = 0x7FC86544; // D1MODE_VBLANK_STATUS: la rutina de D3D solo atiende el VBlank si el bit 0 esta a 1
std::once_flag g_threadsOnce;
}
std::atomic<uint64_t> g_vblankCount{ 0 };   // vsyncs emulados (gpu_cp.cpp lo usa para presentar solo el ultimo swap de cada vsync)
namespace
{

void StartGpuThreads()
{
    GpuStartCommandProcessor();
    GpuStartWindow();
    // VBlank a 60 Hz: interrupcion source 0 en la CPU 2 (como Xenia)
    std::thread([] {
        auto* ctx = new PPCContext{};
        InitGuestThread(*ctx, 0x10000);
        // Sesion 24: antes Sleep(16), que con la resolucion por defecto de Windows (15,6 ms) dormia ~31 ms: el juego recibia
        // ~32 vblanks por segundo y, como sincroniza sus frames con ellos, iba a ~30 fps (y la escena 3D, que se redibuja uno
        // de cada dos frames, a ~15). Ahora: temporizador de alta resolucion con plazos absolutos (sin deriva) a 60 Hz.
        // GH6_VBLANKHZ=<hz> cambia la frecuencia; GH6_OLDVBLANK=1 = antes.
        static const bool oldVblank = getenv("GH6_OLDVBLANK") != nullptr;
        double hz = getenv("GH6_VBLANKHZ") ? atof(getenv("GH6_VBLANKHZ")) : 60.0;
        if (hz < 10.0 || hz > 240.0) hz = 60.0;
        HANDLE timer = oldVblank ? nullptr : CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!oldVblank && !timer) timeBeginPeriod(1);   // sin temporizador de alta resolucion (Windows antiguos): Sleep a 1 ms
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        LARGE_INTEGER freq, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&now);
        const double period = double(freq.QuadPart) / hz;
        double next = double(now.QuadPart);
        for (;;)
        {
            if (oldVblank) Sleep(16);
            else
            {
                next += period;
                QueryPerformanceCounter(&now);
                double remain = next - double(now.QuadPart);
                if (remain < -4.0 * period) next = double(now.QuadPart);   // muy atrasado (depurador, suspension): resincronizar
                else if (remain > 0)
                {
                    // Esperar hasta ~1 ms antes del plazo y terminar con una espera activa corta (precision sub-milisegundo)
                    double waitMs = remain * 1000.0 / double(freq.QuadPart) - 1.0;
                    if (waitMs > 0)
                    {
                        if (timer)
                        {
                            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
                            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                            WaitForSingleObject(timer, INFINITE);
                        }
                        else Sleep((DWORD)waitMs);
                    }
                    do { YieldProcessor(); QueryPerformanceCounter(&now); } while (double(now.QuadPart) < next);
                }
            }
            g_vblankCount++;
            {   // Sesion 25 (experimento panoramico): GH6_ASPECTO = proporcion de la camara del juego (global 829A2584, 16/9 por defecto)

                static const float aspFix = getenv("GH6_ASPECTO") ? (float)atof(getenv("GH6_ASPECTO")) : 0.0f;
                const float asp = aspFix > 0 ? aspFix : GH6PanoCameraAspect();
                if (asp > 0) { uint32_t want; memcpy(&want, &asp, 4); if (Load32(0x829A2584) != want && Load32(0x829A2584) != 0) Store32(0x829A2584, want); }
            }
            Store32(kD1ModeVblankStatus, 1);
            GpuDispatchInterrupt(*ctx, 0, 2);
            {   // GH6_VBLANKLOG=<dir hex>: cada 5 s, cuanto avanzo ese contador de 64 bits del juego por vblank (diagnostico)
                static uint32_t watch = getenv("GH6_VBLANKLOG") ? (uint32_t)strtoul(getenv("GH6_VBLANKLOG"), nullptr, 16) : 0;
                static uint64_t lastV = 0, lastC = 0;
                if (watch && g_vblankCount % 300 == 0)
                {
                    uint64_t c = ((uint64_t)Load32(watch) << 32) | Load32(watch + 4);
                    LOG("[vblank] %llu vblanks: contador %08X = %llu (+%llu en 300 vblanks)", (unsigned long long)g_vblankCount.load(), watch, (unsigned long long)c, (unsigned long long)(c - lastC));
                    lastV = g_vblankCount; lastC = c;
                }
            }
        }
    }).detach();
}
} // namespace

GUEST_IMPL(VdInitializeEngines) { std::call_once(g_threadsOnce, StartGpuThreads); ctx.r3.u64 = 1; }
GUEST_IMPL(VdShutdownEngines) {}
GUEST_IMPL(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_IMPL(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_IMPL(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_IMPL(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
GUEST_IMPL(VdSetDisplayMode) { ctx.r3.u64 = 0; }
GUEST_IMPL(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
GUEST_IMPL(VdQueryVideoMode) { __imp__XGetVideoMode(ctx, base); }
GUEST_IMPL(VdQueryVideoFlags) { ctx.r3.u64 = 0x3; } // panoramico | >= 1024 de ancho
// VdGetCurrentDisplayGamma(type_ptr, power_ptr)
GUEST_IMPL(VdGetCurrentDisplayGamma)
{
    Store32(ctx.r3.u32, 1);
    float p = 2.22222233f; uint32_t b; memcpy(&b, &p, 4); Store32(ctx.r4.u32, b);
}
// VdGetCurrentDisplayInformation(X_DISPLAY_INFO*)
GUEST_IMPL(VdGetCurrentDisplayInformation)
{
    uint32_t p = ctx.r3.u32;
    memset(g_base + p, 0, 0x58);
    static const uint16_t vw = getenv("GH6_ANCHO") ? (uint16_t)atoi(getenv("GH6_ANCHO")) : 1280;   // sesion 25 (experimento)
    Store16(p + 0x00, vw); Store16(p + 0x02, 720);   // front buffer
    Store16(p + 0x50, vw); Store16(p + 0x52, 720);   // display
    float hz = 60.0f; uint32_t b; memcpy(&b, &hz, 4); Store32(p + 0x54, b);
}
// VdInitializeRingBuffer(base_ptr, size_log2)
GUEST_IMPL(VdInitializeRingBuffer) { GpuSetRingBuffer(ctx.r3.u32, ctx.r4.u32); LOG("[gpu] ring buffer 0x%08X (2^%u)", ctx.r3.u32, ctx.r4.u32); }
// VdEnableRingBufferRPtrWriteBack(writeback_ptr, block_size_log2)
GUEST_IMPL(VdEnableRingBufferRPtrWriteBack) { GpuSetReadPointerWriteback(ctx.r3.u32); LOG("[gpu] rptr writeback 0x%08X (bloque 2^%u)", ctx.r3.u32, ctx.r4.u32); }
GUEST_IMPL(VdSetSystemCommandBufferGpuIdentifierAddress) {}
// VdGetSystemCommandBuffer(p0, p1)
GUEST_IMPL(VdGetSystemCommandBuffer) { Store32(ctx.r3.u32, 0xBEEF0000); Store32(ctx.r4.u32, 0xBEEF0001); }
// VdSetGraphicsInterruptCallback(callback, user_data)
GUEST_IMPL(VdSetGraphicsInterruptCallback)
{
    GpuSetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
    LOG("[gpu] callback de interrupciones 0x%08X (user 0x%08X)", ctx.r3.u32, ctx.r4.u32);
}
// VdSwap(buffer_ptr, fetch_ptr, unk2, unk3, unk4, frontbuffer_ptr, texture_format_ptr, color_space_ptr, [pila] width_ptr, height_ptr)
// El juego reserva 64 dwords en el ring buffer: escribimos ahi un paquete XE_SWAP (opcode 0x64, como Xenia).
GUEST_IMPL(VdSwap)
{
    uint32_t buf = ctx.r3.u32;
    uint32_t widthPtr = Load32(ctx.r1.u32 + 0x54), heightPtr = Load32(ctx.r1.u32 + 0x5C);
    uint32_t fb = ctx.r8.u32 ? Load32(ctx.r8.u32) : 0;
    Store32(buf + 0, (3u << 30) | ((63u - 1) << 16) | (0x64u << 8));
    Store32(buf + 4, 0x53574150); // 'SWAP'
    Store32(buf + 8, fb & 0x1FFFFFFF);
    Store32(buf + 12, widthPtr ? Load32(widthPtr) : 1280);
    Store32(buf + 16, heightPtr ? Load32(heightPtr) : 720);
    ctx.r3.u64 = 0;
}

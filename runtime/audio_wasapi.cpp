// Salida de audio (sesion 21): WASAPI en modo compartido, estereo float a 48 kHz (Windows convierte al formato del dispositivo).
// El juego entrega frames de 256 muestras x 6 canales (5.1) con XAudioSubmitRenderDriverFrame; aqui llegan ya mezclados a estereo.
// GH6_NOAUDIO=1 desactiva la salida.
#include "runtime.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace
{
constexpr uint32_t kRate = 48000;
constexpr size_t kRingFrames = kRate / 2;          // 0,5 s de margen
constexpr size_t kMaxLatencyFrames = kRate / 4;    // si se acumulan mas de 125 ms se descarta lo antiguo
std::mutex g_mutex;
std::vector<float> g_ring(kRingFrames * 2);
size_t g_read = 0, g_count = 0;                    // en frames estereo
std::atomic<bool> g_started{ false }, g_running{ false };
std::atomic<uint64_t> g_underruns{ 0 }, g_drops{ 0 };
float g_peak = 0; uint64_t g_frames = 0;   // protegidos por g_mutex
uint64_t g_dropouts = 0, g_dropoutsWin = 0;
std::atomic<uint64_t> g_submitted{ 0 };
std::atomic<uint32_t> g_lastPad{ 0 };   // relleno de WASAPI en la ultima vuelta (para el contador en pantalla)   // frames estereo entregados por el juego desde el arranque (medida de latencia)
// Medida del retraso de salida (sesion 24): cola propia + relleno de WASAPI + latencia del motor de audio de Windows
std::atomic<uint64_t> g_padSum{ 0 }, g_queueSum{ 0 }, g_samples{ 0 }; std::atomic<uint32_t> g_streamLatFrames{ 0 };

void RenderThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)))
    { LOG("[wasapi] no hay dispositivo de salida"); return; }
    {   // nombre del dispositivo de salida predeterminado (sesion 24: para diagnosticar "no se oye")
        IPropertyStore* ps = nullptr; PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        { char name[256]; WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof(name), nullptr, nullptr); LOG("[wasapi] dispositivo: %s", name); }
        PropVariantClear(&pv); if (ps) ps->Release();
    }
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = kRate;
    wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = kRate * 8; wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Sesion 24: modo de baja latencia de Windows 10+ (IAudioClient3): periodo minimo del motor de audio (p. ej. 3 ms en vez de
    // 10 ms) cuando el formato coincide con el del motor. Si el driver no lo admite se usa el modo compartido normal.
    // GH6_NOLOWLAT=1 lo desactiva (lo pone la latencia "segura" del launcher).
    UINT32 lowPeriod = 0;
    if (!getenv("GH6_NOLOWLAT"))
    {
        IAudioClient3* c3 = nullptr;
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), (void**)&c3)))
        {
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            HRESULT h3 = c3->GetSharedModeEnginePeriod(&wf.Format, &def, &fund, &mn, &mx);
            if (SUCCEEDED(h3) && mn && mn < def) h3 = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, &wf.Format, nullptr);
            else if (SUCCEEDED(h3)) h3 = E_NOTIMPL;   // el driver no ofrece periodos menores que el normal
            if (SUCCEEDED(h3)) lowPeriod = mn;
            LOG("[wasapi] baja latencia: %s (periodos del motor: normal %u, minimo %u frames)", lowPeriod ? "activa" : "no disponible", def, mn);
            c3->Release();
            if (!lowPeriod)
            {   // un Initialize fallido puede dejar el cliente inutilizable: se pide uno nuevo
                client->Release(); client = nullptr;
                if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) { LOG("[wasapi] no se pudo reabrir el dispositivo"); return; }
            }
        }
    }
    HRESULT hr = S_OK;
    if (!lowPeriod)
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            600000 /* 60 ms */, 0, &wf.Format, nullptr);
    if (FAILED(hr)) { LOG("[wasapi] Initialize fallo (0x%08lX)", (unsigned long)hr); return; }
    client->SetEventHandle(ev);
    client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    UINT32 bufFrames = 0; client->GetBufferSize(&bufFrames);
    // Sesion 24 (retraso): solo se mantienen ~GH6_WASAPIMS ms (por defecto 20, minimo 2 periodos del dispositivo) en el buffer
    // de WASAPI; antes se llenaba entero (60 ms) y sumaba ese retraso a cada sonido.
    REFERENCE_TIME defPeriod = 100000, minPeriod = 0; client->GetDevicePeriod(&defPeriod, &minPeriod);
    int wasapiMs = getenv("GH6_WASAPIMS") ? atoi(getenv("GH6_WASAPIMS")) : 20;
    UINT32 periodFrames = lowPeriod ? lowPeriod : (UINT32)(defPeriod * kRate / 10000000);
    // con baja latencia basta con 2 periodos (unos 6 ms) en el buffer de WASAPI
    UINT32 targetPad = lowPeriod ? std::min<UINT32>(bufFrames, std::max<UINT32>(2 * lowPeriod, kRate * 4 / 1000))
                                 : std::min<UINT32>(bufFrames, std::max<UINT32>((UINT32)(kRate * std::max(wasapiMs, 1) / 1000), 2 * periodFrames));
    REFERENCE_TIME lat = 0; client->GetStreamLatency(&lat); g_streamLatFrames = (uint32_t)(lat * kRate / 10000000);
    client->Start();
    g_running = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);   // sesion 23
    LOG("[wasapi] salida iniciada: estereo float %u Hz, buffer %u frames, objetivo %u frames (periodo %u)", kRate, bufFrames, targetPad, periodFrames);
    for (;;)
    {
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0; if (FAILED(client->GetCurrentPadding(&pad))) continue;
        { size_t q; { std::lock_guard l(g_mutex); q = g_count; } g_padSum += pad; g_queueSum += q; g_samples++; g_lastPad = pad; }
        UINT32 want = targetPad > pad ? targetPad - pad : 0;
        if (!want) continue;
        BYTE* data = nullptr; if (FAILED(render->GetBuffer(want, &data))) continue;
        float* out = (float*)data;
        {
            std::lock_guard l(g_mutex);
            UINT32 n = (UINT32)std::min<size_t>(want, g_count);
            for (UINT32 i = 0; i < n; i++)
            {
                size_t r = (g_read + i) % kRingFrames;
                out[i * 2] = g_ring[r * 2]; out[i * 2 + 1] = g_ring[r * 2 + 1];
            }
            g_read = (g_read + n) % kRingFrames; g_count -= n;
            if (n < want) { memset(out + n * 2, 0, (want - n) * 8); if (g_started) g_underruns++; }
        }
        render->ReleaseBuffer(want, 0);
    }
}
} // namespace

// Mezcla un frame del XAudio de la 360 (256 muestras x 6 canales planares, float big-endian; orden FL FR FC LFE BL BR) a estereo
void AudioSubmitXenonFrame(const uint8_t* src)
{
    static bool disabled = getenv("GH6_NOAUDIO") != nullptr;
    if (disabled) return;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(RenderThread).detach(); });
    auto ch = [src](int c, int i) { uint32_t v = __builtin_bswap32(((const uint32_t*)src)[c * 256 + i]); float f; memcpy(&f, &v, 4); return f; };
    std::lock_guard l(g_mutex);
    g_started = true;
    // GH6_WAVDUMP=<archivo>: guarda la mezcla estereo (float 48 kHz) que se envia a WASAPI, hasta 10 minutos (diagnostico, sesion 24)
    static FILE* wav = [] {
        const char* pth = getenv("GH6_WAVDUMP"); if (!pth) return (FILE*)nullptr;
        FILE* f = fopen(pth, "wb"); if (!f) return f;
        uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 3,0, 2,0 };
        *(uint32_t*)(hdr + 24) = kRate; *(uint32_t*)(hdr + 28) = kRate * 8; *(uint16_t*)(hdr + 32) = 8; *(uint16_t*)(hdr + 34) = 32;
        memcpy(hdr + 36, "data", 4); fwrite(hdr, 1, 44, f); return f; }();
    static uint64_t wavFrames = 0;
    g_submitted += 256;
    for (int i = 0; i < 256; i++)
    {
        float c = ch(2, i) * 0.7071f;
        float L = ch(0, i) + c + ch(4, i) * 0.7071f;
        float R = ch(1, i) + c + ch(5, i) * 0.7071f;
        if (g_count >= kMaxLatencyFrames) { g_read = (g_read + 1) % kRingFrames; g_count--; g_drops++; }
        size_t w = (g_read + g_count) % kRingFrames;
        g_ring[w * 2] = std::clamp(L, -1.0f, 1.0f); g_ring[w * 2 + 1] = std::clamp(R, -1.0f, 1.0f);
        g_count++;
        g_peak = std::max(g_peak, std::max(std::fabs(L), std::fabs(R)));
        if (wav && wavFrames < uint64_t(kRate) * 600) { float lr[2] = { L, R }; fwrite(lr, 4, 2, wav); wavFrames++;
            if ((wavFrames & 0x3FFF) == 0) { long pos = ftell(wav); uint32_t d = (uint32_t)(wavFrames * 8), r = d + 36;
                fseek(wav, 4, SEEK_SET); fwrite(&r, 4, 1, wav); fseek(wav, 40, SEEK_SET); fwrite(&d, 4, 1, wav); fseek(wav, pos, SEEK_SET); fflush(wav); } }
    }
    // Caidas bruscas a silencio en la mezcla del juego (sesion 23): frame de 256 muestras casi mudo tras uno con sonido
    {
        float fp = 0;
        for (int i = 0; i < 256; i++) fp = std::max(fp, std::max(std::fabs(ch(0, i)), std::fabs(ch(1, i))));
        static float lastFp = 0;
        if (fp < 0.0005f && lastFp > 0.05f) { g_dropouts++; g_dropoutsWin++; }
        lastFp = fp;
    }
    // Nivel de pico cada ~5 s (para comprobar en el log que llega sonido de verdad)
    if (++g_frames % 940 == 0)
    {
        LOG("[wasapi] t=%lus pico %.3f, en cola %zu frames, huecos %llu, descartes %llu, cortes en la mezcla %llu (total %llu)", GetTickCount() / 1000, g_peak, g_count,
            (unsigned long long)g_underruns.load(), (unsigned long long)g_drops.load(), (unsigned long long)g_dropoutsWin, (unsigned long long)g_dropouts);
        g_peak = 0; g_dropoutsWin = 0;
        uint64_t n = g_samples.exchange(0), ps = g_padSum.exchange(0), qs = g_queueSum.exchange(0);
        if (n) LOG("[wasapi] retraso de salida ~%.0f ms (cola %.0f + WASAPI %.0f + Windows %.0f)", (qs + ps) / (double)n / 48.0 + g_streamLatFrames / 48.0,
                   qs / (double)n / 48.0, ps / (double)n / 48.0, g_streamLatFrames / 48.0);
    }
}
uint64_t AudioSubmitted() { return g_submitted.load(); }
// Retraso de salida aproximado ahora mismo (cola propia + buffer de WASAPI + latencia del motor), en ms; 0 si no hay audio
float AudioLatencyNowMs()
{
    if (!g_running) return 0;
    size_t q; { std::lock_guard l(g_mutex); q = g_count; }
    return (q + g_lastPad.load() + g_streamLatFrames.load()) / 48.0f;
}
size_t AudioQueuedFrames() { std::lock_guard l(g_mutex); return g_count; }
bool AudioOutputActive() { return g_running.load(); }
uint64_t AudioUnderruns() { return g_underruns.load(); }
uint64_t AudioDrops() { return g_drops.load(); }

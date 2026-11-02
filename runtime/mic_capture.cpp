// Microfono del sistema (sesion 24): captura WASAPI en modo compartido, convertida por Windows a 48 kHz, mono, PCM de 16 bits
// (AUTOCONVERTPCM). El microfono USB emulado de la 360 (MicDeviceRequest, xam.cpp) lee de este anillo a ritmo de 1 ms por paquete.
// GH6_MIC=0 desactiva el microfono; GH6_MIC_GANANCIA = multiplicador (por defecto 1.0).
#include "runtime.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace
{
constexpr uint32_t kRate = 48000;
constexpr size_t kRing = kRate;                    // 1 s
constexpr size_t kMaxLatency = kRate * 60 / 1000;  // mas de 60 ms acumulados: se descarta lo antiguo
std::mutex g_mutex;
std::vector<int16_t> g_ring(kRing);
size_t g_read = 0, g_count = 0;
std::atomic<bool> g_started{ false }, g_ok{ false };
uint64_t g_captured = 0, g_underruns = 0, g_drops = 0;
float g_peak = 0;

void Push(const int16_t* s, size_t n)
{
    std::lock_guard l(g_mutex);
    for (size_t i = 0; i < n; i++)
    {
        g_ring[(g_read + g_count) % kRing] = s[i];
        if (g_count < kRing) g_count++; else g_read = (g_read + 1) % kRing;
        float a = s[i] < 0 ? -s[i] / 32768.0f : s[i] / 32767.0f; if (a > g_peak) g_peak = a;
    }
    if (g_count > kMaxLatency) { size_t d = g_count - kMaxLatency; g_read = (g_read + d) % kRing; g_count -= d; g_drops += d; }
    g_captured += n;
}

// GH6_MIC_DISPOSITIVO = identificador de WASAPI del microfono (lo escribe el launcher); vacio = el predeterminado de Windows
HRESULT OpenDevice(IMMDeviceEnumerator* en, IMMDevice** dev)
{
    const char* id = getenv("GH6_MIC_DISPOSITIVO");
    if (id && *id)
    {
        wchar_t w[512]; MultiByteToWideChar(CP_UTF8, 0, id, -1, w, 512);
        if (SUCCEEDED(en->GetDevice(w, dev))) return S_OK;
        LOG("[mic] el microfono elegido no esta conectado: se usa el predeterminado");
    }
    return en->GetDefaultAudioEndpoint(eCapture, eConsole, dev);
}
void CaptureThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* client = nullptr; IAudioCaptureClient* cap = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
        FAILED(OpenDevice(en, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)))
    { LOG("[mic] no hay microfono en Windows (dispositivo de grabacion predeterminado)"); return; }
    {
        IPropertyStore* ps = nullptr; PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        { char name[256]; WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof(name), nullptr, nullptr); LOG("[mic] microfono de Windows: %s", name); }
        PropVariantClear(&pv); if (ps) ps->Release();
    }
    WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = kRate; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2; wf.nAvgBytesPerSec = kRate * 2;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    200000 /* 20 ms */, 0, &wf, nullptr);
    if (FAILED(hr) || FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)))
    { LOG("[mic] no se pudo abrir el microfono de Windows (0x%08lX)", (unsigned long)hr); return; }
    client->Start();
    g_ok = true;
    LOG("[mic] captura iniciada: 48 kHz mono 16 bits");
    static const float gain = getenv("GH6_MIC_GANANCIA") ? (float)atof(getenv("GH6_MIC_GANANCIA")) : 1.0f;
    std::vector<int16_t> tmp;
    for (;;)
    {
        Sleep(2);
        UINT32 pkt = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt)
        {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            tmp.resize(frames);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(tmp.begin(), tmp.end(), (int16_t)0);
            else
            {
                const int16_t* s = (const int16_t*)data;
                for (UINT32 i = 0; i < frames; i++) tmp[i] = (int16_t)std::clamp((int)(s[i] * gain), -32768, 32767);
            }
            cap->ReleaseBuffer(frames);
            Push(tmp.data(), frames);
        }
    }
}
} // namespace

// Microfonos activos de Windows (identificador, nombre), para el launcher
void MicListDevices(std::vector<std::pair<std::wstring, std::wstring>>& out)
{
    out.clear();
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)))
    {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++)
        {
            IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr; d->GetId(&id);
            std::wstring name = L"?";
            IPropertyStore* ps = nullptr; PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
            PropVariantClear(&pv); if (ps) ps->Release();
            if (id) { out.emplace_back(id, name); CoTaskMemFree(id); }
            d->Release();
        }
        col->Release();
    }
    en->Release();
}
void MicCaptureStart()
{
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) std::thread(CaptureThread).detach();
}
// Lee n muestras (48 kHz mono s16, orden del PC = little endian); rellena con silencio si no hay suficientes
void MicCaptureRead(int16_t* out, size_t n)
{
    // GH6_MIC_TONO=<Hz> (diagnostico): tono sinusoidal en vez del microfono, para comprobar que el juego detecta la afinacion
    static const double tone = getenv("GH6_MIC_TONO") ? atof(getenv("GH6_MIC_TONO")) : 0;
    if (tone != 0)
    {   // tono > 0: fijo; tono < 0: barrido de 110 a 440 Hz y vuelta cada 4 s
        static double ph = 0; static uint64_t t = 0;
        for (size_t i = 0; i < n; i++, t++)
        {
            double f = tone;
            if (tone < 0) { double x = fmod(t / (double)kRate, 4.0) / 2.0; if (x > 1) x = 2 - x; f = 110.0 * pow(4.0, x); }
            out[i] = (int16_t)(sin(ph) * 12000); ph += 6.283185307179586 * f / kRate; if (ph > 6.283185307179586) ph -= 6.283185307179586;
        }
        return;
    }
    std::lock_guard l(g_mutex);
    size_t k = std::min(n, g_count);
    for (size_t i = 0; i < k; i++) out[i] = g_ring[(g_read + i) % kRing];
    g_read = (g_read + k) % kRing; g_count -= k;
    if (k < n) { std::memset(out + k, 0, (n - k) * 2); if (g_ok) g_underruns++; }
    static uint64_t reads = 0;
    if (++reads % 1250 == 0)   // ~cada 10 s (lecturas de 8 ms)
    {
        LOG("[mic] capturadas %llu muestras, pico %.3f, huecos %llu, descartes %llu", (unsigned long long)g_captured, g_peak, (unsigned long long)g_underruns, (unsigned long long)g_drops);
        g_peak = 0;
    }
}

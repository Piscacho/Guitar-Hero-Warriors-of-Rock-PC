// "Hardware" XMA de la Xbox 360 (sesion 11): contextos en memoria, registros MMIO y decodificacion con FFmpeg (xma2).
// Disposicion del contexto y registros segun Xenia (BSD): apu/xma_context.h, xma_register_table.inc, xma_decoder.cc.
// Diferencia con Xenia: se usa el decodificador xma2 oficial de FFmpeg alimentado con paquetes completos de 2048 bytes.
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <mmsystem.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace
{
constexpr uint32_t kMmio = 0x7FEA0000;
constexpr uint32_t kRegContextArray = 0x600, kRegCurrentContext = 0x606, kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0;
constexpr uint32_t kContexts = 320, kContextSize = 64, kPacketSize = 2048;

uint32_t g_arrayGuest = 0;      // direccion de invitado (0xA...) del array de contextos
std::mutex g_allocMutex;
std::vector<bool> g_used(kContexts, false);
std::vector<bool> g_everKicked(kContexts, false);
bool g_xmaLog = getenv("GH6_XMALOG") != nullptr;
std::atomic<uint64_t> g_kicks{ 0 }, g_packets{ 0 }, g_samples{ 0 }, g_errors{ 0 }, g_starves{ 0 };
uint32_t g_ctxWritten[320], g_ctxStarves[320], g_ctxKicks[320];
// Sesion 25 (desfase): muestras escritas por contexto desde su primera escritura y la hora de esa escritura (ritmo real de consumo)
uint64_t g_ctxTotal[320]; LARGE_INTEGER g_ctxFirst[320]; uint32_t g_ctxRate[320];   // por contexto, desde el ultimo informe (sesion 24)

inline volatile LONG* RegPtr(uint32_t r) { return (volatile LONG*)(g_base + kMmio + r * 4); }
inline uint32_t Ctx(uint32_t i) { return g_arrayGuest + i * kContextSize; }
inline uint32_t Dw(uint32_t c, int n) { return Load32(c + n * 4); }
inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline uint32_t Phys(uint32_t p) { return 0xA0000000u | (p & 0x1FFFFFFF); }

// Modificacion atomica de bits de un dword BE del contexto (el juego puede tocar otros bits a la vez)
void UpdateBits(uint32_t addr, uint32_t clearMask, uint32_t setBits)
{
    volatile LONG* p = (volatile LONG*)(g_base + addr);
    for (;;)
    {
        LONG oldRaw = *p;
        uint32_t v = __builtin_bswap32((uint32_t)oldRaw);
        uint32_t nv = (v & ~clearMask) | setBits;
        if (InterlockedCompareExchange(p, (LONG)__builtin_bswap32(nv), oldRaw) == oldRaw) return;
    }
}

struct Decoder
{
    bool enabled = false;
    AVCodecContext* av = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int channels = 0;
    std::vector<int16_t> pending;   // muestras decodificadas aun no escritas (intercaladas)
    void Reset()
    {
        if (av) avcodec_free_context(&av);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        channels = 0; pending.clear();
    }
    bool Open(int ch, int rate)
    {
        Reset();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec) { LOG("[xma] FFmpeg sin decodificador xma2"); return false; }
        av = avcodec_alloc_context3(codec);
        av_channel_layout_default(&av->ch_layout, ch);
        av->sample_rate = rate;
        // XMA2WAVEFORMATEX minimo (34 bytes): [1] = numero de streams, [2..5] = mascara de canales
        av->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
        av->extradata_size = 34;
        av->extradata[0] = 1; av->extradata[1] = 0;     // wNumStreams = 1 (LE16)
        uint32_t mask = ch == 2 ? 3 : 4;
        memcpy(av->extradata + 2, &mask, 4);
        int err = avcodec_open2(av, codec, nullptr);
        if (err < 0) { char e[128]; av_strerror(err, e, sizeof(e)); LOG("[xma] avcodec_open2 fallo: %s", e); Reset(); return false; }
        pkt = av_packet_alloc(); frame = av_frame_alloc();
        channels = ch;
        return true;
    }
};
Decoder g_dec[kContexts];
std::mutex g_decMutex;

// Decodifica un paquete y deja las muestras en pending
void DecodePacket(Decoder& d, const uint8_t* data)
{
    av_new_packet(d.pkt, kPacketSize);
    memcpy(d.pkt->data, data, kPacketSize);
    int r = avcodec_send_packet(d.av, d.pkt);
    av_packet_unref(d.pkt);
    if (r < 0) { g_errors++; return; }
    while (avcodec_receive_frame(d.av, d.frame) == 0)
    {
        int n = d.frame->nb_samples, ch = d.channels;
        size_t base = d.pending.size();
        d.pending.resize(base + size_t(n) * ch);
        for (int c = 0; c < ch; c++)
        {
            const float* src = (const float*)d.frame->extended_data[c < d.frame->ch_layout.nb_channels ? c : 0];
            for (int i = 0; i < n; i++)
            {
                float f = src[i] * 32767.0f;
                f = f > 32767.0f ? 32767.0f : (f < -32768.0f ? -32768.0f : f);
                d.pending[base + size_t(i) * ch + c] = (int16_t)f;
            }
        }
        g_samples += n;
        av_frame_unref(d.frame);
    }
}

// Una pasada de decodificacion por kick (como Xenia): solo frames completos que quepan en la salida circular.
// Si la salida se llena (escritura == lectura) o no queda entrada, se pone output_buffer_valid = 0.
// Devuelve el numero de muestras (por canal) disponibles en pending tras asegurar al menos 'need'.
static bool EnsureSamples(uint32_t c, Decoder& d, size_t need)
{
    for (int guard = 0; d.pending.size() < need && guard < 64; guard++)
    {
        uint32_t d0 = Dw(c, 0), d4 = Dw(c, 4);
        uint32_t cur = Bits(d4, 31, 1);
        bool valid = cur ? Bits(d0, 21, 1) : Bits(d0, 20, 1);
        if (!valid)
        {
            bool otherValid = cur ? Bits(d0, 20, 1) : Bits(d0, 21, 1);
            if (!otherValid) return false;                       // sin datos de entrada
            UpdateBits(c + 16, 1u << 31, (cur ^ 1) << 31);       // cambiar de buffer
            UpdateBits(c + 8, 0x3FFFFFF, 32);
            continue;
        }
        uint32_t count = cur ? Bits(Dw(c, 1), 0, 12) : Bits(d0, 0, 12);
        uint32_t buf = Phys(Dw(c, cur ? 6 : 5));
        uint32_t packet = Bits(Dw(c, 2), 0, 26) / (kPacketSize * 8);
        if (packet >= count)
        {
            // buffer consumido: invalidarlo y pasar al otro. Sesion 24: los paquetes que sobran del salto (packet_skip_count)
            // se aplican al principio del buffer siguiente, como Xenia.
            static bool noSkip = getenv("GH6_XMA_NOSKIP") != nullptr;
            uint32_t carry = noSkip ? 0 : packet - count;
            UpdateBits(c + 0, 1u << (cur ? 21 : 20), 0);
            UpdateBits(c + 16, 1u << 31, (cur ^ 1) << 31);
            UpdateBits(c + 8, 0x3FFFFFF, carry * kPacketSize * 8 + 32);
            continue;
        }
        const uint8_t* pk = g_base + buf + packet * kPacketSize;
        DecodePacket(d, pk);
        g_packets++;
        // Sesion 24: XMA con varios streams intercalados (las pistas de las canciones: guitarra, bajo, bateria...). El byte 3 de la
        // cabecera del paquete (packet_skip_count) dice cuantos paquetes de otros streams hay que saltar hasta el siguiente de este.
        // Antes se pasaba siempre al paquete siguiente y el decodificador mezclaba pistas: audio entrecortado y la guitarra no
        // quedaba aislada (no sonaba al acertar notas). GH6_XMA_NOSKIP=1 = comportamiento anterior.
        static bool noSkip2 = getenv("GH6_XMA_NOSKIP") != nullptr;
        uint32_t skip = noSkip2 ? 0 : pk[3];
        if (skip == 0xFF) skip = 0;   // valor especial no visto en el juego; por seguridad
        { static int sl = 0; if (skip && sl < 10) { sl++; LOG("[xma] packet_skip_count %u en ctx %08X (paquete %u de %u)", skip, c, packet, count); } }
        UpdateBits(c + 8, 0x3FFFFFF, (packet + 1 + skip) * kPacketSize * 8 + 32);
    }
    return d.pending.size() >= need;
}

void WorkContext(uint32_t i)
{
    Decoder& d = g_dec[i];
    uint32_t c = Ctx(i);
    uint32_t d0 = Dw(c, 0), d1 = Dw(c, 1), d9 = Dw(c, 9);
    if (!Bits(d1, 31, 1)) return;                                  // output_buffer_valid = 0: nada que hacer
    int ch = Bits(d1, 29, 1) ? 2 : 1;
    static const int rates[4] = { 24000, 32000, 44100, 48000 };
    if (!d.av || d.channels != ch) { if (!d.Open(ch, rates[Bits(d1, 27, 2)])) return; }
    if (!Bits(d0, 20, 1) && !Bits(d0, 21, 1) && d.pending.empty()) { UpdateBits(c + 4, 1u << 31, 0); return; }
    uint32_t blocks = Bits(d0, 22, 5);
    if (!blocks) return;
    uint32_t cap = blocks * 256, out = Phys(Dw(c, 7));
    uint32_t wr = Bits(d0, 27, 5) * 256, rd = Bits(d9, 0, 5) * 256;
    uint32_t used = wr >= rd ? wr - rd : cap - (rd - wr);
    uint32_t freeBytes = cap - used;
    // Granularidad de subframe: 128 muestras por canal (256 bytes por canal), como el hardware
    const uint32_t subSamples = 128u * ch, subBytes = subSamples * 2;
    bool wrote = false;
    while (freeBytes >= subBytes)
    {
        if (!EnsureSamples(c, d, subSamples)) { g_starves++; g_ctxStarves[i]++; break; }   // hueco en la salida pero sin entrada (sesion 23)
        g_ctxWritten[i] += 128;
        if (!g_ctxTotal[i]) { QueryPerformanceCounter(&g_ctxFirst[i]); g_ctxRate[i] = rates[Bits(d1, 27, 2)]; }
        g_ctxTotal[i] += 128;
        for (uint32_t k = 0; k < subSamples; k++)
        {
            *(uint16_t*)(g_base + out + wr) = __builtin_bswap16((uint16_t)d.pending[k]);
            wr += 2; if (wr >= cap) wr = 0;
        }
        d.pending.erase(d.pending.begin(), d.pending.begin() + subSamples);
        freeBytes -= subBytes;
        wrote = true;
    }
    UpdateBits(c + 0, 0x1Fu << 27, (wr / 256) << 27);
    // Lleno (escritura alcanzo a lectura) -> el juego nos volvera a lanzar con un buffer nuevo
    if (wrote && wr == rd) UpdateBits(c + 4, 1u << 31, 0);
    // Sin entrada ni muestras pendientes -> salida invalida
    uint32_t nd0 = Dw(c, 0);
    if (!Bits(nd0, 20, 1) && !Bits(nd0, 21, 1) && d.pending.size() < 128u * ch) UpdateBits(c + 4, 1u << 31, 0);
}

void XmaThread()
{
    uint32_t rotating = 1;
    timeBeginPeriod(1);
    for (;;)
    {
        Sleep(1);
        // CurrentContextIndex (0x606) "rota" como en Xenia: el juego compara su contexto con el que procesa el hardware
        // y, si coinciden siempre, se agota la espera y reinicia el XMA. Nunca con el bit 0x200.
        // CurrentContextIndex: valor neutro (sin el bit 0x200) cuando no se procesa nada; ver WorkContext mas abajo
        (void)rotating;
        // Reinicio del XMA por parte del juego (registro 0x601 = 0x7FEA1804): registrar escrituras
        {
            static uint32_t last601 = 0; static int n601 = 0;
            uint32_t v = *(volatile uint32_t*)(g_base + kMmio + 0x601 * 4);
            if (v != last601 && n601 < 20) { n601++; LOG("[xma] registro 0x601 = %08X (antes %08X)", v, last601); }
            last601 = v;
        }
        // kick / lock / clear (bits por contexto; el juego escribe con stwbrx -> valor nativo en memoria)
        for (uint32_t g = 0; g < 10; g++)
        {
            uint32_t kick = (uint32_t)InterlockedExchange(RegPtr(kRegKick + g), 0);
            uint32_t lock = (uint32_t)InterlockedExchange(RegPtr(kRegLock + g), 0);
            uint32_t clr = (uint32_t)InterlockedExchange(RegPtr(kRegClear + g), 0);
            for (uint32_t b = 0; b < 32; b++)
            {
                uint32_t i = g * 32 + b;
                if (i >= kContexts) break;
                if (clr & (1u << b)) { std::lock_guard l(g_decMutex); g_dec[i].Reset(); g_dec[i].enabled = false; g_ctxTotal[i] = 0; }
                if (lock & (1u << b)) g_dec[i].enabled = false;
                if (kick & (1u << b))
                {
                    g_dec[i].enabled = true; g_kicks++; g_everKicked[i] = true; g_ctxKicks[i]++;
                    static uint8_t perCtx[kContexts]; if (g_xmaLog && perCtx[i] < 3) { perCtx[i]++; uint32_t c = Ctx(i); LOG("[xma] kick ctx %u: %08X %08X %08X %08X %08X in0=%08X in1=%08X out=%08X", i, Dw(c, 0), Dw(c, 1), Dw(c, 2), Dw(c, 3), Dw(c, 4), Dw(c, 5), Dw(c, 6), Dw(c, 7)); }
                }
            }
        }
        std::lock_guard l(g_decMutex);
        // Una pasada por kick. Mientras se procesa el contexto i, CurrentContextIndex = i | 0x200: el juego (sub_82769088)
        // ve la colision y no copia ese contexto hasta que terminamos (asi su "commit" no pisa lo que escribimos).
        for (uint32_t i = 0; i < kContexts; i++) if (g_dec[i].enabled)
        {
            *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = i | 0x200;
            MemoryBarrier();
            WorkContext(i); g_dec[i].enabled = false;
            MemoryBarrier();
            *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = 0x1FF;
        }
        // Experimento (GH6_XMA_IMPLICIT, activo por defecto): procesar contextos con entrada y salida validas aunque no haya kick
        static bool implicitWork = getenv("GH6_XMA_IMPLICIT") != nullptr; // desactivado: rompe el protocolo sync/commit de FMOD
        static uint32_t tick = 0;
        // Cebado (sesion 15): contextos reservados que aun no han recibido ningun kick se decodifican en cuanto tienen entrada y salida
        // validas (el hardware real ya habria empezado). Tras el primer kick manda el protocolo sync/commit.
        static bool prime = getenv("GH6_XMA_NOPRIME") == nullptr;
        if (prime)
            for (uint32_t i = 0; i < kContexts; i++)
            {
                if (!g_used[i] || g_everKicked[i]) continue;
                uint32_t c = Ctx(i), d0 = Dw(c, 0), d1 = Dw(c, 1);
                if (Bits(d1, 31, 1) && (Bits(d0, 20, 1) || Bits(d0, 21, 1)) && Dw(c, 7))
                {
                    *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = i | 0x200;
                    WorkContext(i);
                    *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = 0x1FF;
                }
            }
        if (implicitWork && ++tick % 5 == 0)
            for (uint32_t i = 0; i < kContexts; i++)
            {
                uint32_t c = Ctx(i), d0 = Dw(c, 0), d1 = Dw(c, 1);
                if ((g_used[i] || g_everKicked[i]) && Bits(d1, 31, 1) && (Bits(d0, 20, 1) || Bits(d0, 21, 1)) && Dw(c, 7)) WorkContext(i);
            }
        static DWORD last = GetTickCount();
        if (GetTickCount() - last > 5000)
        {
            last = GetTickCount();
            LOG("[xma] t=%lus kicks %llu, paquetes %llu, muestras %llu, errores %llu, sin entrada %llu", GetTickCount() / 1000, (unsigned long long)g_kicks.load(), (unsigned long long)g_packets.load(),
                (unsigned long long)g_samples.load(), (unsigned long long)g_errors.load(), (unsigned long long)g_starves.load());
            {   // resumen por contexto (sesion 24): muestras escritas / huecos sin entrada / kicks en los ultimos ~5 s
                std::string line; char b[64];
                for (uint32_t i = 0; i < kContexts; i++)
                {
                    if (!g_used[i]) continue;
                    uint32_t d1 = Dw(Ctx(i), 1);
                    snprintf(b, sizeof(b), " %u:%u/%u/%u%s", i, g_ctxWritten[i], g_ctxStarves[i], g_ctxKicks[i], Bits(d1, 29, 1) ? "s" : "m");
                    line += b; g_ctxWritten[i] = g_ctxStarves[i] = g_ctxKicks[i] = 0;
                }
                LOG("[xma] por contexto (escritas/sin entrada/kicks):%s", line.c_str());
                {   // ritmo de consumo desde la primera escritura (solo contextos que siguen activos)
                    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
                    std::string rl;
                    for (uint32_t i = 0; i < kContexts; i++)
                    {
                        if (!g_used[i] || !g_ctxTotal[i]) continue;
                        double el = double(now.QuadPart - g_ctxFirst[i].QuadPart) / f.QuadPart;
                        if (el < 20) continue;
                        snprintf(b, sizeof(b), " %u:%.1fHz/%u(%.0fs)", i, g_ctxTotal[i] / el, g_ctxRate[i], el); rl += b;
                    }
                    if (!rl.empty()) LOG("[xma] ritmo desde el inicio:%s", rl.c_str());
                    // Sesion 25: reloj de la cancion del juego (double en 82993A58, lo avanza sub_823D1490 con el mftb de cada vblank)
                    // junto a las muestras totales de cada contexto, con la hora exacta: para medir la deriva musica/notas
                    uint64_t raw = ((uint64_t)Load32(0x82993A58) << 32) | Load32(0x82993A5C); double songT; memcpy(&songT, &raw, 8);
                    std::string tl; char b2[64];
                    for (uint32_t i = 0; i < kContexts; i++)
                        if (g_used[i] && g_ctxTotal[i] > 48000 * 20) { snprintf(b2, sizeof(b2), " %u:%llu", i, (unsigned long long)g_ctxTotal[i]); tl += b2; }
                    LOG("[songclock] qpc %.4f cancion %.4f muestras%s", double(now.QuadPart) / f.QuadPart, songT, tl.c_str());
                }
            }
            for (uint32_t i = 0; i < 16; i++)
            {
                uint32_t c = Ctx(i);
                if (!g_xmaLog) break;
                if (!g_used[i] && !Dw(c, 5)) continue;
                LOG("[xma]   ctx %u reservado=%d lanzado=%d", i, (int)g_used[i], (int)g_everKicked[i]);
                uint32_t d0 = Dw(c, 0), d1 = Dw(c, 1), d4 = Dw(c, 4), d9 = Dw(c, 9);
                LOG("[xma]   ctx %u: in0 %u pq valido %u | in1 %u pq valido %u | actual %u lect %u bits | salida %u bloques escr %u lect %u valida %u | pendientes %zu",
                    i, Bits(d0, 0, 12), Bits(d0, 20, 1), Bits(d1, 0, 12), Bits(d0, 21, 1), Bits(d4, 31, 1), Bits(Dw(c, 2), 0, 26),
                    Bits(d0, 22, 5), Bits(d0, 27, 5), Bits(d9, 0, 5), Bits(d1, 31, 1), g_dec[i].pending.size());
            }
        }
    }
}
} // namespace

void InitXma()
{
    g_arrayGuest = GuestAllocPhysical(kContexts * kContextSize, 0x1000);
    *(uint32_t*)(g_base + kMmio + kRegContextArray * 4) = g_arrayGuest & 0x1FFFFFFF; // registro LE (lwbrx)
    *(uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = 0;
    std::thread(XmaThread).detach();
    LOG("[xma] array de %u contextos en 0x%08X", kContexts, g_arrayGuest);
}

// XMACreateContext(context_out_ptr)
GUEST_IMPL(XMACreateContext)
{
    std::lock_guard l(g_allocMutex);
    for (uint32_t i = 0; i < kContexts; i++)
        if (!g_used[i])
        {
            g_used[i] = true;
            memset(g_base + Ctx(i), 0, kContextSize);
            if (ctx.r3.u32) Store32(ctx.r3.u32, Ctx(i));
            ctx.r3.u64 = 0;
            return;
        }
    ctx.r3.u64 = 0x8007000E;
}
// XMAReleaseContext(context_ptr)
GUEST_IMPL(XMAReleaseContext)
{
    uint32_t i = (ctx.r3.u32 - g_arrayGuest) / kContextSize;
    if (i < kContexts) { std::lock_guard l(g_allocMutex); g_used[i] = false; std::lock_guard l2(g_decMutex); g_dec[i].Reset(); g_dec[i].enabled = false; g_ctxTotal[i] = 0; }
    ctx.r3.u64 = 0;
}

// Kick directo (sesion 15): llamado desde el gancho del "commit" de FMOD (hooks.cpp), porque los kicks por MMIO
// se escriben con stwbrx de un solo bit y dos seguidos en el mismo grupo se pisan antes de que el sondeo los vea.
void XmaKickContext(uint32_t id)
{
    if (id >= kContexts) return;
    g_kicks++; g_everKicked[id] = true; g_ctxKicks[id]++;
    // "Hardware instantaneo": se decodifica ya, en el hilo del juego, antes de su siguiente sincronizacion
    std::lock_guard l(g_decMutex);
    *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = id | 0x200;
    WorkContext(id);
    g_dec[id].enabled = false;
    *(volatile uint32_t*)(g_base + kMmio + kRegCurrentContext * 4) = 0x1FF;
}

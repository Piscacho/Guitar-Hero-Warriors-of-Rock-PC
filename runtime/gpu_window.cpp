// Ventana del juego (sesion 5): por ahora solo muestra el estado de la GPU (frames, draws).
// La imagen real llegara con el backend de dibujo (D3D12/Vulkan) en la siguiente sesion de GPU.
#include "gpu.h"
#include <windows.h>
#include <thread>
#include <cstdio>
#include <cstring>

#include <vector>
#include <string>
#include <atomic>
bool D3D12Present(HWND hwnd, const uint32_t* pixels, uint32_t w, uint32_t h);
extern std::atomic<bool> g_showStats;   // gpu_d3d12.cpp: contador de FPS (F3)
bool D3D12PresentResolved(HWND hwnd, uint32_t fb, std::vector<uint32_t>* readback, uint32_t& outW, uint32_t& outH);
std::atomic<int> g_dumpRequest{ -1 };   // >= 0: guardar shot_<n>.bmp en el siguiente present
std::atomic<int> g_dumpSeq{ 0 };        // > 0: volcar los siguientes frames seguidos como seq_<n>.bmp
namespace
{
HWND g_hwnd = nullptr;
std::atomic<bool> g_d3d12Active{ false };
std::vector<uint32_t> g_pixels;

// Direccion de un texel en una superficie 2D "tiled" de Xenos (como XGAddress2DTiledOffset / Xenia texture_address)
uint32_t TiledOuter(uint32_t y, uint32_t pitch, uint32_t log2Bpp)
{
    uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2Bpp + 7);
    uint32_t micro = ((y & 6) << 2) << log2Bpp;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
}
uint32_t TiledInner(uint32_t x, uint32_t y, uint32_t log2Bpp, uint32_t baseOffset)
{
    uint32_t macro = (x >> 5) << (log2Bpp + 7);
    uint32_t micro = (x & 7) << log2Bpp;
    uint32_t off = baseOffset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// Copia el frontbuffer (fisico, k_8_8_8_8 tiled) a un buffer BGRA para GDI. Devuelve false si no hay frontbuffer valido.
bool ReadFrontbuffer(uint32_t& w, uint32_t& h)
{
    auto& s = GpuGetStats();
    uint32_t fb = s.frontbuffer.load();
    w = s.width.load(); h = s.height.load();
    if (!fb || !w || !h || w > 4096 || h > 4096) return false;
    uint32_t pitch = (w + 31) & ~31u;
    const uint8_t* src = g_base + (0xA0000000u | (fb & 0x1FFFFFFF));
    g_pixels.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
    {
        uint32_t outer = TiledOuter(y, pitch, 2);
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t v = *(const uint32_t*)(src + TiledInner(x, y, 2, outer)); // k_8_8_8_8 con swap R/B: bytes B G R A
            g_pixels[size_t(y) * w + x] = v | 0xFF000000u;
        }
    }
    return true;
}

void ToggleFullscreen(HWND h)
{
    static bool full = false; static RECT saved{}; static LONG savedStyle = 0;
    full = !full;
    if (full)
    {
        GetWindowRect(h, &saved); savedStyle = GetWindowLongW(h, GWL_STYLE);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(h, GWL_STYLE, savedStyle);
        SetWindowPos(h, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT:
    {
        if (g_d3d12Active) { ValidateRect(h, nullptr); return 0; }
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        uint32_t fw, fh;
        if (!getenv("GH6_NOFRONTBUFFER") && ReadFrontbuffer(fw, fh))
        {
            BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = (LONG)fw; bi.bmiHeader.biHeight = -(LONG)fh;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, fw, fh, g_pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        else { HBRUSH bg = CreateSolidBrush(RGB(16, 16, 24)); FillRect(dc, &rc, bg); DeleteObject(bg); }
        auto& s = GpuGetStats();
        char text[256];
        snprintf(text, sizeof(text), "GH:WoR recomp - frames %llu - draws %llu - paquetes PM4 %llu - interrupciones %llu - frontbuffer %08X %ux%u",
                 (unsigned long long)s.swaps.load(), (unsigned long long)s.draws.load(), (unsigned long long)s.packets.load(),
                 (unsigned long long)s.interrupts.load(), s.frontbuffer.load(), s.width.load(), s.height.load());
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(220, 220, 220));
        TextOutA(dc, 16, 16, text, (int)strlen(text));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: ExitProcess(0);
    // Pantalla completa sin bordes con F11 o Alt+Enter (sesion 23). El swapchain es de 1280x720 y se escala solo.
    case WM_SYSKEYDOWN: case WM_KEYDOWN:
        if (w == VK_F12 && m == WM_KEYDOWN)   // F12: captura de pantalla shot_9xx.bmp (sesion 23, para informar de fallos)
        {
            static int n = 900;
            g_dumpRequest = n++;
            return 0;
        }
        if (w == VK_F11 || (m == WM_SYSKEYDOWN && w == VK_RETURN)) { ToggleFullscreen(h); return 0; }
        if (w == VK_F3 && m == WM_KEYDOWN && !(l & (1 << 30))) { g_showStats = !g_showStats; return 0; }   // contador de FPS
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

void GpuStartWindow()
{
    if (getenv("GH6_NOWINDOW")) return;
    std::thread([] {
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"GH6Recomp";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // icono propio (project/gh6.rc)
        RegisterClassW(&wc);
        // Sesion 24 (launcher): GH6_VENTANA=<ancho>x<alto> y GH6_PANTALLA_COMPLETA=1
        int ww = 1280, wh = 720;
        if (const char* v = getenv("GH6_VENTANA")) { int a = 0, b = 0; if (sscanf(v, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 360) { ww = a; wh = b; } }
        RECT r{ 0, 0, ww, wh }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_hwnd = CreateWindowW(L"GH6Recomp", L"Guitar Hero: Warriors of Rock (recompilado)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (const char* f = getenv("GH6_PANTALLA_COMPLETA"); f && atoi(f) == 1) ToggleFullscreen(g_hwnd);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }).detach();
}

// GH6_RAWDUMP=<dir hex>:<bytes hex>[,...]: con cada captura guarda esa memoria cruda en raw<k>_<captura>.bin (sesion 24)
static void RawDumpForShot(const char* name)
{
    // GH6_FINDTEX=<base hex>: con cada captura busca en la memoria del juego constantes de fetch de textura (6 dwords, tipo 2)
    // cuya direccion base sea esa, para localizar los objetos de textura (sesion 24)
    static bool findDone = false;
    if (name[0] && getenv("GH6_FINDTEX") && !findDone && strncmp(name, "shot_", 5) == 0 && atoi(name + 5) >= 90)
    {
        findDone = true;
        uint32_t want = (uint32_t)strtoul(getenv("GH6_FINDTEX"), nullptr, 16) & 0xFFFFF000u;
        extern void GuestAllocTops(uint32_t& virt, uint32_t& phys);
        uint32_t vt, pt; GuestAllocTops(vt, pt);
        const uint32_t ranges[][2] = { { 0x40000000u, vt }, { 0x82000000u, 0x83000000u }, { 0xA0000000u, pt } };
        LOG("[findtex] rangos 40000000-%08X A0000000-%08X", vt, pt);
        int found = 0;
        for (auto& r : ranges)
            for (uint32_t a = r[0] + 4; a < r[1] && found < 40; a += 4)
            {
                uint32_t v = __builtin_bswap32(*(const uint32_t*)(g_base + a));
                if ((v & 0x1FFFF000u) != want) continue;
                uint32_t d0 = __builtin_bswap32(*(const uint32_t*)(g_base + a - 4));
                uint32_t d2 = __builtin_bswap32(*(const uint32_t*)(g_base + a + 4)), d3 = __builtin_bswap32(*(const uint32_t*)(g_base + a + 8));
                static uint32_t wantSize = getenv("GH6_FINDTEXSIZE") ? (uint32_t)strtoul(getenv("GH6_FINDTEXSIZE"), nullptr, 16) : 0;
                if (wantSize ? d2 != wantSize : ((d0 & 0xFFFF0003u) != 0x81C20002u && (d0 & 0xFF000003u) != 0x81000002u)) continue;
                LOG("[findtex] %s: %08X  dw0 %08X dw1 %08X dw2 %08X dw3 %08X", name, a - 4, d0, v, d2, d3);
                found++;
            }
        LOG("[findtex] %s: busqueda de %08X terminada (%d)", name, want, found);
    }
    // GH6_MEMSEARCH=v@off,v@off,... (dwords hex en orden de la consola, desplazamiento en bytes): en la primera captura >= 90
    // busca esa secuencia en la memoria del juego y muestra el entorno (sesion 24: microcodigo de shaders parcheados)
    static bool msDone = false;
    if (name[0] && getenv("GH6_MEMSEARCH") && !msDone && strncmp(name, "shot_", 5) == 0 && atoi(name + 5) >= 90)
    {
        msDone = true;
        std::vector<std::pair<uint32_t, uint32_t>> pat;
        for (const char* q = getenv("GH6_MEMSEARCH"); q && *q; )
        {
            char* e; uint32_t v = (uint32_t)strtoul(q, &e, 16), off = 0;
            if (*e == '@') off = (uint32_t)strtoul(e + 1, &e, 10);
            pat.push_back({ v, off }); q = (*e == ',') ? e + 1 : nullptr;
        }
        extern void GuestAllocTops(uint32_t& virt, uint32_t& phys);
        uint32_t vt, pt; GuestAllocTops(vt, pt);
        const uint32_t ranges[][2] = { { 0x40000000u, vt }, { 0x82000000u, 0x83000000u }, { 0xA0000000u, pt } };
        int found = 0;
        for (auto& r : ranges)
            for (uint32_t a = r[0] + 256; a + 256 < r[1] && found < 20; a += 4)
            {
                bool ok = true;
                for (auto& pv : pat) if (__builtin_bswap32(*(const uint32_t*)(g_base + a + pv.second)) != pv.first) { ok = false; break; }
                if (!ok) continue;
                found++;
                std::string ctx;
                for (int k = -30; k < 6; k++) { char b[12]; snprintf(b, sizeof(b), "%08X ", __builtin_bswap32(*(const uint32_t*)(g_base + a + k * 4))); ctx += b; }
                LOG("[memsearch] %08X: %s", a, ctx.c_str());
            }
        LOG("[memsearch] terminada (%d)", found);
    }
    // GH6_ALIASSCAN=1: en la primera captura cuenta los bloques de 64 KB no nulos en los alias fisicos 0xC0000000 y 0xE0000000 (sesion 24)
    static bool aliasDone = false;
    if (name[0] && getenv("GH6_ALIASSCAN") && !aliasDone)
    {
        aliasDone = true;
        const uint32_t ranges[][2] = { { 0xC0000000u, 0xE0000000u }, { 0xE0000000u, 0xFF000000u } };
        for (auto& r : ranges)
        {
            uint32_t blocks = 0, first = 0;
            for (uint64_t a = r[0]; a < r[1]; a += 0x10000)
            {
                const uint64_t* p = (const uint64_t*)(g_base + a); bool nz = false;
                for (int i = 0; i < 0x10000 / 8; i += 64) if (p[i]) { nz = true; break; }
                if (nz) { if (!blocks) first = (uint32_t)a; blocks++; }
            }
            LOG("[alias] %08X-%08X: %u bloques de 64 KB con datos (primero %08X)", r[0], r[1], blocks, first);
        }
    }
    if (!name[0] || !getenv("GH6_RAWDUMP")) return;
    const char* s = getenv("GH6_RAWDUMP");
    for (int k = 0; s && *s; k++)
    {
        char* e = nullptr;
        uint32_t addr = (uint32_t)strtoul(s, &e, 16), len = (e && *e == ':') ? (uint32_t)strtoul(e + 1, nullptr, 16) : 0x10000;
        char pn[96]; snprintf(pn, sizeof(pn), "raw%d_%s.bin", k, name);
        if (FILE* f = fopen(pn, "wb")) { fwrite(g_base + (0xA0000000u | (addr & 0x1FFFFFFF)), 1, std::min<uint32_t>(len, 64u << 20), f); fclose(f); }
        s = strchr(s, ','); if (s) s++;
    }
}


void GpuPresent()
{
    static DWORD lastTitle = 0;
    static bool useD3D12 = getenv("GH6_GDI") == nullptr;
    DWORD now = GetTickCount();
    if (!g_hwnd) return;
    // Sesion 21: presentacion directa desde la textura resuelta por GPU (sin leer el frontbuffer de la memoria del juego)
    static bool fbFromMemory = getenv("GH6_FBMEM") != nullptr;
    if (useD3D12 && !fbFromMemory && g_d3d12Active)
    {
        int req = g_dumpRequest.load();
        static std::vector<long long> dumpList2 = [] {
            std::vector<long long> v; const char* s = getenv("GH6_DUMPFRAME");
            while (s && *s) { v.push_back(atoll(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        static size_t dumpIdx2 = 0;
        long long swapsNow = (long long)GpuGetStats().swaps.load();
        char name[64] = {};
        if (dumpIdx2 < dumpList2.size() && swapsNow >= dumpList2[dumpIdx2]) snprintf(name, sizeof(name), "fb_%lld.bmp", dumpList2[dumpIdx2]);
        if (req >= 0) snprintf(name, sizeof(name), "shot_%03d.bmp", req);
        static int seqN = 0; bool seqDump = false;
        if (!name[0] && g_dumpSeq.load() > 0) { snprintf(name, sizeof(name), "seq_%03d.bmp", seqN++); seqDump = true; }
        uint32_t w = 0, h = 0;
        if (D3D12PresentResolved(g_hwnd, GpuGetStats().frontbuffer.load(), name[0] ? &g_pixels : nullptr, w, h))
        {
            RawDumpForShot(name);
            if (name[0])
            {
                if (seqDump) g_dumpSeq--; else if (req >= 0) g_dumpRequest = -1; else dumpIdx2++;
                if (FILE* f = fopen(name, "wb"))
                {
                    uint32_t size = w * h * 4;
                    uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
                    *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
                    fwrite(hdr, 1, 54, f); fwrite(g_pixels.data(), 1, size, f); fclose(f);
                }
            }
            goto title;
        }
    }
    if (useD3D12)
    {
        uint32_t w = 0, h = 0;
        bool ok = !getenv("GH6_NOFRONTBUFFER") && ReadFrontbuffer(w, h);
        // GH6_DUMPFRAME=a,b,c: vuelca el frontbuffer al alcanzar (o pasar) cada uno de esos swaps
        static std::vector<long long> dumpList = [] {
            std::vector<long long> v; const char* s = getenv("GH6_DUMPFRAME");
            while (s && *s) { v.push_back(atoll(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        static size_t dumpIdx = 0;
        long long swapsNow = (long long)GpuGetStats().swaps.load();
        char name[64] = {};
        if (ok && dumpIdx < dumpList.size() && swapsNow >= dumpList[dumpIdx]) snprintf(name, sizeof(name), "fb_%lld.bmp", dumpList[dumpIdx++]);
        int req = g_dumpRequest.exchange(-1);   // orden DUMP del guion de entrada (xam.cpp)
        if (req >= 0) { if (ok) snprintf(name, sizeof(name), "shot_%03d.bmp", req); else g_dumpRequest = req; }
        { static int seqM = 0; if (ok && !name[0] && g_dumpSeq.load() > 0) { snprintf(name, sizeof(name), "seq_%03d.bmp", seqM++); g_dumpSeq--; } }
        // GH6_PEEKTEX=<direccion hex>[,<direccion>]: con cada captura vuelca tambien esas texturas 1280x720 8888 tiled (diagnostico)
        RawDumpForShot(name);
        if (name[0] && getenv("GH6_PEEKTEX"))
        {
            const char* s = getenv("GH6_PEEKTEX");
            for (int k = 0; s && *s; k++)
            {
                uint32_t addr = (uint32_t)strtoul(s, nullptr, 16);
                std::vector<uint32_t> px(1280 * 720);
                const uint8_t* src = g_base + (0xA0000000u | (addr & 0x1FFFFFFF));
                for (uint32_t y = 0; y < 720; y++)
                {
                    uint32_t outer = TiledOuter(y, 1280, 2);
                    for (uint32_t x = 0; x < 1280; x++) px[y * 1280 + x] = __builtin_bswap32(*(const uint32_t*)(src + TiledInner(x, y, 2, outer))) | 0xFF000000u;
                }
                char pn[96]; snprintf(pn, sizeof(pn), "peek%d_%s", k, name);
                if (FILE* f = fopen(pn, "wb"))
                {
                    uint32_t size = 1280 * 720 * 4;
                    uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
                    *(int32_t*)(hdr + 18) = 1280; *(int32_t*)(hdr + 22) = -720; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
                    fwrite(hdr, 1, 54, f); fwrite(px.data(), 1, size, f); fclose(f);
                }
                s = strchr(s, ','); if (s) s++;
            }
        }
        if (name[0])
        {
            if (FILE* f = fopen(name, "wb"))
            {
                uint32_t size = w * h * 4;
                uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
                *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
                fwrite(hdr, 1, 54, f); fwrite(g_pixels.data(), 1, size, f); fclose(f);
            }
        }
        if (D3D12Present(g_hwnd, ok ? g_pixels.data() : nullptr, w, h)) g_d3d12Active = true;
        else useD3D12 = false; // fallo: volver a GDI
    }
title:
    if (now - lastTitle > 500)
    {
        lastTitle = now;
        auto& s = GpuGetStats();
        char t[256];
        // Sesion 24: titulo limpio para jugar; contadores solo en modo desarrollo (GH6.exe basefile.bin default.xex)
        static const bool dev = getenv("GH6_DEVMODE") != nullptr;
        if (dev) snprintf(t, sizeof(t), "Guitar Hero: Warriors of Rock (recompilado) - %s - frames %llu - draws %llu", g_d3d12Active ? "D3D12" : "GDI",
                          (unsigned long long)s.swaps.load(), (unsigned long long)s.draws.load());
        else snprintf(t, sizeof(t), "Guitar Hero: Warriors of Rock");
        static char lastT[256] = {};
        if (strcmp(t, lastT)) { strcpy(lastT, t); SetWindowTextA(g_hwnd, t); }
        if (!g_d3d12Active) InvalidateRect(g_hwnd, nullptr, FALSE);
        static DWORD fpsT = now; static uint64_t fpsN = s.swaps.load();
        if (now - fpsT >= 5000) { LOG("[fps] t=%lus %.1f fps", now / 1000, (s.swaps.load() - fpsN) * 1000.0 / (now - fpsT)); fpsT = now; fpsN = s.swaps.load(); }
    }
}

// Launcher (sesion 24): ventana de configuracion que se abre antes del juego (juego, graficos, controles por jugador y microfono).
// Lee y escribe gh6.ini conservando los comentarios y las claves que no conoce. "mostrar_launcher=0" entra directo al juego
// (mantener Mayus al abrir lo vuelve a mostrar); GH6WoR.exe --jugar lo salta y --launcher lo fuerza.
#include "runtime.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <xinput.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <shellapi.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

// Controles comunes v6 (estilos visuales modernos en la ventana del launcher)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
void MicListDevices(std::vector<std::pair<std::wstring, std::wstring>>& out);            // mic_capture.cpp
bool PsPadGetState(uint16_t& buttons, uint8_t& lt, uint8_t& rt, int16_t sticks[4]);    // dualsense.cpp
uint32_t InputParseSources(const char* s);                                               // xam.cpp
uint32_t InputSourceBits(uint16_t b, uint8_t lt, uint8_t rt, const int16_t st[4]);      // xam.cpp
// updater.cpp
struct UpdateInfo { bool ok = false, newer = false; std::string error, tag, zipUrl, notes, page; };
UpdateInfo UpdateCheck(const std::string& repo);
bool UpdateInstall(const UpdateInfo& u, const std::vector<std::wstring>& preserve, const std::function<void(int, int)>& progress, std::string& err);
bool UpdaterRelaunch();
const char* AppVersion();
// net.cpp (sesion 27): juego en linea P2P
void NetLauncherStart();
std::vector<uint8_t> IsoExtract(const std::string& isoPath, const std::string& relPath);   // fs.cpp (sesion 28)
bool NetLauncherInfo(std::string& code, std::wstring& status);

namespace
{
std::string ExeDir()
{
    char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p; return s.substr(0, s.find_last_of("\\/"));
}
// ---- gh6.ini como lista de lineas (se conservan comentarios y claves desconocidas) ----
struct IniDoc
{
    std::vector<std::string> lines;
    bool appendedHeader = false;
    static std::string Trim(const std::string& s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
    void Load(const std::string& path)
    {
        lines.clear();
        if (FILE* f = fopen(path.c_str(), "r"))
        {
            char buf[2048];
            while (fgets(buf, sizeof(buf), f)) { std::string s = buf; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); lines.push_back(s); }
            fclose(f);
        }
    }
    bool Save(const std::string& path) const
    {
        FILE* f = fopen(path.c_str(), "w"); if (!f) return false;
        for (auto& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f); return true;
    }
    int Find(const std::string& k) const
    {
        for (size_t i = 0; i < lines.size(); i++)
        {
            std::string t = Trim(lines[i]);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            size_t eq = t.find('='); if (eq == std::string::npos) continue;
            if (Trim(t.substr(0, eq)) == k) return (int)i;
        }
        return -1;
    }
    std::string Get(const std::string& k, const std::string& def = "") const
    {
        int i = Find(k); if (i < 0) return def;
        return Trim(lines[i].substr(lines[i].find('=') + 1));
    }
    void Remove(const std::string& k) { int i = Find(k); if (i >= 0) lines.erase(lines.begin() + i); }
    void Set(const std::string& k, const std::string& v)
    {
        int i = Find(k);
        if (i >= 0) { lines[i] = k + "=" + v; return; }
        if (!appendedHeader && Find("mostrar_launcher") < 0) { lines.push_back("; ---- Opciones del launcher ----"); appendedHeader = true; }
        lines.push_back(k + "=" + v);
    }
};

std::wstring W(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n - 1, 0); MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n); return w; }
std::string A(const std::wstring& w, UINT cp = CP_ACP) { if (w.empty()) return ""; int n = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n - 1, 0); WideCharToMultiByte(cp, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s; }
std::wstring GetText(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n, 0); GetWindowTextW(h, s.data(), n + 1); return s; }

// ---- captura del microfono para el medidor de nivel ----
struct MeterCapture
{
    std::thread t; std::atomic<bool> stop{ false }; std::atomic<float> peak{ 0 }; std::atomic<bool> ok{ false };
    void Start(const std::wstring& id) { Stop(); stop = false; t = std::thread([this, id] { Run(id); }); }
    void Stop() { if (t.joinable()) { stop = true; t.join(); } peak = 0; ok = false; }
    void Run(std::wstring id)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* cl = nullptr; IAudioCaptureClient* cap = nullptr;
        bool good = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en));
        if (good) good = SUCCEEDED(id.empty() ? en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev) : en->GetDevice(id.c_str(), &dev));
        if (good) good = SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl));
        if (good)
        {
            WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 96000;
            good = SUCCEEDED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &wf, nullptr))
                && SUCCEEDED(cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) && SUCCEEDED(cl->Start());
        }
        ok = good;
        while (good && !stop)
        {
            Sleep(10);
            UINT32 pkt = 0; float m = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt)
            {
                BYTE* d = nullptr; UINT32 fr = 0; DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) { auto* s = (const int16_t*)d; for (UINT32 i = 0; i < fr; i++) m = std::max(m, std::abs(s[i]) / 32768.0f); }
                cap->ReleaseBuffer(fr);
            }
            if (m > peak) peak = m;
        }
        if (cl) cl->Stop();
        if (cap) cap->Release(); if (cl) cl->Release(); if (dev) dev->Release(); if (en) en->Release();
        CoUninitialize();
    }
};

// ---- asignaciones de botones (mismas claves y valores por defecto que xam.cpp) ----
// val = valor comun; ovr[p] = valor propio del jugador p+1 (vacio = usa el comun), en gh6.ini jugadorN_<clave>
struct MapFn { const char* key; const wchar_t* label; const char* def; std::string val; std::string ovr[4]; };
const std::string& Eff(const MapFn& f, int p) { return p >= 0 && p < 4 && !f.ovr[p].empty() ? f.ovr[p] : f.val; }
MapFn g_guitar[] = {
    { "mando_verde", L"Traste verde", "A" }, { "mando_rojo", L"Traste rojo", "B" }, { "mando_amarillo", L"Traste amarillo", "Y" },
    { "mando_azul", L"Traste azul", "X" }, { "mando_naranja", L"Traste naranja", "LB" },
    { "mando_rasgueo_arriba", L"Rasgueo arriba", "UP|LS_UP" }, { "mando_rasgueo_abajo", L"Rasgueo abajo", "DOWN|LS_DOWN" },
    { "mando_izquierda", L"Izquierda (menús)", "LEFT|LS_LEFT" }, { "mando_derecha", L"Derecha (menús)", "RIGHT|LS_RIGHT" },
    { "mando_start", L"Start / pausa", "START" }, { "mando_poder_estrella", L"Poder estrella", "BACK|RB" },
    { "mando_palanca", L"Palanca (whammy)", "RT" }, { "mando_inclinacion", L"Inclinación (estrella)", "" } };
MapFn g_drum[] = {
    { "bateria_verde", L"Pad verde", "A" }, { "bateria_rojo", L"Pad rojo", "B" }, { "bateria_amarillo", L"Platillo amarillo", "Y" },
    { "bateria_azul", L"Pad azul", "X" }, { "bateria_naranja", L"Platillo naranja", "RB" }, { "bateria_bombo", L"Bombo", "LB|LT|RT" },
    { "bateria_arriba", L"Arriba (menús)", "UP|LS_UP" }, { "bateria_abajo", L"Abajo (menús)", "DOWN|LS_DOWN" },
    { "bateria_start", L"Start / pausa", "START" }, { "bateria_back", L"Atrás", "BACK" } };
constexpr int kGuitarN = 13, kDrumN = 10;
constexpr uint32_t kLT = 1u << 16, kRT = 1u << 17, kRSDirs = (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25);
const struct { const char* n; uint32_t m; } kNames[] = {
    { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 }, { "LB", 0x0100 }, { "RB", 0x0200 }, { "LT", kLT }, { "RT", kRT },
    { "UP", 0x0001 }, { "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 }, { "START", 0x0010 }, { "BACK", 0x0020 },
    { "LS", 0x0040 }, { "RS", 0x0080 }, { "LS_UP", 1u << 18 }, { "LS_DOWN", 1u << 19 }, { "LS_LEFT", 1u << 20 }, { "LS_RIGHT", 1u << 21 },
    { "RS_UP", 1u << 22 }, { "RS_DOWN", 1u << 23 }, { "RS_LEFT", 1u << 24 }, { "RS_RIGHT", 1u << 25 } };
std::string BitsToNames(uint32_t b)
{
    std::string s;
    for (auto& n : kNames) if (b & n.m) { if (!s.empty()) s += "|"; s += n.n; }
    return s;
}

// ---- colores y estado de la ventana ----
// Sesion 28: estetica como el launcher de Dead Space (paneles con esquinas cortadas, titulos espaciados, filas con descripcion,
// selectores < valor > y conmutadores SI/NO) con los colores de Guitar Hero (fuego).
const COLORREF kBg = RGB(12, 8, 10), kScan = RGB(17, 11, 13), kSide = RGB(9, 6, 8), kPanel = RGB(22, 14, 16), kPanelHi = RGB(52, 26, 18),
               kBar = RGB(10, 7, 8), kSel = RGB(46, 26, 22), kInput = RGB(30, 19, 20), kText = RGB(242, 234, 226), kDim = RGB(165, 142, 136),
               kAccent = RGB(255, 112, 36), kAccent2 = RGB(255, 192, 84), kLine = RGB(112, 52, 32), kAccentDim = RGB(190, 90, 40),
               kPlayText = RGB(34, 10, 4);
enum
{
    ID_TAB0 = 100, ID_PLAY = 200, ID_SAVE, ID_EXIT, ID_SHOWLAUNCHER, ID_SAVED,
    ID_ISO = 300, ID_ISO_BROWSE, ID_DLC, ID_DLC_BROWSE, ID_DLCCOUNT, ID_SKIPINTRO, ID_SAVEINFO, ID_SAVEOPEN, ID_SAVERESET, ID_SAVERESTORE,
    ID_UPDINFO, ID_UPDCHECK, ID_UPDAUTO,
    ID_FULLSCREEN = 400, ID_WINSIZE, ID_SCALE, ID_VSYNC, ID_MBLUR, ID_GRAIN, ID_AUDIOLAT, ID_AA, ID_ANISO, ID_SHARP, ID_FPS, ID_RES, ID_PANO,
    ID_INST0 = 500, ID_PAD0 = 510, ID_STATUS0 = 520, ID_KBD = 530, ID_PS, ID_PADMODE, ID_DIAG, ID_MAPSET, ID_MAPPLAYER, ID_ASSIGN, ID_ADD, ID_RESET, ID_MAPLIST, ID_TESTPLAYER, ID_TESTPANEL,
    ID_MIC_ON = 600, ID_MIC_DEV, ID_MIC_GAIN, ID_MIC_GAINLBL, ID_MIC_METER,
    ID_ACHINFO = 700, ID_ACHLIST,
    ID_NET_ON = 800, ID_NET_NAME, ID_NET_CODE, ID_NET_COPY, ID_NET_PEERS, ID_NET_STATUS,
    ID_SEC0 = 900, ID_BANNER = 990,
    ID_Q_RES = 1100, ID_Q_PANO, ID_Q_FULL, ID_Q_NET, ID_HOME_ISO, ID_HOME_UPD, ID_HOME_KEYS,
};
// Paginas: 0 Juego, 1 Graficos, 2 Controles, 3 Audio (latencia + microfono), 4 Ayuda, 5 Logros, 6 En linea, 7 Inicio
constexpr int kTabs = 8, kHome = 7;
const int kTabOrder[kTabs] = { kHome, 0, 1, 2, 3, 6, 5, 4 };   // orden en la barra lateral
int TabPos(int i) { for (int k = 0; k < kTabs; k++) if (kTabOrder[k] == i) return k; return 0; }
// Geometria (pixeles logicos a 96 ppp): barra lateral, panel de contenido y barra inferior
constexpr int kW = 1120, kH = 720, kSideW = 250, kX0 = 280, kXR = 1086, kY0 = 30, kPanelBottom = 634, kBarTop = 648;
struct AchRow { std::wstring name, desc, howto, date; int gs = 0; bool unlocked = false; };
// Sesion 25: + tamanos panoramicos (21:9 y 32:9)
const wchar_t* kWinSizes[] = { L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160",
                               L"2560 x 1080 (21:9)", L"3440 x 1440 (21:9)", L"3840 x 1080 (32:9)", L"5120 x 1440 (32:9)" };
const char* kWinSizesIni[] = { "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160", "2560x1080", "3440x1440", "3840x1080", "5120x1440" };
constexpr int kNumWinSizes = 9;
const char* kInstIni[] = { "guitarra", "bateria", "voz" };

struct Ui
{
    HWND wnd = nullptr; HINSTANCE inst = nullptr; int dpi = 96;
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSub = nullptr, fontTab = nullptr, fontIcon = nullptr, fontSmall = nullptr,
          fontHead = nullptr, fontPlay = nullptr, fontBanner = nullptr;
    HBRUSH brBg = nullptr, brInput = nullptr, brSide = nullptr, brPanel = nullptr, brBar = nullptr;
    int tab = kHome; std::vector<HWND> pages[kTabs];
    std::vector<std::pair<int, RECT>> frames;   // marcos dibujados detras de campos de texto y listas (pagina, rectangulo logico)
    int nextSec = ID_SEC0;
    HBITMAP banner = nullptr; int bannerW = 0, bannerH = 0;
    HBITMAP logo = nullptr; int logoW = 0, logoH = 0; std::wstring logoIso;
    std::vector<uint32_t> logoCut; int cutW = 0, cutH = 0;   // solo las letras del logo (alfa premultiplicado)
    HBITMAP cutBmp[2] = {}; int cutBw[2] = {}, cutBh[2] = {};   // logoCut escalado (0 = barra lateral, 1 = portada)   // logo oficial leido del ISO del usuario (data\memcard\ICON0.png)
    bool isoMissing = false;
    std::vector<AchRow> achs;
    std::map<HWND, bool> toggles; std::set<HWND> dimLabels;
    IniDoc ini;
    int mapSet = 0, mapPlayer = 0;   // mapPlayer: 0 = todos (valor comun), 1-4 = solo ese jugador
    bool capturing = false, capAdd = false; int capItem = -1; DWORD capStart = 0; uint32_t capPrev = 0;
    std::vector<std::pair<std::wstring, std::wstring>> mics;
    MeterCapture meter; float meterShown = 0, meterHold = 0; DWORD holdT = 0; bool meterOn = false;
    XINPUT_STATE pads[4] = {}; bool padOk[4] = {}; BYTE padSub[4] = {}; DWORD padTry[4] = {};
    DWORD savedT = 0; int ticks = 0;
    DWORD diagUntil = 0, diagStart = 0; FILE* diag = nullptr; std::string diagLast[4];
    bool result = false, done = false;
    UpdateInfo upd; bool updBusy = false, updAuto = false;
} g;
int S(int v) { return MulDiv(v, g.dpi, 96); }
HWND Item(int id) { return GetDlgItem(g.wnd, id); }
bool Toggled(int id) { auto it = g.toggles.find(Item(id)); return it != g.toggles.end() && it->second; }
int Sel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }

HWND Make(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style | ((page < 0 || page == g.tab) ? WS_VISIBLE : 0), S(x), S(y), S(w), S(h), g.wnd,
                             (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    if (page >= 0) g.pages[page].push_back(c);
    return c;
}
HWND Label(int page, const wchar_t* t, int x, int y, int w, int h = 22, int id = 0, bool dim = false, HFONT f = nullptr)
{
    HWND c = Make(page, L"STATIC", t, SS_LEFT | SS_NOPREFIX, x, y, w, h, id);
    if (dim) g.dimLabels.insert(c);
    if (f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}
// Titulo de seccion (texto espaciado en naranja con una raya y tres marcas); devuelve la y siguiente
int Section(int page, int y, const wchar_t* t)
{
    Make(page, L"STATIC", t, SS_OWNERDRAW, kX0, y, kXR - kX0, 32, g.nextSec++);
    return y + 40;
}
// Fila: nombre y descripcion a la izquierda (el control va a la derecha)
void Row(int page, int y, const wchar_t* label, const wchar_t* hint, int labelW = 520, int hintId = 0)
{
    Label(page, label, kX0, y, labelW, 21);
    if ((hint && *hint) || hintId) Label(page, hint ? hint : L"", kX0, y + 21, labelW + 40, 18, hintId, true, g.fontSmall);
}
// Conmutador SI/NO (84 x 26)
HWND Toggle(int page, const wchar_t* t, int x, int y, int w, int id, bool on)
{
    HWND c = Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 26, id);
    g.toggles[c] = on; return c;
}
HWND ToggleRight(int page, int y, int id, bool on) { return Toggle(page, L"", kXR - 84, y + 6, 84, id, on); }
// Selector  < valor >  (clase GHSel: responde a CB_ADDSTRING / CB_SETCURSEL / CB_GETCURSEL y avisa con CBN_SELCHANGE)
HWND Combo(int page, int x, int y, int w, int id, const std::vector<std::wstring>& items, int sel)
{
    HWND c = Make(page, L"GHSel", L"", WS_TABSTOP, x, y, w, 30, id);
    for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(c, CB_SETCURSEL, std::clamp(sel, 0, (int)items.size() - 1), 0);
    return c;
}
HWND ComboRight(int page, int y, int id, const std::vector<std::wstring>& items, int sel, int w = 280) { return Combo(page, kXR - w, y + 4, w, id, items, sel); }
HWND Button(int page, const wchar_t* t, int x, int y, int w, int h, int id) { return Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id); }
// Campo de texto sin borde dentro de un marco con esquinas cortadas
HWND Edit(int page, const std::wstring& t, int x, int y, int w, int id)
{
    g.frames.push_back({ page, RECT{ x, y, x + w, y + 30 } });
    return Make(page, L"EDIT", t.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, x + 10, y + 6, w - 20, 20, id);
}
HWND List(int page, int x, int y, int w, int h, int id, DWORD extra)
{
    g.frames.push_back({ page, RECT{ x, y, x + w, y + h } });
    HWND l = Make(page, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | extra, x + 2, y + 2, w - 4, h - 4, id);
    SetWindowTheme(l, L"DarkMode_Explorer", nullptr);
    return l;
}

// ---- selector < valor > ----
struct SelData { std::vector<std::wstring> items; int sel = 0, hot = 0; bool tracking = false; };
void Chamfer(HDC dc, RECT r, int cut, COLORREF fill, COLORREF border, int width = 1);
void Text(HDC dc, const wchar_t* t, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE, int spacing = 0);
LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    SelData* d = (SelData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    auto step = [&](int dir) {
        if (!d || d->items.empty()) return;
        int n = (int)d->items.size(); d->sel = (d->sel + dir + n) % n;
        InvalidateRect(h, nullptr, FALSE);
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), CBN_SELCHANGE), (LPARAM)h);
    };
    switch (m)
    {
    case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)new SelData()); break;
    case WM_NCDESTROY: delete d; SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
    case CB_ADDSTRING: d->items.push_back((const wchar_t*)l); InvalidateRect(h, nullptr, FALSE); return (LRESULT)d->items.size() - 1;
    case CB_RESETCONTENT: d->items.clear(); d->sel = 0; InvalidateRect(h, nullptr, FALSE); return 0;
    case CB_SETCURSEL: d->sel = d->items.empty() ? 0 : std::clamp((int)w, 0, (int)d->items.size() - 1); InvalidateRect(h, nullptr, FALSE); return d->sel;
    case CB_GETCURSEL: return d->items.empty() ? -1 : d->sel;
    case CB_GETCOUNT: return (LRESULT)d->items.size();
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: { SetFocus(h); RECT rc; GetClientRect(h, &rc); step(GET_X_LPARAM(l) < rc.right / 2 ? -1 : 1); return 0; }
    case WM_LBUTTONDBLCLK: { RECT rc; GetClientRect(h, &rc); step(GET_X_LPARAM(l) < rc.right / 2 ? -1 : 1); return 0; }
    case WM_MOUSEWHEEL: step(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0;
    case WM_KEYDOWN: if (w == VK_LEFT) step(-1); else if (w == VK_RIGHT || w == VK_SPACE) step(1); return 0;
    case WM_MOUSEMOVE:
    {
        RECT rc; GetClientRect(h, &rc); int hot = GET_X_LPARAM(l) < rc.right / 2 ? -1 : 1;
        if (!d->tracking) { TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); d->tracking = true; }
        if (hot != d->hot) { d->hot = hot; InvalidateRect(h, nullptr, FALSE); }
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return 0;
    }
    case WM_MOUSELEAVE: d->hot = 0; d->tracking = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
        HBRUSH bg = CreateSolidBrush(kPanel); FillRect(mem, &rc, bg); DeleteObject(bg);
        bool focus = GetFocus() == h, en = IsWindowEnabled(h) != 0;
        RECT box{ 0, 0, rc.right - 1, rc.bottom - 1 };
        Chamfer(mem, box, S(6), d->hot ? kPanelHi : kInput, focus || d->hot ? kAccent : kLine);
        int aw = S(30);
        RECT left{ 0, 0, aw, rc.bottom }, right{ rc.right - aw, 0, rc.right, rc.bottom }, mid{ aw, 0, rc.right - aw, rc.bottom };
        Text(mem, L"◀", left, d->hot < 0 ? kAccent2 : kAccentDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(mem, L"▶", right, d->hot > 0 ? kAccent2 : kAccentDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!d->items.empty()) Text(mem, d->items[d->sel].c_str(), mid, en ? kText : kDim, g.font, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- mandos ----
void PollPads()
{
    DWORD now = GetTickCount();
    for (int i = 0; i < 4; i++)
    {
        if (!g.padOk[i] && g.padTry[i] && now - g.padTry[i] < 1000) continue;
        g.padTry[i] = now;
        bool was = g.padOk[i];
        g.padOk[i] = XInputGetState(i, &g.pads[i]) == ERROR_SUCCESS;
        if (g.padOk[i] && !was) { XINPUT_CAPABILITIES c{}; g.padSub[i] = XInputGetCapabilities(i, 0, &c) == ERROR_SUCCESS ? c.SubType : 1; }
    }
}
// Estado crudo de un jugador: su mando XInput y, si le toca, el mando de PlayStation
bool PlayerRaw(int p, uint16_t& b, uint8_t& lt, uint8_t& rt, int16_t st[4])
{
    b = 0; lt = rt = 0; st[0] = st[1] = st[2] = st[3] = 0; bool any = false;
    int slot = Sel(ID_PAD0 + p);
    if (slot >= 0 && slot < 4 && g.padOk[slot])
    {
        auto& gp = g.pads[slot].Gamepad;
        b |= gp.wButtons; lt = gp.bLeftTrigger; rt = gp.bRightTrigger;
        st[0] = gp.sThumbLX; st[1] = gp.sThumbLY; st[2] = gp.sThumbRX; st[3] = gp.sThumbRY; any = true;
    }
    uint16_t pb; uint8_t pl, pr; int16_t ps[4];
    if (Sel(ID_PS) == p && PsPadGetState(pb, pl, pr, ps))
    {
        b |= pb; lt = std::max(lt, pl); rt = std::max(rt, pr);
        if (!any) memcpy(st, ps, sizeof(ps));
        any = true;
    }
    return any;
}
uint32_t AllPadsBits()
{
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++)
        if (g.padOk[i]) { auto& gp = g.pads[i].Gamepad; int16_t st[4] = { gp.sThumbLX, gp.sThumbLY, gp.sThumbRX, gp.sThumbRY }; bits |= InputSourceBits(gp.wButtons, gp.bLeftTrigger, gp.bRightTrigger, st); }
    uint16_t pb; uint8_t pl, pr; int16_t ps[4];
    if (PsPadGetState(pb, pl, pr, ps)) bits |= InputSourceBits(pb, pl, pr, ps);
    return bits;
}
void UpdateStatus()
{
    uint16_t pb; uint8_t pl, pr; int16_t ps[4];
    bool psOk = PsPadGetState(pb, pl, pr, ps);
    for (int p = 0; p < 4; p++)
    {
        int slot = Sel(ID_PAD0 + p);
        wchar_t t[160];
        if (slot >= 0 && slot < 4)
        {
            if (!g.padOk[slot]) swprintf(t, 160, L"Mando %d sin conectar", slot + 1);
            else if (g.padSub[slot] == 6 || g.padSub[slot] == 7 || g.padSub[slot] == 11) swprintf(t, 160, L"Mando %d: guitarra real", slot + 1);
            else if (g.padSub[slot] == 8) swprintf(t, 160, L"Mando %d: batería real", slot + 1);
            else if (g.padSub[slot] == 1) swprintf(t, 160, L"Mando %d conectado (mando)", slot + 1);
            else swprintf(t, 160, L"Mando %d conectado (tipo %u)", slot + 1, g.padSub[slot]);
        }
        else wcscpy(t, L"Sin mando XInput");
        if (Sel(ID_KBD) == p) wcscat(t, L" + teclado");
        if (Sel(ID_PS) == p && psOk) wcscat(t, L" + PS");
        SetWindowTextW(Item(ID_STATUS0 + p), t);
    }
}

// ---- lista de asignaciones ----
MapFn* CurMap() { return g.mapSet == 0 ? g_guitar : g_drum; }
int CurMapN() { return g.mapSet == 0 ? kGuitarN : kDrumN; }
void FillMapList()
{
    HWND l = Item(ID_MAPLIST);
    int sel = (int)SendMessageW(l, LB_GETCURSEL, 0, 0);
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < CurMapN(); i++) SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)i);
    SendMessageW(l, LB_SETCURSEL, sel >= 0 && sel < CurMapN() ? sel : 0, 0);
}
void StartCapture(bool add)
{
    int sel = (int)SendMessageW(Item(ID_MAPLIST), LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    g.capturing = true; g.capAdd = add; g.capItem = sel; g.capStart = GetTickCount(); PollPads(); g.capPrev = AllPadsBits();
    InvalidateRect(Item(ID_MAPLIST), nullptr, TRUE);
}
void TickCapture()
{
    if (!g.capturing) return;
    uint32_t cur = AllPadsBits(), rise = cur & ~g.capPrev; g.capPrev = cur;
    bool cancel = GetTickCount() - g.capStart > 8000 || ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && GetForegroundWindow() == g.wnd);
    MapFn& fn = CurMap()[g.capItem];
    std::string& tv = g.mapPlayer == 0 ? fn.val : fn.ovr[g.mapPlayer - 1];
    struct { std::string& val; } f{ tv };
    if (g.capAdd && f.val.empty()) f.val = fn.val;   // anadir a un jugador sin valor propio: parte del comun
    bool analogOnly = g.mapSet == 0 && g.capItem == 11;   // la palanca solo acepta RT, LT o el stick derecho
    if (analogOnly) { uint32_t a = rise & (kLT | kRT | kRSDirs); rise = (a & kRT) ? kRT : (a & kLT) ? kLT : (a & kRSDirs) ? 0x0080 : 0; }
    if (!cancel && rise)
    {
        std::string n = analogOnly && rise == 0x0080 ? "RS" : BitsToNames(rise);
        if (g.capAdd && !f.val.empty() && !analogOnly) { if (!(InputParseSources(f.val.c_str()) & rise)) f.val += "|" + n; }
        else f.val = n;
    }
    if (cancel || rise) { g.capturing = false; InvalidateRect(Item(ID_MAPLIST), nullptr, TRUE); }
}

// ---- dibujo ----
void FillR(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
void RoundR(HDC dc, RECT r, COLORREF fill, COLORREF border, int rad)
{
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
// Panel con dos esquinas cortadas (arriba a la izquierda y abajo a la derecha), la forma de toda la interfaz
void Chamfer(HDC dc, RECT r, int cut, COLORREF fill, COLORREF border, int width)
{
    POINT pts[6] = { { r.left + cut, r.top }, { r.right, r.top }, { r.right, r.bottom - cut }, { r.right - cut, r.bottom }, { r.left, r.bottom }, { r.left, r.top + cut } };
    HBRUSH b = fill == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = border == CLR_NONE ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, width, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 6);
    SelectObject(dc, ob); SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(b);
    if (border != CLR_NONE) DeleteObject(p);
}
void LineC(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int width = 1)
{
    HPEN p = CreatePen(PS_SOLID, width, c); HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
    SelectObject(dc, op); DeleteObject(p);
}
void Circle(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, std::max(1, S(2)), border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
COLORREF Dim(COLORREF c, float k) { return RGB((int)(GetRValue(c) * k), (int)(GetGValue(c) * k), (int)(GetBValue(c) * k)); }
COLORREF Mix(COLORREF a, COLORREF b, float t) { return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t), (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t)); }
// Letras del logo (alfa premultiplicado) escaladas a dw x dh con un promedio por area; se guarda por uso y tamano
HBITMAP ScaleCut(int slot, int dw, int dh)
{
    if (g.logoCut.empty() || dw <= 0 || dh <= 0) return nullptr;
    if (g.cutBmp[slot] && g.cutBw[slot] == dw && g.cutBh[slot] == dh) return g.cutBmp[slot];
    if (g.cutBmp[slot]) DeleteObject(g.cutBmp[slot]);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32, BI_RGB };
    uint32_t* px = nullptr; HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    g.cutBmp[slot] = hb; g.cutBw[slot] = dw; g.cutBh[slot] = dh;
    if (!hb || !px) return hb;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
        {
            float x0 = (float)x * g.cutW / dw, x1 = (float)(x + 1) * g.cutW / dw, y0 = (float)y * g.cutH / dh, y1 = (float)(y + 1) * g.cutH / dh;
            float acc[4] = {}, wsum = 0;
            for (int sy = (int)y0; sy < (int)std::ceil(y1) && sy < g.cutH; sy++)
                for (int sx = (int)x0; sx < (int)std::ceil(x1) && sx < g.cutW; sx++)
                {
                    float wx = std::min(x1, (float)sx + 1) - std::max(x0, (float)sx), wy = std::min(y1, (float)sy + 1) - std::max(y0, (float)sy), wgt = wx * wy;
                    if (wgt <= 0) continue;
                    uint32_t c = g.logoCut[sy * g.cutW + sx];
                    for (int k = 0; k < 4; k++) acc[k] += wgt * ((c >> (k * 8)) & 255);
                    wsum += wgt;
                }
            uint32_t o = 0;
            for (int k = 0; k < 4; k++) o |= (uint32_t)std::clamp(acc[k] / std::max(wsum, 1e-6f) + 0.5f, 0.0f, 255.0f) << (k * 8);
            px[y * dw + x] = o;
        }
    return hb;
}
void Text(HDC dc, const wchar_t* t, RECT r, COLORREF c, HFONT f, UINT fmt, int spacing)
{
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c); HGDIOBJ of = SelectObject(dc, f);
    SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, t, -1, &r, fmt | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, of);
}
void Triangle(HDC dc, int cx, int cy, int s, bool up, COLORREF c)
{
    POINT pt[3] = { { cx - s, up ? cy + s / 2 : cy - s / 2 }, { cx + s, up ? cy + s / 2 : cy - s / 2 }, { cx, up ? cy - s / 2 : cy + s / 2 } };
    HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p); Polygon(dc, pt, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
void DrawTestPanel(HDC dc, RECT rc)
{
    Chamfer(dc, RECT{ rc.left, rc.top, rc.right - 1, rc.bottom - 1 }, S(10), kInput, kLine);
    if (g.capturing)
    {
        wchar_t t[128]; swprintf(t, 128, L"Pulsa un botón del mando para\n\"%s\"  (Esc cancela, %d s)", CurMap()[g.capItem].label, 8 - (int)((GetTickCount() - g.capStart) / 1000));
        RECT r = rc; InflateRect(&r, -S(10), -S(10));
        Text(dc, t, r, kAccent2, g.fontBold, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    uint16_t b; uint8_t lt, rt; int16_t st[4];
    int p = Sel(ID_TESTPLAYER);
    bool any = PlayerRaw(p, b, lt, rt, st);
    uint32_t bits = any ? InputSourceBits(b, lt, rt, st) : 0;
    MapFn* m = CurMap();
    auto on = [&](int i) { return (bits & InputParseSources(Eff(m[i], p).c_str())) != 0; };
    const COLORREF fret[5] = { RGB(40, 200, 80), RGB(230, 50, 50), RGB(240, 210, 40), RGB(50, 120, 240), RGB(250, 140, 30) };
    int r = S(18), x0 = rc.left + S(36), y0 = rc.top + S(40), dx = S(48);
    if (g.mapSet == 0)
    {
        for (int i = 0; i < 5; i++) { bool o = on(i); Circle(dc, x0 + i * dx, y0, r, o ? fret[i] : Dim(fret[i], 0.25f), o ? RGB(255, 255, 255) : Dim(fret[i], 0.5f)); }
        int ax = rc.left + S(300);
        Triangle(dc, ax, y0 - S(12), S(11), true, on(5) ? kAccent2 : kLine);
        Triangle(dc, ax, y0 + S(14), S(11), false, on(6) ? kAccent2 : kLine);
        uint32_t wm = InputParseSources(Eff(m[11], p).c_str()); int v = 0;
        if (wm & kRT) v = std::max(v, (int)rt);
        if (wm & kLT) v = std::max(v, (int)lt);
        if (wm & (0x0080 | kRSDirs)) v = std::max(v, std::min(255, std::max(std::abs((int)st[2]), std::abs((int)st[3])) >> 7));
        RECT bar{ rc.left + S(16), rc.top + S(84), rc.left + S(240), rc.top + S(94) };
        Chamfer(dc, bar, S(3), kPanel, kLine);
        RECT fill = bar; fill.right = fill.left + (bar.right - bar.left) * v / 255;
        if (v > 4) FillR(dc, fill, kAccent);
        RECT lr{ bar.right + S(10), bar.top - S(6), rc.right - S(8), bar.bottom + S(6) };
        Text(dc, L"Palanca", lr, kDim, g.fontSmall);
        RECT sr{ rc.left + S(16), rc.top + S(110), rc.left + S(200), rc.bottom - S(8) };
        if (any) Text(dc, L"★ Poder estrella", sr, on(10) ? kAccent2 : kLine, g.fontSmall);
        RECT tr{ rc.left + S(160), rc.top + S(110), rc.right - S(14), rc.bottom - S(8) };
        Text(dc, L"START", tr, on(9) ? kAccent2 : kLine, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    else
    {
        const int order[5] = { 1, 2, 3, 4, 0 };   // rojo, amarillo, azul, naranja, verde (como la bateria de 5 piezas)
        for (int k = 0; k < 5; k++) { int i = order[k]; bool o = on(i); Circle(dc, x0 + k * dx, y0, r, o ? fret[i] : Dim(fret[i], 0.25f), o ? RGB(255, 255, 255) : Dim(fret[i], 0.5f)); }
        RECT kick{ rc.left + S(16), rc.top + S(80), rc.left + S(260), rc.top + S(92) };
        Chamfer(dc, kick, S(3), on(5) ? kAccent : kPanel, kLine);
        RECT kr{ kick.right + S(10), kick.top - S(6), rc.right - S(8), kick.bottom + S(6) };
        Text(dc, L"Bombo", kr, on(5) ? kAccent2 : kDim, g.fontSmall);
        RECT tr{ rc.left + S(16), rc.top + S(110), rc.right - S(14), rc.bottom - S(8) };
        Text(dc, L"START", tr, on(8) ? kAccent2 : kLine, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (!any)
    {
        RECT nr{ rc.left + S(16), rc.top + S(110), rc.left + S(280), rc.bottom - S(8) };
        Text(dc, L"Conecta un mando a este jugador", nr, kDim, g.fontSmall);
    }
}
void DrawMeter(HDC dc, RECT rc)
{
    Chamfer(dc, RECT{ rc.left, rc.top, rc.right - 1, rc.bottom - 1 }, S(6), kInput, kLine);
    RECT in = rc; InflateRect(&in, -S(6), -S(6));
    float v = g.meterShown;
    int wfull = in.right - in.left, wv = (int)(wfull * std::min(1.0f, v));
    for (int x = 0; x < wv; x += S(6))
    {
        float f = (float)x / wfull;
        COLORREF c = f < 0.6f ? RGB(60, 200, 90) : f < 0.85f ? RGB(240, 200, 50) : RGB(235, 60, 50);
        RECT seg{ in.left + x, in.top, std::min(in.left + x + S(4), in.right), in.bottom }; FillR(dc, seg, c);
    }
    if (g.meterHold > 0.01f) { int hx = in.left + (int)(wfull * std::min(1.0f, g.meterHold)); RECT hr{ hx - S(1), in.top, hx + S(1), in.bottom }; FillR(dc, hr, kText); }
    if (!g.meterOn) Text(dc, Toggled(ID_MIC_ON) ? L"Abriendo el micrófono..." : L"Micrófono desactivado", in, kDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else if (!g.meter.ok) Text(dc, L"No se pudo abrir este micrófono", in, kDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
// Titulo de seccion: texto espaciado en naranja, raya y tres marcas a la derecha
void DrawSection(HDC dc, RECT rc, const wchar_t* t)
{
    int w = rc.right - rc.left;
    RECT tr{ 0, 0, w, S(26) };
    Text(dc, t, tr, kAccent, g.fontHead, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    LineC(dc, 0, S(30), w, S(30), kLine);
    for (int k = 0; k < 3; k++) LineC(dc, w - S(6) - k * S(8), S(27), w - S(10) - k * S(8), S(33), kAccentDim);
}
// Portada de la pagina Inicio: portada.bmp junto al exe (una captura del juego) o, si no hay, un dibujo de fuego con el titulo
void DrawBanner(HDC dc, RECT rc)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top, band = S(48), ph = h - band;
    if (g.banner)
    {   // portada propia (portada.png/jpg/bmp junto al exe), como la cabecera de la biblioteca de Steam: el arte cubre el ancho, se
        // oscurece hacia la izquierda y encima van las letras del logo del juego (del ISO del usuario)
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -ph, 1, 32, BI_RGB };
        uint32_t* pa = nullptr; HBITMAP ba = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pa, nullptr, 0);
        if (ba && pa)
        {
            HDC mem = CreateCompatibleDC(dc), da = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g.banner), oa = SelectObject(da, ba);
            SetStretchBltMode(da, HALFTONE); SetBrushOrgEx(da, 0, 0, nullptr);
            int sw = g.bannerW, sh = g.bannerW * ph / std::max(1, w);   // cubre el ancho; recorte vertical centrado
            if (sh > g.bannerH) { sh = g.bannerH; sw = g.bannerH * w / std::max(1, ph); }
            StretchBlt(da, 0, 0, w, ph, mem, (g.bannerW - sw) / 2, (g.bannerH - sh) / 2, sw, sh, SRCCOPY);
            GdiFlush();
            if (!g.logoCut.empty())
                for (int x = 0; x < w * 55 / 100; x++)
                {
                    float f = 1.0f - 0.62f * std::pow(1.0f - (float)x / (w * 55 / 100), 1.4f);
                    for (int y = 0; y < ph; y++)
                    {
                        uint32_t c = pa[y * w + x];
                        pa[y * w + x] = ((uint32_t)(((c >> 16) & 255) * f) << 16) | ((uint32_t)(((c >> 8) & 255) * f) << 8) | (uint32_t)((c & 255) * f);
                    }
                }
            BitBlt(dc, 0, 0, w, ph, da, 0, 0, SRCCOPY);
            SelectObject(mem, old); SelectObject(da, oa); DeleteDC(mem); DeleteDC(da);
        }
        if (ba) DeleteObject(ba);
        int lh = ph * 74 / 100, lw = g.cutW * lh / std::max(1, g.cutH);
        if (HBITMAP lb = ScaleCut(1, lw, lh))
        {
            HDC mem = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mem, lb);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, S(40), (ph - lh) / 2, lw, lh, mem, 0, 0, lw, lh, bf);
            SelectObject(mem, old); DeleteDC(mem);
        }
    }
    else
    {
        auto grad = [&](int y) { float f = (float)y / ph; return Mix(RGB(12, 4, 6), RGB(70, 14, 6), f * f); };
        for (int y = 0; y < ph; y++) { RECT r{ 0, y, w, y + 1 }; FillR(dc, r, grad(y)); }
        if (g.logo)
        {   // logo oficial (del ISO del usuario) centrado a toda la altura. Capa A: su fondo de fuego alargado a los lados (columnas
            // del borde de la imagen); capa B: el logo. Se mezclan por pixel en los bordes del logo y se oscurece hacia fuera.
            int lh = ph, lw = g.logoW * lh / std::max(1, g.logoH), lx = (w - lw) / 2, sw = std::max(1, g.logoW * 30 / 320), fade = std::min(S(70), lw / 4);
            BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -ph, 1, 32, BI_RGB };
            uint32_t *pa = nullptr, *pb = nullptr;
            HBITMAP ba = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pa, nullptr, 0), bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pb, nullptr, 0);
            if (ba && bb && pa && pb)
            {
                HDC mem = CreateCompatibleDC(dc), da = CreateCompatibleDC(dc), db = CreateCompatibleDC(dc);
                HGDIOBJ om = SelectObject(mem, g.logo), oa = SelectObject(da, ba), ob2 = SelectObject(db, bb);
                for (HDC x : { da, db }) { SetStretchBltMode(x, HALFTONE); SetBrushOrgEx(x, 0, 0, nullptr); }
                StretchBlt(da, 0, 0, w / 2, ph, mem, 0, 0, sw, g.logoH, SRCCOPY);
                StretchBlt(da, w / 2, 0, w - w / 2, ph, mem, g.logoW - sw, 0, sw, g.logoH, SRCCOPY);
                StretchBlt(db, lx, 0, lw, lh, mem, 0, 0, g.logoW, g.logoH, SRCCOPY);
                GdiFlush();
                for (int x = 0; x < w; x++)
                {
                    float t = std::clamp(std::min((float)(x - lx), (float)(lx + lw - 1 - x)) / (float)std::max(1, fade), 0.0f, 1.0f);
                    t = t * t * (3 - 2 * t);
                    float v = 1.0f;
                    if (x < lx) v = 0.30f + 0.70f * std::pow((float)x / std::max(1, lx), 0.6f);
                    else if (x >= lx + lw) v = 0.30f + 0.70f * std::pow((float)(w - 1 - x) / std::max(1, w - lx - lw), 0.6f);
                    for (int y = 0; y < ph; y++)
                    {
                        uint32_t a = pa[y * w + x], b = pb[y * w + x], o = 0;
                        for (int c = 0; c < 24; c += 8)
                        {
                            float ca = (float)((a >> c) & 255) * v, cb = (float)((b >> c) & 255);
                            o |= (uint32_t)std::clamp(ca + (cb - ca) * t, 0.0f, 255.0f) << c;
                        }
                        pa[y * w + x] = o;
                    }
                }
                BitBlt(dc, 0, 0, w, ph, da, 0, 0, SRCCOPY);
                SelectObject(mem, om); SelectObject(da, oa); SelectObject(db, ob2);
                DeleteDC(mem); DeleteDC(da); DeleteDC(db);
            }
            if (ba) DeleteObject(ba);
            if (bb) DeleteObject(bb);
        }
        else
        {
        // llamas: capas de picos de atras (rojo oscuro) a delante (dorado), siempre iguales
        const COLORREF layer[4] = { RGB(90, 16, 8), RGB(150, 32, 10), RGB(220, 84, 20), RGB(255, 170, 60) };
        uint32_t seed = 12345;
        auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFF; };
        for (int L = 0; L < 4; L++)
        {
            int n = 18 + L * 6, maxH = ph * (66 - L * 12) / 100;
            std::vector<POINT> pts; pts.push_back({ 0, ph });
            for (int i = 0; i <= n; i++)
            {
                int x = w * i / n, half = w / n / 2;
                int hgt = maxH * (40 + (int)(rnd() % 60)) / 100;
                pts.push_back({ x - half, ph - hgt / 4 }); pts.push_back({ x, ph - hgt });
            }
            pts.push_back({ w, ph });
            HBRUSH b = CreateSolidBrush(layer[L]); HPEN p = CreatePen(PS_SOLID, 1, layer[L]);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            Polygon(dc, pts.data(), (int)pts.size());
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
        }
        RECT t1{ 0, S(18), w, S(96) };
        Text(dc, L"GUITAR HERO", RECT{ t1.left + S(4), t1.top + S(4), t1.right + S(4), t1.bottom + S(4) }, RGB(0, 0, 0), g.fontBanner, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(4));
        Text(dc, L"GUITAR HERO", t1, kAccent2, g.fontBanner, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(4));
        RECT t2{ 0, S(96), w, S(124) };
        Text(dc, L"W A R R I O R S   O F   R O C K", RECT{ t2.left + S(2), t2.top + S(2), t2.right + S(2), t2.bottom + S(2) }, RGB(0, 0, 0), g.fontHead, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(2));
        Text(dc, L"W A R R I O R S   O F   R O C K", t2, kText, g.fontHead, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(2));
        for (int y = 0; y < ph; y += S(3)) LineC(dc, 0, y, w, y, RGB(0, 0, 0));   // lineas de barrido (solo en el dibujo)
        }
    }
    RECT bd{ 0, ph, w, h }; FillR(dc, bd, RGB(16, 9, 10));
    LineC(dc, 0, ph, w, ph, kAccent);
    RECT nm{ S(20), ph, w - S(20), h };
    Text(dc, L"GUITAR HERO: WARRIORS OF ROCK", nm, kText, g.fontHead, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(3));
    Text(dc, L"41560883  ·  XBOX 360", nm, kAccentDim, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void DrawItem(DRAWITEMSTRUCT* d)
{
    HDC dc = d->hDC; RECT rc = d->rcItem;
    if (d->CtlType == ODT_STATIC)
    {
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, w, h); HGDIOBJ ob = SelectObject(mem, bm);
        RECT lr{ 0, 0, w, h }; FillR(mem, lr, kPanel);
        if (d->CtlID == ID_TESTPANEL) DrawTestPanel(mem, lr);
        else if (d->CtlID == ID_MIC_METER) DrawMeter(mem, lr);
        else if (d->CtlID == ID_BANNER) DrawBanner(mem, lr);
        else { wchar_t t[128]; GetWindowTextW(d->hwndItem, t, 128); DrawSection(mem, lr, t); }
        BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        return;
    }
    if (d->CtlType == ODT_LISTBOX && d->CtlID == ID_ACHLIST)
    {
        if ((int)d->itemID < 0 || d->itemID >= g.achs.size()) return;
        const AchRow& a = g.achs[d->itemID];
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        FillR(dc, rc, sel ? kSel : (d->itemID % 2 ? kInput : Mix(kInput, kPanel, 0.5f)));
        if (sel) { RECT bar{ rc.left, rc.top, rc.left + S(3), rc.bottom }; FillR(dc, bar, kAccent); }
        RECT ic{ rc.left + S(8), rc.top, rc.left + S(42), rc.bottom };
        Text(dc, L"", ic, a.unlocked ? kAccent2 : kLine, g.fontIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT t1{ rc.left + S(48), rc.top + S(4), rc.right - S(170), rc.top + S(24) };
        Text(dc, a.name.c_str(), t1, a.unlocked ? kText : kDim, a.unlocked ? g.fontBold : g.font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t2{ rc.left + S(48), rc.top + S(24), rc.right - S(12), rc.bottom - S(4) };
        Text(dc, (a.unlocked ? a.desc : a.howto.empty() ? a.desc : a.howto).c_str(), t2, kDim, g.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t3{ rc.right - S(170), rc.top + S(4), rc.right - S(12), rc.top + S(24) };
        std::wstring right = std::to_wstring(a.gs) + L" G" + (a.unlocked ? L"  ·  " + a.date.substr(0, 10) : L"");
        Text(dc, right.c_str(), t3, a.unlocked ? kAccent2 : kDim, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (d->CtlType == ODT_LISTBOX)
    {
        if ((int)d->itemID < 0) return;
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        FillR(dc, rc, sel ? kSel : (d->itemID % 2 ? kInput : Mix(kInput, kPanel, 0.5f)));
        if (sel) { RECT bar{ rc.left, rc.top, rc.left + S(3), rc.bottom }; FillR(dc, bar, kAccent); }
        MapFn& f = CurMap()[d->itemID];
        RECT l{ rc.left + S(12), rc.top, rc.left + S(170), rc.bottom };
        Text(dc, f.label, l, kText, g.font);
        int mp = g.mapPlayer - 1; bool own = mp >= 0 && !f.ovr[mp].empty();
        std::wstring v = W(Eff(f, mp)); size_t pos; while ((pos = v.find(L'|')) != std::wstring::npos) v.replace(pos, 1, L"  /  ");
        if (v.empty()) v = L"(ninguno)";
        if (mp >= 0 && !own) v += L"   (común)";
        bool cap = g.capturing && g.capItem == (int)d->itemID;
        if (cap) v = L"pulsa un botón...";
        RECT r{ rc.left + S(170), rc.top, rc.right - S(8), rc.bottom };
        Text(dc, v.c_str(), r, cap ? kAccent2 : own ? kText : kDim, g.font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (d->CtlType != ODT_BUTTON) return;
    HWND h = d->hwndItem;
    wchar_t t[128]; GetWindowTextW(h, t, 128);
    bool pressed = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0, disabled = (d->itemState & ODS_DISABLED) != 0;
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, w, hh); HGDIOBJ ob = SelectObject(mem, bm);
    RECT r{ 0, 0, w, hh }, box{ 0, 0, w - 1, hh - 1 };
    bool isTab = d->CtlID >= ID_TAB0 && d->CtlID < ID_TAB0 + kTabs, onBar = d->CtlID == ID_SAVE || d->CtlID == ID_PLAY;
    FillR(mem, r, isTab ? kSide : onBar ? kBar : kPanel);
    auto tg = g.toggles.find(h);
    if (isTab)
    {   // pagina de la barra lateral: nombre espaciado; la activa con panel, borde y barra naranja
        int i = d->CtlID - ID_TAB0; bool sel = i == g.tab;
        if (sel) Chamfer(mem, box, S(10), kPanelHi, kAccent);
        else if (pressed) Chamfer(mem, box, S(10), kPanel, kLine);
        std::wstring up = t; CharUpperBuffW(up.data(), (DWORD)up.size());
        RECT tr{ S(22), 0, w, hh };
        Text(mem, up.c_str(), tr, sel ? kText : kDim, g.fontTab, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
        if (sel) LineC(mem, S(10), S(13), S(10), hh - S(13), kAccent, S(3));
    }
    else if (tg != g.toggles.end())
    {   // conmutador SI / NO
        bool on = tg->second;
        Chamfer(mem, box, S(6), on ? kPanelHi : kInput, on || focus ? kAccent : kLine);
        RECT knob = on ? RECT{ box.right - S(36), S(4), box.right - S(4), box.bottom - S(4) } : RECT{ S(4), S(4), S(36), box.bottom - S(4) };
        Chamfer(mem, knob, S(4), on ? kAccent : kLine, CLR_NONE);
        RECT lb = box; if (on) lb.right -= S(38); else lb.left += S(38);
        Text(mem, on ? L"SÍ" : L"NO", lb, on ? kAccent2 : kDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    }
    else if (d->CtlID == ID_PLAY)
    {
        Chamfer(mem, box, S(12), disabled ? kLine : pressed ? RGB(220, 90, 24) : kAccent, CLR_NONE);
        Text(mem, L"JUGAR", r, kPlayText, g.fontPlay, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(4));
    }
    else
    {
        Chamfer(mem, box, S(8), pressed ? kPanelHi : kInput, focus ? kAccent : kLine);
        std::wstring up = t; if (onBar) CharUpperBuffW(up.data(), (DWORD)up.size());
        Text(mem, up.c_str(), r, disabled ? kDim : pressed ? kAccent2 : kText, onBar ? g.fontTab : g.fontBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE, onBar ? S(2) : 0);
    }
    BitBlt(dc, rc.left, rc.top, w, hh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
}
void PaintMain(HDC dc0)
{
    RECT rc; GetClientRect(g.wnd, &rc);
    HDC dc = CreateCompatibleDC(dc0); HBITMAP bm = CreateCompatibleBitmap(dc0, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(dc, bm);
    FillR(dc, rc, kBg);
    for (int y = 0; y < rc.bottom; y += S(3)) LineC(dc, 0, y, rc.right, y, kScan);
    // barra lateral con el titulo
    int side = S(kSideW);
    RECT sb{ 0, 0, side, rc.bottom }; FillR(dc, sb, kSide);
    LineC(dc, side, 0, side, rc.bottom, kLine);
    if (!g.logoCut.empty())
    {   // solo las letras del logo del juego (sin su fondo de fuego), con transparencia sobre la barra lateral
        RECT bd{ S(16), S(14), side - S(16), S(168) };
        int bw = bd.right - bd.left, bh = bd.bottom - bd.top, dw = bw, dh = g.cutH * bw / std::max(1, g.cutW);
        if (dh > bh) { dh = bh; dw = g.cutW * bh / std::max(1, g.cutH); }
        HBITMAP lb = ScaleCut(0, dw, dh);
        if (lb)
        {
            HDC mem = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mem, lb);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, bd.left + (bw - dw) / 2, bd.top + (bh - dh) / 2, dw, dh, mem, 0, 0, dw, dh, bf);
            SelectObject(mem, old); DeleteDC(mem);
        }
    }
    else
    {
        RECT t1{ S(22), S(54), side - S(8), S(94) };
        Text(dc, L"GUITAR HERO", RECT{ t1.left + S(2), t1.top + S(2), t1.right + S(2), t1.bottom + S(2) }, RGB(0, 0, 0), g.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(1));
        Text(dc, L"GUITAR HERO", t1, kAccent2, g.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(1));
        RECT t2{ S(24), S(96), side - S(8), S(116) };
        Text(dc, L"W A R R I O R S   O F   R O C K", t2, kAccent, g.fontSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    LineC(dc, S(24), S(176), side - S(24), S(176), kLine);
    RECT t3{ S(24), S(182), side - S(8), S(204) };
    Text(dc, L"PORT PC", t3, kAccent, g.fontHead, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(4));
    RECT t4{ S(24), S(204), side - S(8), S(222) };
    wchar_t by[64]; swprintf(by, 64, L"por Piscacho  ·  v%hs", AppVersion());
    Text(dc, by, t4, kDim, g.fontSmall);
    RECT note{ S(24), S(kH - 70), side - S(12), S(kH - 16) };
    Text(dc, L"En el juego: F11 o Alt+Intro\npantalla completa  ·  F3 FPS", note, kDim, g.fontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
    // panel de contenido y marcos de la pagina
    RECT panel{ S(kX0 - 14), S(16), S(kXR + 14), S(kPanelBottom) };
    Chamfer(dc, panel, S(18), kPanel, kLine);
    for (auto& [p, fr] : g.frames)
        if (p == g.tab || p < 0) Chamfer(dc, RECT{ S(fr.left), S(fr.top), S(fr.right), S(fr.bottom) }, S(6), kInput, kLine);
    // barra inferior
    RECT bar{ side + 1, S(kBarTop), rc.right, rc.bottom }; FillR(dc, bar, kBar);
    LineC(dc, side, S(kBarTop), rc.right, S(kBarTop), kLine);
    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
}
void LoadAchievements();
void UpdateHome();
void ShowTab(int t)
{
    g.tab = t;
    for (int p = 0; p < kTabs; p++) for (HWND h : g.pages[p]) ShowWindow(h, p == t ? SW_SHOW : SW_HIDE);
    InvalidateRect(g.wnd, nullptr, TRUE);
    for (int i = 0; i < kTabs; i++) InvalidateRect(Item(ID_TAB0 + i), nullptr, TRUE);
    if (t == 5) LoadAchievements();
    if (t == 6 && Toggled(ID_NET_ON)) NetLauncherStart();
    if (t == kHome) UpdateHome();
    if (t != 3) { g.meter.Stop(); g.meterOn = false; }
}
void StartMeter()
{
    g.meter.Stop(); g.meterOn = false;
    if (!Toggled(ID_MIC_ON)) return;
    int s = Sel(ID_MIC_DEV);
    g.meter.Start(s <= 0 || s > (int)g.mics.size() ? L"" : g.mics[s - 1].first);
    g.meterOn = true;
}
float Gain() { return (float)SendMessageW(Item(ID_MIC_GAIN), TBM_GETPOS, 0, 0) / 100.0f; }
void UpdateGainLabel() { wchar_t t[32]; swprintf(t, 32, L"x %.2f", Gain()); SetWindowTextW(Item(ID_MIC_GAINLBL), t); }
void UpdateDlcCount()
{
    std::wstring dir = GetText(Item(ID_DLC)); int n = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do
        {   // los paquetes de 360 no tienen extension; los .txt/.md (LEEME) no cuentan
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            bool doc = ext && (!_wcsicmp(ext, L".txt") || !_wcsicmp(ext, L".md"));
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !doc) n++;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    wchar_t t[160];
    if (dir.empty()) wcscpy(t, L"Sin carpeta de DLC");
    else if (f == INVALID_HANDLE_VALUE) wcscpy(t, L"La carpeta no existe");
    else swprintf(t, 160, L"%d archivo%s en la carpeta (se importan al arrancar, una sola vez)", n, n == 1 ? L"" : L"s");
    SetWindowTextW(Item(ID_DLCCOUNT), t);
}
void BrowseIso()
{
    wchar_t file[MAX_PATH] = {}; std::wstring cur = GetText(Item(ID_ISO)); wcsncpy(file, cur.c_str(), MAX_PATH - 1);
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = g.wnd;
    o.lpstrFilter = L"ISO de Xbox 360 (*.iso)\0*.iso\0Todos los archivos\0*.*\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Selecciona el ISO de Guitar Hero: Warriors of Rock (Xbox 360)"; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) SetWindowTextW(Item(ID_ISO), file);
}
void BrowseFolder(int editId)
{
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Carpeta con los paquetes de DLC");
    if (SUCCEEDED(d->Show(g.wnd)))
    {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { SetWindowTextW(Item(editId), p); CoTaskMemFree(p); } it->Release(); }
    }
    d->Release();
}

// ---- crear la ventana a partir de gh6.ini ----
// Sesion 28: paginas con filas (nombre + descripcion a la izquierda, control a la derecha) como el launcher de Dead Space
void BuildUi()
{
    IniDoc& ini = g.ini;
    auto flag = [&](const char* k, bool def) { std::string v = ini.Get(k, def ? "1" : "0"); return v != "0"; };
    // Barra lateral (por orden de kTabOrder) y barra inferior
    const wchar_t* tabs[kTabs] = { L"Juego", L"Gráficos", L"Controles", L"Audio", L"Ayuda", L"Logros", L"En línea", L"Inicio" };
    for (int i = 0; i < kTabs; i++) Button(-1, tabs[i], 16, 236 + TabPos(i) * 48, kSideW - 32, 40, ID_TAB0 + i);
    Label(-1, L"", kX0, kBarTop + 26, 380, 22, ID_SAVED, true);
    Button(-1, L"Guardar", kW - 390, kBarTop + 16, 140, 42, ID_SAVE);
    Button(-1, L"JUGAR", kW - 234, kBarTop + 12, 200, 50, ID_PLAY);

    // Opciones compartidas por Inicio y las otras paginas
    std::string rs = ini.Get("resolucion_interna", "1"), pn = ini.Get("panoramico", "16:9");
    const std::vector<std::wstring> resItems = { L"720p (como la consola)", L"1440p (2x, recomendada)", L"2160p / 4K (3x, PC potente)" };
    const std::vector<std::wstring> panoItems = { L"16:9 (como la consola)", L"Panorámica del monitor", L"21:9 (experimental)", L"32:9 (experimental)" };
    int resSel = rs == "2" ? 1 : rs == "3" ? 2 : 0, panoSel = pn == "auto" ? 1 : pn == "21:9" ? 2 : pn == "32:9" ? 3 : 0;

    // ---- Inicio: portada y ajustes rapidos ----
    {
        int y = kY0;
        Make(kHome, L"STATIC", L"", SS_OWNERDRAW, kX0, y, kXR - kX0, 244, ID_BANNER);
        y = Section(kHome, y + 258, L"AJUSTES RÁPIDOS");
        Row(kHome, y, L"Resolución interna", L"El juego se dibuja a 2x o 3x la resolución de la consola: más nítido."); ComboRight(kHome, y, ID_Q_RES, resItems, resSel); y += 44;
        Row(kHome, y, L"Proporción de la imagen", L"21:9 y 32:9 amplían la vista a los lados; menús y HUD quedan en el centro."); ComboRight(kHome, y, ID_Q_PANO, panoItems, panoSel); y += 44;
        Row(kHome, y, L"Pantalla completa", L"Sin bordes, en el monitor de la ventana. F11 la alterna en el juego."); ToggleRight(kHome, y, ID_Q_FULL, flag("pantalla_completa", false)); y += 44;
        Row(kHome, y, L"Juego en línea (experimental)", L"Partidas con otros jugadores de esta versión. Más opciones en EN LÍNEA."); ToggleRight(kHome, y, ID_Q_NET, flag("online", false)); y += 48;
        g.frames.push_back({ kHome, RECT{ kX0, y, kXR, y + 80 } });
        Label(kHome, L"", kX0 + 16, y + 8, kXR - kX0 - 32, 20, ID_HOME_ISO);
        Label(kHome, L"", kX0 + 16, y + 31, kXR - kX0 - 32, 18, ID_HOME_UPD, true, g.fontSmall);
        Label(kHome, L"Teclado: flechas  ·  Intro = Start  ·  1-5 = trastes  ·  Retroceso = poder estrella  ·  F6 = instrumento del teclado",
              kX0 + 16, y + 52, kXR - kX0 - 32, 18, ID_HOME_KEYS, true, g.fontSmall);
    }

    // ---- Juego ----
    {
        int y = Section(0, kY0, L"ARCHIVOS DEL JUEGO");
        Row(0, y, L"ISO del juego (Xbox 360)", L"Guitar Hero - Warriors of Rock (USA, Europe) (En,Fr,De,Es,It)"); y += 42;
        Edit(0, W(ini.Get("iso")), kX0, y, kXR - kX0 - 140, ID_ISO);
        Button(0, L"Examinar...", kXR - 130, y, 130, 30, ID_ISO_BROWSE); y += 42;
        Row(0, y, L"Carpeta de DLC", nullptr, 520, ID_DLCCOUNT); y += 42;
        Edit(0, W(ini.Get("dlc", ExeDir() + "\\dlc")), kX0, y, kXR - kX0 - 140, ID_DLC);
        Button(0, L"Examinar...", kXR - 130, y, 130, 30, ID_DLC_BROWSE); y += 42;
        Row(0, y, L"Saltar el vídeo de logos al arrancar", L"Entra directo a la pantalla de título."); ToggleRight(0, y, ID_SKIPINTRO, flag("saltar_intro", true)); y += 46;
        y = Section(0, y, L"PARTIDAS");
        Label(0, L"", kX0, y, kXR - kX0, 20, ID_SAVEINFO, true, g.fontSmall); y += 24;
        Button(0, L"Abrir carpeta", kX0, y, 160, 32, ID_SAVEOPEN);
        Button(0, L"Empezar de cero", kX0 + 172, y, 180, 32, ID_SAVERESET);
        Button(0, L"Restaurar respaldo", kX0 + 364, y, 190, 32, ID_SAVERESTORE); y += 44;
        y = Section(0, y, L"ACTUALIZACIONES Y LAUNCHER");
        Label(0, L"", kX0, y, kXR - kX0 - 240, 20, ID_UPDINFO, true, g.fontSmall);
        Button(0, L"Buscar actualizaciones", kXR - 230, y - 4, 230, 30, ID_UPDCHECK); y += 32;
        Row(0, y, L"Buscar actualizaciones al abrir", L"Avisa si hay una versión nueva en GitHub."); ToggleRight(0, y, ID_UPDAUTO, flag("actualizar_al_abrir", true)); y += 42;
        Row(0, y, L"Mostrar este launcher al iniciar", L"Si lo desactivas, mantén Mayús al abrir el juego para verlo."); ToggleRight(0, y, ID_SHOWLAUNCHER, flag("mostrar_launcher", true));
    }

    // ---- Graficos ----
    {
        int ws = 0; std::string v = ini.Get("ventana", "1280x720");
        for (int i = 0; i < kNumWinSizes; i++) if (v == kWinSizesIni[i]) ws = i;
        std::string an = ini.Get("anisotropico", "16"), ni = ini.Get("nitidez", "0.4");
        int y = Section(1, kY0, L"IMAGEN");
        Row(1, y, L"Resolución interna", L"1440p va igual de fluido que 720p en un PC actual; 4K pide una tarjeta potente."); ComboRight(1, y, ID_RES, resItems, resSel); y += 42;
        Row(1, y, L"Proporción de la imagen (experimental)", L"Más campo de visión a los lados; algunos ángulos de cámara aún fallan."); ComboRight(1, y, ID_PANO, panoItems, panoSel); y += 42;
        { std::string aa = ini.Get("suavizado", "fxaa");
          Row(1, y, L"Suavizado de bordes", L"Quita los dientes de sierra. SMAA lo hace sin emborronar textos, notas ni cuerdas.");
          ComboRight(1, y, ID_AA, { L"FXAA", L"SMAA (más nítido)", L"Desactivado" }, aa == "no" ? 2 : aa == "smaa" ? 1 : 0); y += 42; }
        Row(1, y, L"Filtrado de texturas", L"Texturas nítidas en ángulo (suelo, mástil)."); ComboRight(1, y, ID_ANISO, { L"Anisotrópico 16x", L"Anisotrópico 8x", L"Anisotrópico 4x", L"Trilineal (consola)" },
              an == "8" ? 1 : an == "4" ? 2 : an == "1" ? 3 : 0); y += 42;
        Row(1, y, L"Nitidez", L"Realce de detalle (CAS)."); ComboRight(1, y, ID_SHARP, { L"Desactivada", L"Suave", L"Media (recomendada)", L"Alta" }, ni == "0" ? 0 : ni == "0.25" ? 1 : ni == "0.7" ? 3 : 2); y += 42;
        Row(1, y, L"Desenfoque de movimiento", L"Como en la consola."); ToggleRight(1, y, ID_MBLUR, flag("desenfoque_movimiento", true)); y += 42;
        Row(1, y, L"Grano de película", L"Como en la consola."); ToggleRight(1, y, ID_GRAIN, flag("grano_pelicula", false)); y += 44;
        y = Section(1, y, L"PANTALLA");
        Row(1, y, L"Pantalla completa al arrancar", L"Sin bordes. F11 o Alt+Intro la alternan en el juego."); ToggleRight(1, y, ID_FULLSCREEN, flag("pantalla_completa", false)); y += 42;
        Row(1, y, L"Tamaño de la ventana", L"Solo en modo ventana; en pantalla completa se usa el monitor."); ComboRight(1, y, ID_WINSIZE, { kWinSizes[0], kWinSizes[1], kWinSizes[2], kWinSizes[3], kWinSizes[4], kWinSizes[5], kWinSizes[6], kWinSizes[7], kWinSizes[8] }, ws); y += 42;
        Row(1, y, L"Escalado de la imagen", L"Cómo se adapta la imagen a la ventana."); ComboRight(1, y, ID_SCALE, { L"Nítido (Catmull-Rom)", L"Suave (bilineal)" }, ini.Get("escalado") == "bilineal" ? 1 : 0); y += 42;
        Row(1, y, L"Sincronización vertical (VSync)", L"Evita el desgarro de la imagen."); ToggleRight(1, y, ID_VSYNC, flag("vsync", true)); y += 42;
        Row(1, y, L"Contador de FPS", L"Se muestra y oculta con F3."); ToggleRight(1, y, ID_FPS, flag("mostrar_fps", false));
    }

    // ---- Controles ----
    {
        int y = Section(2, kY0, L"JUGADORES");
        Label(2, L"JUGADOR", kX0, y, 100, 18, 0, true, g.fontSmall); Label(2, L"INSTRUMENTO", kX0 + 110, y, 180, 18, 0, true, g.fontSmall);
        Label(2, L"MANDO", kX0 + 300, y, 180, 18, 0, true, g.fontSmall); Label(2, L"ESTADO", kX0 + 492, y, 300, 18, 0, true, g.fontSmall); y += 22;
        std::string def1 = ini.Get("mando_instrumento", "guitarra");
        const char* instDef[4] = { def1.c_str(), "guitarra", "bateria", "voz" };
        std::vector<std::wstring> padItems = { L"Mando XInput 1", L"Mando XInput 2", L"Mando XInput 3", L"Mando XInput 4", L"Ninguno" };
        for (int p = 0; p < 4; p++)
        {
            char k[40]; snprintf(k, sizeof(k), "jugador%d_instrumento", p + 1);
            std::string in = ini.Get(k, instDef[p]); int is = in == "bateria" ? 1 : in == "voz" ? 2 : 0;
            snprintf(k, sizeof(k), "jugador%d_xinput", p + 1);
            int x = atoi(ini.Get(k, std::to_string(p + 1)).c_str());
            wchar_t lbl[16]; swprintf(lbl, 16, L"Jugador %d", p + 1);
            Label(2, lbl, kX0, y + 5, 100, 22, 0, false, g.fontBold);
            Combo(2, kX0 + 110, y, 180, ID_INST0 + p, { L"Guitarra", L"Batería", L"Voz" }, is);
            Combo(2, kX0 + 300, y, 180, ID_PAD0 + p, padItems, x >= 1 && x <= 4 ? x - 1 : 4);
            Label(2, L"", kX0 + 492, y + 6, kXR - kX0 - 492, 20, ID_STATUS0 + p, true, g.fontSmall);
            y += 36;
        }
        std::vector<std::wstring> players = { L"Jugador 1", L"Jugador 2", L"Jugador 3", L"Jugador 4" };
        Label(2, L"Teclado", kX0, y + 5, 100, 22);
        Combo(2, kX0 + 110, y, 180, ID_KBD, players, atoi(ini.Get("teclado_jugador", "1").c_str()) - 1);
        Label(2, L"Mando PlayStation", kX0 + 300, y + 5, 150, 22);
        Combo(2, kX0 + 452, y, 180, ID_PS, players, atoi(ini.Get("mando_ps_jugador", "1").c_str()) - 1); y += 40;
        Label(2, L"Mando normal (sin convertirlo en guitarra o batería)", kX0, y + 5, 400, 22);
        Toggle(2, L"", kX0 + 452, y + 2, 84, ID_PADMODE, ini.Get("mando_modo", "guitarra") == "mando");
        Button(2, L"Informe de mandos", kXR - 190, y, 190, 30, ID_DIAG); y += 40;
        y = Section(2, y, L"BOTONES");
        Combo(2, kX0, y, 150, ID_MAPSET, { L"Guitarra", L"Batería" }, 0);
        Combo(2, kX0 + 160, y, 210, ID_MAPPLAYER, { L"Todos los jugadores", L"Solo jugador 1", L"Solo jugador 2", L"Solo jugador 3", L"Solo jugador 4" }, 0);
        Button(2, L"Asignar...", kX0 + 382, y, 110, 30, ID_ASSIGN);
        Button(2, L"Añadir...", kX0 + 502, y, 100, 30, ID_ADD);
        Button(2, L"Preajustes", kXR - 120, y, 120, 30, ID_RESET); y += 40;
        List(2, kX0, y, 404, kPanelBottom - 14 - y, ID_MAPLIST, WS_TABSTOP);
        Label(2, L"Probar", kX0 + 420, y + 5, 70, 22, 0, true);
        Combo(2, kX0 + 496, y, 170, ID_TESTPLAYER, players, 0);
        Make(2, L"STATIC", L"", SS_OWNERDRAW, kX0 + 420, y + 40, kXR - kX0 - 420, kPanelBottom - 14 - y - 40, ID_TESTPANEL);
        FillMapList();
    }

    // ---- Audio (latencia y microfono) ----
    {
        std::string al = ini.Get("latencia_audio", "normal");
        int y = Section(3, kY0, L"SONIDO");
        Row(3, y, L"Latencia del audio", L"Baja puede dar algún chasquido en las cargas; Segura para PCs lentos."); ComboRight(3, y, ID_AUDIOLAT, { L"Baja (~40 ms)", L"Normal (~55 ms)", L"Segura (~95 ms)" }, al == "baja" ? 0 : al == "segura" ? 2 : 1); y += 42;
        Label(3, L"Para que notas y música coincidan, calibra una vez en el juego: Options > Calibrate Lag > Calibrate.", kX0, y, kXR - kX0, 18, 0, true, g.fontSmall); y += 32;
        y = Section(3, y, L"MICRÓFONO");
        g.mics.clear(); MicListDevices(g.mics);
        std::vector<std::wstring> micItems = { L"Predeterminado de Windows" }; int ms = 0;
        std::string devIni = ini.Get("microfono_dispositivo");
        for (size_t i = 0; i < g.mics.size(); i++) { micItems.push_back(g.mics[i].second); if (!devIni.empty() && A(g.mics[i].first, CP_UTF8) == devIni) ms = (int)i + 1; }
        Row(3, y, L"Micrófono de Windows", L"Hace de micrófono USB de la consola para la voz."); ToggleRight(3, y, ID_MIC_ON, flag("microfono", true)); y += 42;
        Row(3, y, L"Dispositivo", L""); ComboRight(3, y, ID_MIC_DEV, micItems, ms, 440); y += 42;
        Row(3, y, L"Ganancia", L"Si la barra apenas se mueve al cantar, súbela; si llega al rojo, bájala.", 420);
        Label(3, L"x 1.00", kXR - 72, y + 9, 72, 22, ID_MIC_GAINLBL);
        HWND tb = Make(3, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP | WS_CLIPSIBLINGS, kXR - 380, y + 4, 300, 30, ID_MIC_GAIN);
        SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(25, 400)); SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(tb, TBM_SETPOS, TRUE, (LPARAM)std::clamp((int)lround(atof(ini.Get("microfono_ganancia", "1.0").c_str()) * 100), 25, 400));
        y += 46;
        Label(3, L"Nivel (habla o canta para probarlo)", kX0, y, 400, 21); y += 26;
        Make(3, L"STATIC", L"", SS_OWNERDRAW, kX0, y, kXR - kX0, 32, ID_MIC_METER); y += 42;
        Label(3, L"El micrófono se asigna al primer jugador con instrumento Voz (página CONTROLES).", kX0, y, kXR - kX0, 18, 0, true, g.fontSmall);
    }

    // ---- En linea (sesion 27): online P2P sin servidores ----
    {
        char un[64] = {}; DWORD unl = sizeof(un); GetUserNameA(un, &unl); un[15] = 0;
        int y = Section(6, kY0, L"JUEGO EN LÍNEA (EXPERIMENTAL)");
        Row(6, y, L"Activar el juego en línea", L"Con otros jugadores de esta versión para PC. Sin servidores: conexión directa."); ToggleRight(6, y, ID_NET_ON, flag("online", false)); y += 44;
        Row(6, y, L"Tu nombre", L"Hasta 15 letras.", 360); SendMessageW(Edit(6, W(ini.Get("online_nombre", un)), kXR - 280, y + 4, 280, ID_NET_NAME), EM_SETLIMITTEXT, 15, 0); y += 44;
        Row(6, y, L"Tu código de sala", L"Pásaselo a tu amigo para encontrarte por internet.", 360);
        SendMessageW(Edit(6, L"", kXR - 400, y + 4, 280, ID_NET_CODE), EM_SETREADONLY, TRUE, 0);
        Button(6, L"Copiar", kXR - 110, y + 4, 110, 30, ID_NET_COPY); y += 46;
        Label(6, L"Activa el juego en línea para ver tu código.", kX0, y, kXR - kX0, 40, ID_NET_STATUS, true, g.fontSmall); y += 44;
        Row(6, y, L"Códigos de tus amigos", L"Separados por comas. En la misma red o en una LAN virtual no hacen falta."); y += 42;
        Edit(6, W(ini.Get("online_amigos")), kX0, y, kXR - kX0, ID_NET_PEERS); y += 44;
        y = Section(6, y, L"CÓMO JUGAR");
        Label(6, L"1. Abrid el juego con el juego en línea activado.\n"
                 L"2. Entrad en Quickplay+ y pulsad FIND Xbox LIVE PLAYERS (botón naranja): el juego os junta solo.\n"
                 L"3. Elegís canciones por turnos. Solo se pueden tocar las canciones y DLC que tengáis todos.\n"
                 L"Windows puede pedir permiso al cortafuegos la primera vez (acepta en redes privadas).", kX0, y, kXR - kX0, 90, 0, true, g.fontSmall);
    }

    // ---- Logros (sesion 25): la lista la escribe el juego en la carpeta de partidas (logros_lista.txt) ----
    {
        int y = Section(5, kY0, L"LOGROS");
        Label(5, L"", kX0, y, kXR - kX0, 20, ID_ACHINFO, true, g.fontSmall); y += 28;
        List(5, kX0, y, kXR - kX0, kPanelBottom - 14 - y, ID_ACHLIST, 0);
    }

    // ---- Ayuda ----
    {
        int y = Section(4, kY0, L"TECLAS DURANTE EL JUEGO");
        Label(4, L"F11 o Alt+Intro: pantalla completa  ·  F3: contador de FPS  ·  F12: captura (shot_9xx.bmp)\n"
                 L"F6: cambia el instrumento del jugador del teclado (guitarra, batería, voz)\n"
                 L"Teclado: flechas, Intro = Start, Retroceso = poder estrella, Espacio/1 = verde, Esc/2 = rojo, 3 = amarillo,\n"
                 L"4 = azul, 5 = naranja (en batería 5 = platillo naranja, K o B = bombo), Q/E = gatillos", kX0, y, kXR - kX0, 84, 0, true, g.fontSmall); y += 92;
        y = Section(4, y, L"VARIOS JUGADORES");
        Label(4, L"Cada mando es un jugador; elige su instrumento en CONTROLES. Los jugadores 2 a 4 entran como invitados:\n"
                 L"en el aviso de perfil elige \"Continue without saving\". La voz usa el micrófono de Windows.", kX0, y, kXR - kX0, 44, 0, true, g.fontSmall); y += 52;
        y = Section(4, y, L"SINCRONÍA");
        Label(4, L"Calibra una vez en el juego: Options > Calibrate Lag > Calibrate.", kX0, y, kXR - kX0, 22, 0, true, g.fontSmall); y += 30;
        y = Section(4, y, L"ARCHIVOS");
        Label(4, L"gh6.ini: toda la configuración (lo que cambias aquí).  gh6.log: registro de la última partida.\n"
                 L"Carpeta de partidas: partidas, DLC importado y respaldos. Consérvala (y gh6.ini) al actualizar a mano.\n"
                 L"portada.jpg / .png / .bmp junto al ejecutable: imagen propia para la portada de INICIO (p. ej. un hero de SteamGridDB).", kX0, y, kXR - kX0, 64, 0, true, g.fontSmall);
    }
    UpdateGainLabel(); UpdateDlcCount(); PollPads(); UpdateStatus();
}
// Inicio: estado del ISO y de la version
HBITMAP DecodeImage(const std::vector<uint8_t>& d, int& w, int& h)
{   // PNG (u otro formato de WIC) -> DIB de 32 bits
    HBITMAP hb = nullptr; IWICImagingFactory* f = nullptr; IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&s))
        && SUCCEEDED(s->InitializeFromMemory((BYTE*)d.data(), (DWORD)d.size())) && SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec))
        && SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv))
        && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    {
        UINT W = 0, H = 0; cv->GetSize(&W, &H);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)W, -(LONG)H, 1, 32, BI_RGB };
        void* bits = nullptr; hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hb && bits && SUCCEEDED(cv->CopyPixels(nullptr, W * 4, W * H * 4, (BYTE*)bits))) { w = (int)W; h = (int)H; }
        else if (hb) { DeleteObject(hb); hb = nullptr; }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ cv, fr, dec, s, f }) if (u) u->Release();
    return hb;
}
// Logo oficial (data\memcard\ICON0.png del ISO del usuario): portada de Inicio e insignia de la barra lateral
void LoadIsoLogo(const std::wstring& iso)
{
    if (iso.empty() || iso == g.logoIso) return;
    g.logoIso = iso;
    auto png = IsoExtract(A(iso), "data\\memcard\\ICON0.png");
    if (png.empty()) return;
    int w = 0, h = 0; HBITMAP b = DecodeImage(png, w, h);
    if (!b) return;
    if (g.logo) DeleteObject(g.logo);
    g.logo = b; g.logoW = w; g.logoH = h;
    {   // solo las letras: el fondo de fuego es naranja/marron (saturado) y las letras blancas, grises y negras (sin color)
        DIBSECTION ds{}; GetObject(b, sizeof(ds), &ds);
        const uint32_t* src = (const uint32_t*)ds.dsBm.bmBits;
        int x0 = w * 40 / 320, x1 = w * 280 / 320, y1 = h * 168 / 176;   // zona del logo en ICON0 (320x176)
        g.cutW = x1 - x0; g.cutH = y1; g.logoCut.assign((size_t)g.cutW * g.cutH, 0);
        if (src)
            for (int y = 0; y < g.cutH; y++)
                for (int x = 0; x < g.cutW; x++)
                {
                    uint32_t c = src[y * w + x0 + x];
                    int bl = c & 255, gr = (c >> 8) & 255, rd = (c >> 16) & 255, mx = std::max({ rd, gr, bl }), mn = std::min({ rd, gr, bl });
                    float sat = mx ? (float)(mx - mn) / mx : 0.0f, t = std::clamp((sat - 0.25f) / 0.20f, 0.0f, 1.0f);
                    float a = 1.0f - t * t * (3 - 2 * t);
                    g.logoCut[y * g.cutW + x] = ((uint32_t)(a * 255 + 0.5f) << 24) | ((uint32_t)(rd * a + 0.5f) << 16) | ((uint32_t)(gr * a + 0.5f) << 8) | (uint32_t)(bl * a + 0.5f);
                }
        for (int k = 0; k < 2; k++) if (g.cutBmp[k]) { DeleteObject(g.cutBmp[k]); g.cutBmp[k] = nullptr; }
    }
    if (g.wnd) { InvalidateRect(g.wnd, nullptr, TRUE); InvalidateRect(Item(ID_BANNER), nullptr, FALSE); }
}
void UpdateHome()
{
    std::wstring iso = GetText(Item(ID_ISO));
    if (GetFileAttributesW(iso.c_str()) != INVALID_FILE_ATTRIBUTES) LoadIsoLogo(iso);
    g.isoMissing = iso.empty() || GetFileAttributesW(iso.c_str()) == INVALID_FILE_ATTRIBUTES;
    std::wstring t = g.isoMissing ? L"Falta el ISO del juego: elígelo en la página JUEGO para poder jugar."
                                  : L"ISO del juego: " + iso.substr(iso.find_last_of(L"\\/") + 1);
    SetWindowTextW(Item(ID_HOME_ISO), t.c_str());
    SetWindowTextW(Item(ID_HOME_UPD), GetText(Item(ID_UPDINFO)).c_str());
}
// Valores de la ventana -> gh6.ini
bool Collect(bool requireIso)
{
    IniDoc& ini = g.ini;
    std::string iso = A(GetText(Item(ID_ISO)));
    if (requireIso && (iso.empty() || GetFileAttributesA(iso.c_str()) == INVALID_FILE_ATTRIBUTES))
    {
        ShowTab(0); InvalidateRect(g.wnd, nullptr, TRUE);
        MessageBoxW(g.wnd, L"Elige el ISO de Guitar Hero: Warriors of Rock (Xbox 360) para poder jugar.", L"GH:WoR", MB_ICONINFORMATION);
        return false;
    }
    ini.Set("iso", iso);
    ini.Set("dlc", A(GetText(Item(ID_DLC))));
    ini.Set("saltar_intro", Toggled(ID_SKIPINTRO) ? "1" : "0");
    ini.Set("actualizar_al_abrir", Toggled(ID_UPDAUTO) ? "1" : "0");
    ini.Set("mostrar_launcher", Toggled(ID_SHOWLAUNCHER) ? "1" : "0");
    ini.Set("pantalla_completa", Toggled(ID_FULLSCREEN) ? "1" : "0");
    ini.Set("ventana", kWinSizesIni[std::clamp(Sel(ID_WINSIZE), 0, kNumWinSizes - 1)]);
    ini.Set("escalado", Sel(ID_SCALE) == 1 ? "bilineal" : "catmull-rom");
    ini.Set("vsync", Toggled(ID_VSYNC) ? "1" : "0");
    ini.Set("mostrar_fps", Toggled(ID_FPS) ? "1" : "0");
    ini.Set("desenfoque_movimiento", Toggled(ID_MBLUR) ? "1" : "0");
    ini.Set("grano_pelicula", Toggled(ID_GRAIN) ? "1" : "0");
    { static const char* aa[] = { "fxaa", "smaa", "no" }; ini.Set("suavizado", aa[std::clamp(Sel(ID_AA), 0, 2)]); }
    { static const char* an[] = { "16", "8", "4", "1" }; ini.Set("anisotropico", an[std::clamp(Sel(ID_ANISO), 0, 3)]); }
    { static const char* ni[] = { "0", "0.25", "0.4", "0.7" }; ini.Set("nitidez", ni[std::clamp(Sel(ID_SHARP), 0, 3)]); }
    { static const char* rr[] = { "1", "2", "3" }; ini.Set("resolucion_interna", rr[std::clamp(Sel(ID_RES), 0, 2)]); }
    { static const char* pp[] = { "16:9", "auto", "21:9", "32:9" }; ini.Set("panoramico", pp[std::clamp(Sel(ID_PANO), 0, 3)]); }
    { static const char* lat[] = { "baja", "normal", "segura" }; ini.Set("latencia_audio", lat[std::clamp(Sel(ID_AUDIOLAT), 0, 2)]); }
    for (int p = 0; p < 4; p++)
    {
        char k[40];
        snprintf(k, sizeof(k), "jugador%d_instrumento", p + 1); ini.Set(k, kInstIni[std::max(0, Sel(ID_INST0 + p))]);
        int slot = Sel(ID_PAD0 + p);
        snprintf(k, sizeof(k), "jugador%d_xinput", p + 1); ini.Set(k, std::to_string(slot >= 0 && slot < 4 ? slot + 1 : 0));
    }
    ini.Set("mando_instrumento", kInstIni[std::max(0, Sel(ID_INST0))]);
    ini.Set("teclado_jugador", std::to_string(Sel(ID_KBD) + 1));
    ini.Set("mando_ps_jugador", std::to_string(Sel(ID_PS) + 1));
    ini.Set("mando_modo", Toggled(ID_PADMODE) ? "mando" : "guitarra");
    auto saveMap = [&](MapFn* m, int n) {
        for (int i = 0; i < n; i++)
        {
            ini.Set(m[i].key, m[i].val);
            for (int p = 0; p < 4; p++)
            {
                std::string k = "jugador" + std::to_string(p + 1) + "_" + m[i].key;
                if (!m[i].ovr[p].empty() && m[i].ovr[p] != m[i].val) ini.Set(k, m[i].ovr[p]); else ini.Remove(k);
            }
        }
    };
    saveMap(g_guitar, kGuitarN); saveMap(g_drum, kDrumN);
    ini.Set("microfono", Toggled(ID_MIC_ON) ? "1" : "0");
    ini.Set("online", Toggled(ID_NET_ON) ? "1" : "0");
    ini.Set("online_nombre", A(GetText(Item(ID_NET_NAME)), 20127));   // ASCII (la consola usa gamertags ASCII; á -> a)
    ini.Set("online_amigos", A(GetText(Item(ID_NET_PEERS))));
    int ms = Sel(ID_MIC_DEV);
    ini.Set("microfono_dispositivo", ms >= 1 && ms <= (int)g.mics.size() ? A(g.mics[ms - 1].first, CP_UTF8) : "");
    char gb[16]; snprintf(gb, sizeof(gb), "%.2f", Gain()); ini.Set("microfono_ganancia", gb);
    return true;
}

// ---- informe de mandos (para configurar guitarras/baterias que no tenemos): tipo de cada mando y 15 s de pulsaciones ----
void DiagStart()
{
    if (g.diag) return;
    std::string path = ExeDir() + "\\mandos.txt";
    g.diag = fopen(path.c_str(), "w");
    if (!g.diag) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g.diag, "Informe de mandos - GH:WoR PC v%s - %04d-%02d-%02d %02d:%02d\n", AppVersion(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    fprintf(g.diag, "Subtipos XInput: 1 mando, 6 guitarra, 7 guitarra alternativa, 8 bateria, 11 bajo\n\n");
    for (int i = 0; i < 4; i++)
    {
        XINPUT_CAPABILITIES c{}; XINPUT_STATE x{};
        if (XInputGetState(i, &x) != ERROR_SUCCESS) { fprintf(g.diag, "Mando XInput %d: sin conectar\n", i + 1); continue; }
        XInputGetCapabilities(i, 0, &c);
        fprintf(g.diag, "Mando XInput %d: tipo %u, subtipo %u, flags 0x%04X, botones 0x%04X, gatillos %u/%u, sticks %d/%d/%d/%d\n", i + 1, c.Type, c.SubType, c.Flags,
                c.Gamepad.wButtons, c.Gamepad.bLeftTrigger, c.Gamepad.bRightTrigger, c.Gamepad.sThumbLX, c.Gamepad.sThumbLY, c.Gamepad.sThumbRX, c.Gamepad.sThumbRY);
    }
    uint16_t pb; uint8_t pl, pr; int16_t ps[4];
    fprintf(g.diag, "Mando PlayStation: %s\n\nPulsaciones (cada cambio):\n", PsPadGetState(pb, pl, pr, ps) ? "conectado" : "no");
    for (auto& l : g.diagLast) l.clear();
    g.diagStart = GetTickCount(); g.diagUntil = g.diagStart + 15000;
}
void DiagTick()
{
    if (!g.diag) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < 4; i++)
    {
        if (!g.padOk[i]) continue;
        auto& gp = g.pads[i].Gamepad;
        char line[256];
        snprintf(line, sizeof(line), "botones=%s LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d", gp.wButtons ? BitsToNames(gp.wButtons).c_str() : "-", gp.bLeftTrigger, gp.bRightTrigger,
                 gp.sThumbLX / 1024 * 1024, gp.sThumbLY / 1024 * 1024, gp.sThumbRX / 1024 * 1024, gp.sThumbRY / 1024 * 1024);   // redondeo: sin ruido
        if (g.diagLast[i] != line) { g.diagLast[i] = line; fprintf(g.diag, "t=%5.2f s  mando %d: %s\n", (now - g.diagStart) / 1000.0, i + 1, line); }
    }
    wchar_t t[160]; swprintf(t, 160, L"Grabando: pulsa cada traste, rasguea, mueve la palanca e inclina la guitarra (%u s)", (g.diagUntil - now + 999) / 1000);
    SetWindowTextW(Item(ID_SAVED), t); g.savedT = 0;
    if (now >= g.diagUntil)
    {
        fclose(g.diag); g.diag = nullptr;
        SetWindowTextW(Item(ID_SAVED), L"Informe guardado en mandos.txt"); g.savedT = GetTickCount();
        ShellExecuteW(g.wnd, L"open", W(ExeDir() + "\\mandos.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
bool Collect(bool requireIso);
// ---- partidas (carpeta de datos: gh6.ini datos=, por defecto userdata) ----
namespace fsx = std::filesystem;
fsx::path DataDir()
{
    std::string d = g.ini.Get("datos", "userdata");
    fsx::path p = fsx::path(W(d));
    return p.is_absolute() ? p : fsx::path(W(ExeDir())) / p;
}
// Sesion 25: logros_lista.txt (id|puntos|fecha o 0|nombre|descripcion|como conseguirlo, UTF-8), escrita por el juego
void LoadAchievements()
{
    g.achs.clear();
    if (FILE* f = _wfopen((DataDir() / L"logros_lista.txt").c_str(), L"rb"))
    {
        char line[2048];
        while (fgets(line, sizeof(line), f))
        {
            std::vector<std::string> c; std::string cur;
            for (char* q = line; *q && *q != '\r' && *q != '\n'; q++) { if (*q == '|') { c.push_back(cur); cur.clear(); } else cur += *q; }
            c.push_back(cur);
            if (c.size() < 6) continue;
            auto u8 = [](const std::string& x) { std::wstring w(x.size() + 1, L' '); int n = MultiByteToWideChar(CP_UTF8, 0, x.c_str(), (int)x.size(), &w[0], (int)w.size()); w.resize(n > 0 ? n : 0); return w; };
            AchRow a; a.gs = atoi(c[1].c_str()); a.unlocked = c[2] != "0"; a.date = u8(a.unlocked ? c[2] : ""); a.name = u8(c[3]); a.desc = u8(c[4]); a.howto = u8(c[5]);
            g.achs.push_back(a);
        }
        fclose(f);
    }
    std::stable_sort(g.achs.begin(), g.achs.end(), [](const AchRow& x, const AchRow& y) { return x.unlocked > y.unlocked; });
    int n = 0, pts = 0, tot = 0; for (auto& a : g.achs) { tot += a.gs; if (a.unlocked) { n++; pts += a.gs; } }
    wchar_t info[160];
    if (g.achs.empty()) swprintf(info, 160, L"Juega una vez para ver la lista de logros.");
    else swprintf(info, 160, L"%d de %zu desbloqueados  ·  %d de %d puntos", n, g.achs.size(), pts, tot);
    SetWindowTextW(Item(ID_ACHINFO), info);
    HWND l = Item(ID_ACHLIST);
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g.achs.size(); i++) SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)i);
}
// Las partidas son el contenido de tipo 00000001 (savegame); el DLC importado (00000002, 00009000) no se toca
std::vector<fsx::path> SaveEntries()
{
    std::vector<fsx::path> v; std::error_code ec;
    for (auto& e : fsx::directory_iterator(DataDir() / L"content", ec))
        if (_wcsnicmp(e.path().filename().c_str(), L"00000001_", 9) == 0) v.push_back(e.path());
    return v;
}
std::vector<fsx::path> Backups()
{
    std::vector<fsx::path> v; std::error_code ec;
    for (auto& e : fsx::directory_iterator(DataDir() / L"respaldos", ec)) if (e.is_directory()) v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v;
}
void UpdateSaveInfo()
{
    auto s = SaveEntries(); auto b = Backups();
    wchar_t t[200];
    swprintf(t, 200, L"%s  ·  %d respaldo%s  ·  carpeta %s", s.empty() ? L"Sin partida guardada" : L"Hay una partida guardada",
             (int)b.size(), b.size() == 1 ? L"" : L"s", DataDir().filename().c_str());
    SetWindowTextW(Item(ID_SAVEINFO), t);
    EnableWindow(Item(ID_SAVERESET), !s.empty());
    EnableWindow(Item(ID_SAVERESTORE), !b.empty());
}
// Mueve la partida actual a respaldos\partida_<fecha>; devuelve la carpeta o vacio
fsx::path BackupCurrent()
{
    auto s = SaveEntries(); if (s.empty()) return {};
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64]; swprintf(name, 64, L"partida_%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fsx::path dst = DataDir() / L"respaldos" / name / L"content"; std::error_code ec;
    fsx::create_directories(dst, ec);
    for (auto& e : s) fsx::rename(e, dst / e.filename(), ec);
    return dst.parent_path();
}
void ResetSave()
{
    if (MessageBoxW(g.wnd, L"¿Empezar el juego de cero?\n\nTu partida actual no se borra: se guarda en la carpeta \"respaldos\" y puedes recuperarla con \"Restaurar respaldo\".",
                    L"Empezar de cero", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    fsx::path b = BackupCurrent();
    wchar_t t[400]; swprintf(t, 400, L"Listo: el juego empezará de cero.\nTu partida quedó en:\n%s", b.c_str());
    MessageBoxW(g.wnd, t, L"Empezar de cero", MB_ICONINFORMATION);
    UpdateSaveInfo();
}
void RestoreSave()
{
    auto b = Backups(); if (b.empty()) return;
    fsx::path last = b.back();
    wchar_t t[400]; swprintf(t, 400, L"¿Restaurar el respaldo \"%s\"?\n\nSi hay una partida actual, antes se guarda como otro respaldo.", last.filename().c_str());
    if (MessageBoxW(g.wnd, t, L"Restaurar respaldo", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    BackupCurrent();
    std::error_code ec; fsx::create_directories(DataDir() / L"content", ec);
    for (auto& e : fsx::directory_iterator(last / L"content", ec)) fsx::rename(e.path(), DataDir() / L"content" / e.path().filename(), ec);
    fsx::remove_all(last, ec);
    MessageBoxW(g.wnd, L"Partida restaurada.", L"Restaurar respaldo", MB_ICONINFORMATION);
    UpdateSaveInfo();
}
// ---- actualizaciones (updater.cpp en un hilo; los resultados llegan por mensajes) ----
enum { WM_UPD_CHECKED = WM_APP + 1, WM_UPD_PROGRESS, WM_UPD_INSTALLED };
std::string Repo() { return g.ini.Get("actualizar_repo", "Piscacho/Guitar-Hero-Warriors-of-Rock-PC"); }
void SetUpdInfo(const std::wstring& t) { SetWindowTextW(Item(ID_UPDINFO), t.c_str()); SetWindowTextW(Item(ID_HOME_UPD), t.c_str()); }
void StartUpdateCheck(bool automatic)
{
    if (g.updBusy) return;
    g.updBusy = true; g.updAuto = automatic;
    if (!automatic) { SetUpdInfo(L"Buscando actualizaciones..."); EnableWindow(Item(ID_UPDCHECK), FALSE); }
    HWND wnd = g.wnd; std::string repo = Repo();
    std::thread([wnd, repo] { auto* u = new UpdateInfo(UpdateCheck(repo)); if (!PostMessageW(wnd, WM_UPD_CHECKED, 0, (LPARAM)u)) delete u; }).detach();
}
void StartUpdateInstall()
{
    if (g.updBusy || !g.upd.newer) return;
    g.updBusy = true;
    for (int id : { ID_UPDCHECK, ID_PLAY, ID_SAVE, ID_EXIT }) EnableWindow(Item(id), FALSE);
    SetUpdInfo(L"Descargando...");
    HWND wnd = g.wnd; UpdateInfo u = g.upd;
    std::vector<std::wstring> keep = { DataDir().filename().wstring() };
    std::thread([wnd, u, keep] {
        std::string err;
        bool ok = UpdateInstall(u, keep, [wnd](int pct, int phase) { PostMessageW(wnd, WM_UPD_PROGRESS, pct, phase); }, err);
        PostMessageW(wnd, WM_UPD_INSTALLED, ok, (LPARAM)new std::string(err));
    }).detach();
}
void OnUpdateChecked(UpdateInfo* u)
{
    g.updBusy = false; g.upd = *u; delete u;
    EnableWindow(Item(ID_UPDCHECK), TRUE);
    std::wstring cur = W(AppVersion());
    if (!g.upd.ok)
    {
        if (!g.updAuto) SetUpdInfo(L"No se pudo comprobar: " + W(g.upd.error) + L"  (repositorio " + W(Repo()) + L")");
        return;
    }
    if (!g.upd.newer)
    {
        if (!g.updAuto) SetUpdInfo(L"Tienes la última versión (v" + cur + L").");
        return;
    }
    std::wstring tag = W(g.upd.tag);
    SetUpdInfo(L"Nueva versión " + tag + L" disponible (tienes v" + cur + L").");
    SetWindowTextW(Item(ID_UPDCHECK), (L"Instalar " + tag).c_str());
    InvalidateRect(Item(ID_UPDCHECK), nullptr, TRUE);
    SetWindowTextW(Item(ID_SAVED), (L"Hay una versión nueva: " + tag + L" (pestaña Juego)").c_str()); g.savedT = 0;
}
void OnUpdateInstalled(bool ok, std::string* err)
{
    g.updBusy = false;
    for (int id : { ID_UPDCHECK, ID_PLAY, ID_SAVE, ID_EXIT }) EnableWindow(Item(id), TRUE);
    if (!ok) { SetUpdInfo(L"No se pudo actualizar: " + W(*err)); delete err; return; }
    delete err;
    Collect(false); g.ini.Save(ExeDir() + "\\gh6.ini");   // no perder lo cambiado en la ventana
    bool silent = getenv("GH6_UPDATE_AUTOYES") != nullptr;
    if (!silent) MessageBoxW(g.wnd, (L"Actualizado a " + W(g.upd.tag) + L". El launcher se abrirá de nuevo.").c_str(), L"Actualización", MB_ICONINFORMATION);
    if (UpdaterRelaunch()) DestroyWindow(g.wnd);
    else SetUpdInfo(L"Actualizado. Cierra y vuelve a abrir el juego.");
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintMain(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: DrawItem((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT*)l)->itemHeight = ((MEASUREITEMSTRUCT*)l)->CtlID == ID_ACHLIST ? S(48) : S(28); return TRUE;
    case WM_CTLCOLORSTATIC:
    {   // sesion 28: textos sobre el panel de contenido (o sobre la barra inferior el estado)
        HDC dc = (HDC)w; HWND c = (HWND)l;
        bool bar = c == Item(ID_SAVED), card = c == Item(ID_HOME_ISO) || c == Item(ID_HOME_UPD) || c == Item(ID_HOME_KEYS) || c == Item(ID_NET_CODE);   // (campo de solo lectura)
        COLORREF col = g.dimLabels.count(c) ? kDim : kText;
        if (bar || (c == Item(ID_HOME_ISO) && g.isoMissing)) col = kAccent2;
        SetTextColor(dc, col); SetBkColor(dc, bar ? kBar : card ? kInput : kPanel);
        return (LRESULT)(bar ? g.brBar : card ? g.brInput : g.brPanel);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    { HDC dc = (HDC)w; SetTextColor(dc, kText); SetBkColor(dc, kInput); return (LRESULT)g.brInput; }
    case WM_CTLCOLORBTN: return (LRESULT)g.brPanel;
    case WM_HSCROLL: if ((HWND)l == Item(ID_MIC_GAIN)) UpdateGainLabel(); return 0;
    case WM_UPD_CHECKED: OnUpdateChecked((UpdateInfo*)l); return 0;
    case WM_UPD_PROGRESS:
    {
        static const wchar_t* ph[] = { L"Descargando", L"Descomprimiendo", L"Instalando" };
        wchar_t t[64]; if (l == 0) swprintf(t, 64, L"%s... %d %%", ph[0], (int)w); else swprintf(t, 64, L"%s...", ph[std::min<int>((int)l, 2)]);
        SetUpdInfo(t); return 0;
    }
    case WM_UPD_INSTALLED: OnUpdateInstalled(w != 0, (std::string*)l); return 0;
    case WM_TIMER:
    {
        g.ticks++;
        PollPads();
        DiagTick();
        if (g.tab == 2) { TickCapture(); InvalidateRect(Item(ID_TESTPANEL), nullptr, FALSE); if (g.ticks % 15 == 0) UpdateStatus(); }
        if (g.tab == 3)
        {
            if (!g.meterOn && Toggled(ID_MIC_ON)) StartMeter();
            float raw = g.meter.peak.exchange(0) * Gain(), v = std::min(1.0f, raw);
            g.meterShown = std::max(v, g.meterShown * 0.85f);
            if (v >= g.meterHold) { g.meterHold = v; g.holdT = GetTickCount(); }
            else if (GetTickCount() - g.holdT > 1200) g.meterHold = std::max(0.0f, g.meterHold - 0.02f);
            InvalidateRect(Item(ID_MIC_METER), nullptr, FALSE);
        }
        if (g.savedT && GetTickCount() - g.savedT > 2500) { g.savedT = 0; SetWindowTextW(Item(ID_SAVED), L""); }
        if (g.tab == 6 && g.ticks % 10 == 0)
        {   // codigo de sala y estado de la conexion (net.cpp)
            static std::wstring lastS; static std::string lastC;
            std::string code; std::wstring st;
            if (!Toggled(ID_NET_ON)) { code.clear(); st = L"Activa el juego en línea para ver tu código."; }
            else NetLauncherInfo(code, st);
            if (code != lastC) { lastC = code; SetWindowTextW(Item(ID_NET_CODE), W(code).c_str()); }
            if (st != lastS) { lastS = st; SetWindowTextW(Item(ID_NET_STATUS), st.c_str()); }
        }
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(w), code = HIWORD(w); HWND c = (HWND)l;
        if (code == BN_CLICKED && g.toggles.count(c))
        {
            g.toggles[c] = !g.toggles[c]; InvalidateRect(c, nullptr, TRUE);
            if (id == ID_MIC_ON) StartMeter();
            // Inicio repite pantalla completa y juego en linea: los dos interruptores van a la par
            static const int pairs[][2] = { { ID_Q_FULL, ID_FULLSCREEN }, { ID_Q_NET, ID_NET_ON } };
            for (auto& pr : pairs) for (int k = 0; k < 2; k++) if (id == pr[k]) { HWND o = Item(pr[1 - k]); g.toggles[o] = g.toggles[c]; InvalidateRect(o, nullptr, TRUE); }
            if ((id == ID_NET_ON || id == ID_Q_NET) && g.toggles[c]) NetLauncherStart();
            return 0;
        }
        if (id >= ID_TAB0 && id < ID_TAB0 + kTabs && code == BN_CLICKED) { ShowTab(id - ID_TAB0); return 0; }
        switch (id)
        {
        case ID_PLAY: if (Collect(true)) { g.ini.Save(ExeDir() + "\\gh6.ini"); g.result = true; DestroyWindow(h); } return 0;
        case ID_SAVE: if (Collect(false)) { g.ini.Save(ExeDir() + "\\gh6.ini"); SetWindowTextW(Item(ID_SAVED), L"Configuración guardada"); g.savedT = GetTickCount(); } return 0;
        case ID_EXIT: DestroyWindow(h); return 0;
        case ID_ISO_BROWSE: BrowseIso(); return 0;
        case ID_NET_COPY:
        {   // copiar el codigo de sala al portapapeles
            std::wstring t = GetText(Item(ID_NET_CODE));
            if (!t.empty() && OpenClipboard(h))
            {
                EmptyClipboard();
                HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
                if (m) { memcpy(GlobalLock(m), t.c_str(), (t.size() + 1) * sizeof(wchar_t)); GlobalUnlock(m); SetClipboardData(CF_UNICODETEXT, m); }
                CloseClipboard();
                SetWindowTextW(Item(ID_SAVED), L"Código copiado"); g.savedT = GetTickCount();
            }
            return 0;
        }
        case ID_DIAG:
            if (MessageBoxW(h, L"Se grabarán 15 segundos de lo que pulses en los mandos conectados (guitarra, batería...).\n\n"
                               L"Al aceptar: pulsa cada traste uno a uno, rasguea arriba y abajo, mueve la palanca, inclina la guitarra "
                               L"y pulsa Start y Back. Al terminar se abre mandos.txt: envíalo para configurar ese mando.",
                            L"Informe de mandos", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) DiagStart();
            return 0;
        case ID_SAVEOPEN: { std::error_code ec; fsx::create_directories(DataDir(), ec); ShellExecuteW(h, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); } return 0;
        case ID_SAVERESET: ResetSave(); return 0;
        case ID_SAVERESTORE: RestoreSave(); return 0;
        case ID_UPDCHECK:
            if (g.upd.newer)
            {
                std::wstring q = L"¿Descargar e instalar " + W(g.upd.tag) + L"?\n\nSe conservan gh6.ini, las partidas y el DLC.";
                if (!g.upd.notes.empty()) q += L"\n\nNovedades:\n" + W(g.upd.notes.substr(0, 600));
                if (getenv("GH6_UPDATE_AUTOYES") || MessageBoxW(h, q.c_str(), L"Actualización", MB_YESNO | MB_ICONQUESTION) == IDYES) StartUpdateInstall();
            }
            else StartUpdateCheck(false);
            return 0;
        case ID_DLC_BROWSE: BrowseFolder(ID_DLC); UpdateDlcCount(); return 0;
        case ID_DLC: if (code == EN_CHANGE) UpdateDlcCount(); return 0;
        case ID_MAPSET: if (code == CBN_SELCHANGE) { g.capturing = false; g.mapSet = Sel(ID_MAPSET); FillMapList(); } return 0;
        case ID_MAPPLAYER:
            if (code == CBN_SELCHANGE)
            {
                g.capturing = false; g.mapPlayer = Sel(ID_MAPPLAYER);
                if (g.mapPlayer > 0) SendMessageW(Item(ID_TESTPLAYER), CB_SETCURSEL, g.mapPlayer - 1, 0);   // probar a ese jugador
                InvalidateRect(Item(ID_MAPLIST), nullptr, TRUE);
            }
            return 0;
        case ID_ASSIGN: StartCapture(false); return 0;
        case ID_ADD: StartCapture(true); return 0;
        case ID_MAPLIST: if (code == LBN_DBLCLK) StartCapture(false); return 0;
        case ID_RESET:
        {   // Preajustes (sesion 24): se aplican a todos los jugadores o solo al elegido en "Solo jugador N"
            HMENU m = CreatePopupMenu();
            if (g.mapSet == 0)
            {
                AppendMenuW(m, MF_STRING, 1, L"Mando de Xbox: clásico (A verde, B rojo, Y amarillo, X azul, LB naranja)");
                AppendMenuW(m, MF_STRING, 2, L"Mando de Xbox: gatillos (LT, LB, RB, RT, A; palanca con el stick derecho)");
                AppendMenuW(m, MF_STRING, 5, L"Guitarra CRKD (Les Paul) en PC: palanca LT, inclinación RT");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(m, MF_STRING, 3, L"Valores por defecto");
            if (g.mapPlayer > 0) AppendMenuW(m, MF_STRING, 4, L"Quitar los botones propios de este jugador");
            RECT br; GetWindowRect(Item(ID_RESET), &br);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, br.right, br.bottom, 0, h, nullptr);
            DestroyMenu(m);
            if (!cmd) return 0;
            // Valores de cada funcion de la guitarra en el orden de g_guitar (verde..inclinacion)
            static const char* kTriggers[kGuitarN] = { "LT", "LB", "RB", "RT", "A", "UP|LS_UP", "DOWN|LS_DOWN", "LEFT|LS_LEFT", "RIGHT|LS_RIGHT",
                                                       "START", "BACK|Y|X", "RS", "" };
            // Guitarra CRKD Gibson Les Paul (informe de mandos de un usuario, 2026-11-01): llega como mando XInput (subtipo 1) con
            // los trastes de la guitarra de 360 (A B Y X LB), rasgueo en la cruceta, palanca en LT (0-255) e inclinacion en RT (255).
            // Su sensor de movimiento mueve los dos sticks al inclinarla: se quitan del rasgueo para que no rasguee sola.
            static const char* kCrkd[kGuitarN] = { "A", "B", "Y", "X", "LB", "UP", "DOWN", "LEFT", "RIGHT", "START", "BACK|RB", "LT", "RT" };
            for (int i = 0; i < CurMapN(); i++)
            {
                MapFn& f = CurMap()[i];
                std::string v = cmd == 2 ? kTriggers[i] : cmd == 5 ? kCrkd[i] : f.def;
                if (cmd == 4) f.ovr[g.mapPlayer - 1].clear();
                else if (g.mapPlayer > 0) f.ovr[g.mapPlayer - 1] = v;
                else f.val = v;
            }
            g.capturing = false; InvalidateRect(Item(ID_MAPLIST), nullptr, TRUE); InvalidateRect(Item(ID_TESTPANEL), nullptr, FALSE);
            return 0;
        }
        case ID_MIC_DEV: if (code == CBN_SELCHANGE) StartMeter(); return 0;
        case ID_Q_RES: case ID_RES: case ID_Q_PANO: case ID_PANO:
            if (code == CBN_SELCHANGE)
            {   // Inicio repite resolucion interna y proporcion
                int other = id == ID_Q_RES ? ID_RES : id == ID_RES ? ID_Q_RES : id == ID_Q_PANO ? ID_PANO : ID_Q_PANO;
                SendMessageW(Item(other), CB_SETCURSEL, Sel(id), 0);
            }
            return 0;
        case ID_ISO: if (code == EN_CHANGE) UpdateHome(); return 0;
        case ID_KBD: case ID_PS: case ID_PAD0: case ID_PAD0 + 1: case ID_PAD0 + 2: case ID_PAD0 + 3: if (code == CBN_SELCHANGE) UpdateStatus(); return 0;
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g.done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

// ¿Abrir el launcher? --jugar lo salta, --launcher lo fuerza, Mayus pulsada al abrir lo fuerza; si no, mostrar_launcher del ini (1 por defecto)
bool LauncherWanted(int argc, char** argv)
{
    if (getenv("GH6_NOLAUNCHER")) return false;
    for (int i = 1; i < argc; i++) { if (!_stricmp(argv[i], "--jugar")) return false; if (!_stricmp(argv[i], "--launcher")) return true; }
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return true;
    IniDoc ini; ini.Load(ExeDir() + "\\gh6.ini");
    return ini.Get("mostrar_launcher", "1") != "0";
}
// Muestra el launcher. true = JUGAR (gh6.ini ya guardado), false = salir
bool RunLauncherUI()
{
    DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    g.inst = GetModuleHandleW(nullptr);
    g.dpi = (int)GetDpiForSystem(); if (g.dpi < 96) g.dpi = 96;
    g.ini.Load(ExeDir() + "\\gh6.ini");
    auto loadMap = [](MapFn* m, int n) {
        for (int i = 0; i < n; i++)
        {
            m[i].val = g.ini.Get(m[i].key, m[i].def);
            for (int p = 0; p < 4; p++) m[i].ovr[p] = g.ini.Get("jugador" + std::to_string(p + 1) + "_" + m[i].key, "");
        }
    };
    loadMap(g_guitar, kGuitarN); loadMap(g_drum, kDrumN);
    auto font = [](int pt, int weight, const wchar_t* face) { return CreateFontW(-S(pt), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); };
    const wchar_t* face = L"Bahnschrift";   // sesion 28: como el launcher de Dead Space
    g.font = font(14, FW_NORMAL, face); g.fontBold = font(14, FW_SEMIBOLD, face); g.fontSmall = font(12, FW_NORMAL, face);
    g.fontTitle = font(31, FW_BOLD, face); g.fontSub = font(12, FW_SEMIBOLD, face); g.fontHead = font(15, FW_SEMIBOLD, face);
    g.fontTab = font(14, FW_SEMIBOLD, face); g.fontPlay = font(21, FW_BOLD, face); g.fontBanner = font(62, FW_BOLD, face);
    g.fontIcon = font(18, FW_NORMAL, L"Segoe MDL2 Assets");
    g.brBg = CreateSolidBrush(kBg); g.brInput = CreateSolidBrush(kInput); g.brSide = CreateSolidBrush(kSide);
    g.brPanel = CreateSolidBrush(kPanel); g.brBar = CreateSolidBrush(kBar);
    {   // selector < valor >
        WNDCLASSW sc{}; sc.lpfnWndProc = SelProc; sc.hInstance = g.inst; sc.lpszClassName = L"GHSel"; sc.hCursor = LoadCursor(nullptr, IDC_HAND); sc.style = CS_DBLCLKS;
        RegisterClassW(&sc);
    }
    for (const char* ext : { "png", "jpg", "jpeg", "bmp" })
    {   // portada.<ext> junto al exe (opcional): imagen de la portada de Inicio (por ejemplo un "hero" de SteamGridDB)
        if (g.banner) break;
        FILE* f = fopen((ExeDir() + "\\portada." + ext).c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> d; uint8_t buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && d.size() < (64u << 20)) d.insert(d.end(), buf, buf + n);
        fclose(f);
        g.banner = DecodeImage(d, g.bannerW, g.bannerH);
    }
    if (const char* pg = getenv("GH6_LAUNCHER_PAGE")) g.tab = std::clamp(atoi(pg), 0, kTabs - 1);   // capturas de cada pagina
    LoadIsoLogo(W(g.ini.Get("iso")));   // logo del juego desde el ISO del usuario (sesion 28)
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = g.inst; wc.lpszClassName = L"GH6Launcher"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1)); wc.hbrBackground = g.brBg;
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{ 0, 0, S(kW), S(kH) }; AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(0, wc.lpszClassName, L"Guitar Hero: Warriors of Rock - Launcher", style,
                            (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, g.inst, nullptr);
    BOOL dark = TRUE; DwmSetWindowAttribute(g.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = kSide, border = kLine;
    DwmSetWindowAttribute(g.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g.wnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    BuildUi();
    UpdateSaveInfo();
    { wchar_t t[160]; swprintf(t, 160, L"Versión instalada: v%hs  ·  repositorio %hs", AppVersion(), Repo().c_str()); SetUpdInfo(t); }
    ShowTab(g.tab);
    if (Toggled(ID_UPDAUTO) || getenv("GH6_UPDATE_API")) StartUpdateCheck(true);
    ShowWindow(g.wnd, SW_SHOW); UpdateWindow(g.wnd);
    SetTimer(g.wnd, 1, 33, nullptr);
    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g.wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.meter.Stop();
    if (g.logo) DeleteObject(g.logo);
    for (HBITMAP b : g.cutBmp) if (b) DeleteObject(b);
    for (HFONT f : { g.font, g.fontBold, g.fontSmall, g.fontTitle, g.fontSub, g.fontTab, g.fontIcon, g.fontHead, g.fontPlay, g.fontBanner }) DeleteObject(f);
    UnregisterClassW(wc.lpszClassName, g.inst); UnregisterClassW(L"GHSel", g.inst);
    DeleteObject(g.brBg); DeleteObject(g.brInput); DeleteObject(g.brSide); DeleteObject(g.brPanel); DeleteObject(g.brBar);
    if (g.banner) DeleteObject(g.banner);
    CoUninitialize();
    SetThreadDpiAwarenessContext(oldDpi);
    LOG("[launcher] ventana cerrada: %s", g.result ? "JUGAR" : "salir");
    return g.result;
}

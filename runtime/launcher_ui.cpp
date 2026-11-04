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
const COLORREF kBg = RGB(20, 16, 22), kSide = RGB(13, 10, 15), kSel = RGB(38, 30, 42), kInput = RGB(40, 34, 46), kText = RGB(237, 230, 221),
               kDim = RGB(150, 140, 148), kAccent = RGB(232, 85, 30), kAccent2 = RGB(255, 176, 64), kLine = RGB(64, 54, 68);
enum
{
    ID_TAB0 = 100, ID_PLAY = 200, ID_SAVE, ID_EXIT, ID_SHOWLAUNCHER, ID_SAVED,
    ID_ISO = 300, ID_ISO_BROWSE, ID_DLC, ID_DLC_BROWSE, ID_DLCCOUNT, ID_SKIPINTRO, ID_SAVEINFO, ID_SAVEOPEN, ID_SAVERESET, ID_SAVERESTORE,
    ID_UPDINFO, ID_UPDCHECK, ID_UPDAUTO,
    ID_FULLSCREEN = 400, ID_WINSIZE, ID_SCALE, ID_VSYNC, ID_MBLUR, ID_GRAIN, ID_AUDIOLAT, ID_AA, ID_ANISO, ID_SHARP, ID_FPS, ID_RES, ID_PANO,
    ID_INST0 = 500, ID_PAD0 = 510, ID_STATUS0 = 520, ID_KBD = 530, ID_PS, ID_PADMODE, ID_DIAG, ID_MAPSET, ID_MAPPLAYER, ID_ASSIGN, ID_ADD, ID_RESET, ID_MAPLIST, ID_TESTPLAYER, ID_TESTPANEL,
    ID_MIC_ON = 600, ID_MIC_DEV, ID_MIC_GAIN, ID_MIC_GAINLBL, ID_MIC_METER,
    ID_ACHINFO = 700, ID_ACHLIST,
};
constexpr int kTabs = 6;   // sesion 25: + Logros (pagina 5, dibujada encima de Ayuda en la barra lateral)
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
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSub = nullptr, fontTab = nullptr, fontIcon = nullptr, fontSmall = nullptr;
    HBRUSH brBg = nullptr, brInput = nullptr, brSide = nullptr;
    int tab = 0; std::vector<HWND> pages[kTabs];
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
HWND Toggle(int page, const wchar_t* t, int x, int y, int w, int id, bool on)
{
    HWND c = Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 26, id);
    g.toggles[c] = on; return c;
}
HWND Combo(int page, int x, int y, int w, int id, const std::vector<std::wstring>& items, int sel)
{
    HWND c = Make(page, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
    SetWindowTheme(c, L"DarkMode_CFD", nullptr);
    for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(c, CB_SETCURSEL, std::clamp(sel, 0, (int)items.size() - 1), 0);
    return c;
}
HWND Button(int page, const wchar_t* t, int x, int y, int w, int h, int id) { return Make(page, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id); }
HWND Edit(int page, const std::wstring& t, int x, int y, int w, int id)
{
    HWND c = Make(page, L"EDIT", t.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, x, y, w, 26, id);
    SetWindowTheme(c, L"DarkMode_CFD", nullptr);
    return c;
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
void Circle(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, std::max(1, S(2)), border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
COLORREF Dim(COLORREF c, float k) { return RGB((int)(GetRValue(c) * k), (int)(GetGValue(c) * k), (int)(GetBValue(c) * k)); }
void Text(HDC dc, const wchar_t* t, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE)
{
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c); HGDIOBJ of = SelectObject(dc, f);
    DrawTextW(dc, t, -1, &r, fmt | DT_NOPREFIX); SelectObject(dc, of);
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
    RoundR(dc, rc, RGB(26, 21, 30), kLine, S(10));
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
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
    int r = S(16), x0 = rc.left + S(28), y0 = rc.top + S(30), dx = S(42);
    if (g.mapSet == 0)
    {
        for (int i = 0; i < 5; i++) { bool o = on(i); Circle(dc, x0 + i * dx, y0, r, o ? fret[i] : Dim(fret[i], 0.25f), o ? RGB(255, 255, 255) : Dim(fret[i], 0.5f)); }
        int ax = rc.left + S(250);
        Triangle(dc, ax, y0 - S(10), S(10), true, on(5) ? kAccent2 : kLine);
        Triangle(dc, ax, y0 + S(12), S(10), false, on(6) ? kAccent2 : kLine);
        uint32_t wm = InputParseSources(Eff(m[11], p).c_str()); int v = 0;
        if (wm & kRT) v = std::max(v, (int)rt);
        if (wm & kLT) v = std::max(v, (int)lt);
        if (wm & (0x0080 | kRSDirs)) v = std::max(v, std::min(255, std::max(std::abs((int)st[2]), std::abs((int)st[3])) >> 7));
        RECT bar{ rc.left + S(12), rc.top + S(64), rc.left + S(190), rc.top + S(74) };
        RoundR(dc, bar, RGB(40, 34, 46), kLine, S(6));
        RECT fill = bar; fill.right = fill.left + (bar.right - bar.left) * v / 255;
        if (v > 4) RoundR(dc, fill, kAccent, kAccent, S(6));
        RECT lr{ bar.right + S(8), bar.top - S(6), rc.right - S(8), bar.bottom + S(6) };
        Text(dc, L"Palanca", lr, kDim, g.fontSmall);
        RECT sr{ rc.left + S(12), rc.top + S(80), rc.left + S(160), rc.bottom - S(4) };
        if (any) Text(dc, L"★ Poder estrella", sr, on(10) ? kAccent2 : kLine, g.fontSmall);
        RECT tr{ rc.left + S(160), rc.top + S(80), rc.right - S(10), rc.bottom - S(4) };
        Text(dc, L"START", tr, on(9) ? kAccent2 : kLine, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    else
    {
        const int order[5] = { 1, 2, 3, 4, 0 };   // rojo, amarillo, azul, naranja, verde (como la bateria de 5 piezas)
        for (int k = 0; k < 5; k++) { int i = order[k]; bool o = on(i); Circle(dc, x0 + k * dx, y0, r, o ? fret[i] : Dim(fret[i], 0.25f), o ? RGB(255, 255, 255) : Dim(fret[i], 0.5f)); }
        RECT kick{ rc.left + S(12), rc.top + S(60), rc.left + S(220), rc.top + S(72) };
        RoundR(dc, kick, on(5) ? kAccent : RGB(40, 34, 46), kLine, S(6));
        RECT kr{ kick.right + S(8), kick.top - S(6), rc.right - S(8), kick.bottom + S(6) };
        Text(dc, L"Bombo", kr, on(5) ? kAccent2 : kDim, g.fontSmall);
        RECT tr{ rc.left + S(12), rc.top + S(80), rc.right - S(10), rc.bottom - S(4) };
        Text(dc, L"START", tr, on(8) ? kAccent2 : kLine, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (!any)
    {
        RECT nr{ rc.left + S(12), rc.top + S(80), rc.left + S(220), rc.bottom - S(4) };
        Text(dc, L"Conecta un mando a este jugador", nr, kDim, g.fontSmall);
    }
}
void DrawMeter(HDC dc, RECT rc)
{
    RoundR(dc, rc, RGB(26, 21, 30), kLine, S(8));
    RECT in = rc; InflateRect(&in, -S(4), -S(4));
    float v = g.meterShown;
    int wfull = in.right - in.left, wv = (int)(wfull * std::min(1.0f, v));
    for (int x = 0; x < wv; x += S(6))
    {
        float f = (float)x / wfull;
        COLORREF c = f < 0.6f ? RGB(60, 200, 90) : f < 0.85f ? RGB(240, 200, 50) : RGB(235, 60, 50);
        RECT seg{ in.left + x, in.top, std::min(in.left + x + S(4), in.right), in.bottom }; FillR(dc, seg, c);
    }
    if (g.meterHold > 0.01f) { int hx = in.left + (int)(wfull * std::min(1.0f, g.meterHold)); RECT hr{ hx - S(1), in.top, hx + S(1), in.bottom }; FillR(dc, hr, kText); }
    if (!g.meterOn)
    {
        Text(dc, Toggled(ID_MIC_ON) ? L"Abriendo el micrófono..." : L"Micrófono desactivado", in, kDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (!g.meter.ok) Text(dc, L"No se pudo abrir este micrófono", in, kDim, g.fontSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void DrawItem(DRAWITEMSTRUCT* d)
{
    HDC dc = d->hDC; RECT rc = d->rcItem;
    if (d->CtlType == ODT_STATIC)
    {
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, w, h); HGDIOBJ ob = SelectObject(mem, bm);
        RECT lr{ 0, 0, w, h }; FillR(mem, lr, kBg);
        if (d->CtlID == ID_TESTPANEL) DrawTestPanel(mem, lr); else DrawMeter(mem, lr);
        BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        return;
    }
    if (d->CtlType == ODT_LISTBOX && d->CtlID == ID_ACHLIST)
    {
        if ((int)d->itemID < 0 || d->itemID >= g.achs.size()) return;
        const AchRow& a = g.achs[d->itemID];
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        FillR(dc, rc, sel ? kSel : kInput);
        RECT sep{ rc.left, rc.bottom - 1, rc.right, rc.bottom }; FillR(dc, sep, kBg);
        RECT ic{ rc.left + S(6), rc.top, rc.left + S(40), rc.bottom };
        Text(dc, L"\uE734", ic, a.unlocked ? RGB(255, 200, 60) : kLine, g.fontIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT t1{ rc.left + S(44), rc.top + S(3), rc.right - S(150), rc.top + S(23) };
        Text(dc, a.name.c_str(), t1, a.unlocked ? kText : kDim, a.unlocked ? g.fontBold : g.font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t2{ rc.left + S(44), rc.top + S(23), rc.right - S(10), rc.bottom - S(3) };
        Text(dc, (a.unlocked ? a.desc : a.howto.empty() ? a.desc : a.howto).c_str(), t2, kDim, g.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t3{ rc.right - S(150), rc.top + S(3), rc.right - S(10), rc.top + S(23) };
        std::wstring right = std::to_wstring(a.gs) + L" G" + (a.unlocked ? L"  ·  " + a.date.substr(0, 10) : L"");
        Text(dc, right.c_str(), t3, a.unlocked ? RGB(255, 200, 60) : kDim, g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (d->CtlType == ODT_LISTBOX)
    {
        if ((int)d->itemID < 0) return;
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        FillR(dc, rc, sel ? kSel : kInput);
        if (sel) { RECT bar{ rc.left, rc.top, rc.left + S(3), rc.bottom }; FillR(dc, bar, kAccent); }
        MapFn& f = CurMap()[d->itemID];
        RECT l{ rc.left + S(10), rc.top, rc.left + S(160), rc.bottom };
        Text(dc, f.label, l, kText, g.font);
        int mp = g.mapPlayer - 1; bool own = mp >= 0 && !f.ovr[mp].empty();
        std::wstring v = W(Eff(f, mp)); size_t pos; while ((pos = v.find(L'|')) != std::wstring::npos) v.replace(pos, 1, L"  /  ");
        if (v.empty()) v = L"(ninguno)";
        if (mp >= 0 && !own) v += L"   (común)";
        bool cap = g.capturing && g.capItem == (int)d->itemID;
        if (cap) v = L"pulsa un botón...";
        RECT r{ rc.left + S(160), rc.top, rc.right - S(8), rc.bottom };
        Text(dc, v.c_str(), r, cap ? kAccent2 : own ? kText : kDim, g.font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (d->CtlType != ODT_BUTTON) return;
    HWND h = d->hwndItem;
    wchar_t t[128]; GetWindowTextW(h, t, 128);
    bool pressed = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
    if (d->CtlID >= ID_TAB0 && d->CtlID < ID_TAB0 + kTabs)
    {
        int i = d->CtlID - ID_TAB0; bool sel = i == g.tab;
        FillR(dc, rc, sel ? kSel : kSide);
        if (sel) { RECT bar{ rc.left, rc.top + S(8), rc.left + S(4), rc.bottom - S(8) }; FillR(dc, bar, kAccent); }
        static const wchar_t* icons[kTabs] = { L"", L"", L"", L"", L"", L"" };
        RECT ir{ rc.left + S(18), rc.top, rc.left + S(46), rc.bottom };
        Text(dc, icons[i], ir, sel ? kAccent2 : kDim, g.fontIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT tr{ rc.left + S(52), rc.top, rc.right, rc.bottom };
        Text(dc, t, tr, sel ? kText : kDim, g.fontTab);
        return;
    }
    FillR(dc, rc, kBg);
    auto tg = g.toggles.find(h);
    if (tg != g.toggles.end())
    {
        bool on = tg->second;
        int pw = S(40), ph = S(20), py = rc.top + (rc.bottom - rc.top - ph) / 2;
        RECT pill{ rc.left, py, rc.left + pw, py + ph };
        RoundR(dc, pill, on ? kAccent : kLine, on ? kAccent : kLine, ph);
        int kr = ph / 2 - S(3), kx = on ? pill.right - ph / 2 : pill.left + ph / 2;
        Circle(dc, kx, py + ph / 2, kr, on ? RGB(255, 255, 255) : kDim, on ? RGB(255, 255, 255) : kDim);
        RECT tr{ rc.left + pw + S(12), rc.top, rc.right, rc.bottom };
        Text(dc, t, tr, kText, g.font);
        if (focus) { RECT fr = tr; fr.right = fr.left + S(4); }
        return;
    }
    bool play = d->CtlID == ID_PLAY;
    COLORREF fill = play ? (pressed ? RGB(190, 60, 20) : kAccent) : (pressed ? kSel : kInput);
    RoundR(dc, rc, fill, play ? kAccent2 : (focus ? kDim : kLine), S(8));
    Text(dc, t, rc, play ? RGB(255, 255, 255) : kText, play ? g.fontBold : g.font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void PaintMain(HDC dc)
{
    RECT rc; GetClientRect(g.wnd, &rc);
    FillR(dc, rc, kBg);
    int head = S(78), side = S(180), foot = rc.bottom - S(62);
    for (int y = 0; y < head; y++)   // degradado del encabezado (rojo fuego -> fondo)
    {
        float f = (float)y / head;
        COLORREF c = RGB((int)(70 * (1 - f) + 20 * f), (int)(18 * (1 - f) + 16 * f), (int)(14 * (1 - f) + 22 * f));
        RECT r{ 0, y, rc.right, y + 1 }; FillR(dc, r, c);
    }
    RECT line{ 0, head - S(2), rc.right, head }; FillR(dc, line, kAccent);
    RECT t1{ S(22), S(8), rc.right, S(50) };
    Text(dc, L"GUITAR HERO", { t1.left + S(2), t1.top + S(2), t1.right, t1.bottom + S(2) }, RGB(0, 0, 0), g.fontTitle);
    Text(dc, L"GUITAR HERO", t1, kAccent2, g.fontTitle);
    RECT t2{ S(24), S(46), rc.right, S(72) };
    Text(dc, L"WARRIORS OF ROCK  ·  versión para PC recompilada", t2, kText, g.fontSub);
    RECT t3{ 0, S(10), rc.right - S(20), S(40) };
    wchar_t ver[32]; swprintf(ver, 32, L"v%hs", AppVersion());
    Text(dc, ver, t3, kDim, g.fontSmall, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    RECT sb{ 0, head, side, foot }; FillR(dc, sb, kSide);
    RECT fl{ 0, foot, rc.right, foot + 1 }; FillR(dc, fl, kLine);
    
}
void LoadAchievements();
void ShowTab(int t)
{
    g.tab = t;
    for (int p = 0; p < kTabs; p++) for (HWND h : g.pages[p]) ShowWindow(h, p == t ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < kTabs; i++) InvalidateRect(Item(ID_TAB0 + i), nullptr, TRUE);
    if (t == 5) LoadAchievements();
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
void BuildUi()
{
    IniDoc& ini = g.ini;
    auto flag = [&](const char* k, bool def) { std::string v = ini.Get(k, def ? "1" : "0"); return v != "0"; };
    // Barra lateral y pie
    const wchar_t* tabs[kTabs] = { L"Juego", L"Imagen y audio", L"Controles", L"Micrófono", L"Ayuda", L"Logros" };
    for (int i = 0; i < kTabs; i++) { int pos = i == 5 ? 4 : i == 4 ? 5 : i; Button(-1, tabs[i], 0, 96 + pos * 52, 180, 48, ID_TAB0 + i); }
    // Logros (sesion 25): la lista la escribe el juego en la carpeta de partidas (logros_lista.txt)
    Label(5, L"Logros", 204, 98, 200, 22, 0, false, g.fontBold);
    Label(5, L"", 300, 98, 560, 22, ID_ACHINFO, true);
    Make(5, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 204, 128, 656, 384, ID_ACHLIST);
    Toggle(-1, L"Mostrar este launcher al iniciar", 22, 534, 300, ID_SHOWLAUNCHER, flag("mostrar_launcher", true));
    Label(-1, L"", 330, 538, 200, 22, ID_SAVED, true);
    Button(-1, L"Guardar", 540, 528, 100, 40, ID_SAVE);
    Button(-1, L"Salir", 650, 528, 90, 40, ID_EXIT);
    Button(-1, L"▶  JUGAR", 750, 528, 112, 40, ID_PLAY);
    // Juego
    Label(0, L"ISO del juego (Xbox 360)", 204, 98, 400, 22, 0, false, g.fontBold);
    Edit(0, W(ini.Get("iso")), 204, 124, 526, ID_ISO);
    Button(0, L"Examinar...", 740, 123, 120, 28, ID_ISO_BROWSE);
    Label(0, L"Carpeta de DLC", 204, 168, 400, 22, 0, false, g.fontBold);
    Edit(0, W(ini.Get("dlc", ExeDir() + "\\dlc")), 204, 194, 526, ID_DLC);
    Button(0, L"Examinar...", 740, 193, 120, 28, ID_DLC_BROWSE);
    Label(0, L"", 204, 226, 650, 22, ID_DLCCOUNT, true);
    Toggle(0, L"Saltar el vídeo de logos al arrancar", 204, 266, 420, ID_SKIPINTRO, flag("saltar_intro", true));
    Label(0, L"Partidas", 204, 306, 200, 22, 0, false, g.fontBold);
    Label(0, L"", 300, 306, 560, 22, ID_SAVEINFO, true);
    Button(0, L"Abrir carpeta", 204, 334, 140, 28, ID_SAVEOPEN);
    Button(0, L"Empezar de cero", 352, 334, 160, 28, ID_SAVERESET);
    Button(0, L"Restaurar respaldo", 520, 334, 170, 28, ID_SAVERESTORE);
    Label(0, L"Actualizaciones", 204, 390, 200, 22, 0, false, g.fontBold);
    Label(0, L"", 204, 416, 656, 22, ID_UPDINFO, true);
    Button(0, L"Buscar actualizaciones", 204, 444, 220, 30, ID_UPDCHECK);
    Toggle(0, L"Buscar al abrir el launcher", 440, 446, 300, ID_UPDAUTO, flag("actualizar_al_abrir", true));
    // Ayuda
    Label(4, L"Teclas durante el juego", 204, 98, 400, 22, 0, false, g.fontBold);
    Label(4, L"F11 o Alt+Enter: pantalla completa      F12: captura de pantalla (shot_9xx.bmp)\n"
             L"F6: cambia el instrumento del jugador del teclado (guitarra, batería, voz)\n"
             L"Teclado: flechas, Enter = Start, Retroceso = poder estrella, Espacio/1 = verde, Esc/2 = rojo,\n"
             L"3 = amarillo, 4 = azul, 5 = naranja (en batería 5 = platillo naranja, K o B = bombo), Q/E = gatillos",
          204, 124, 660, 92, 0, true);
    Label(4, L"Varios jugadores", 204, 230, 400, 22, 0, false, g.fontBold);
    Label(4, L"Cada mando es un jugador; elige su instrumento en Controles. Los jugadores 2 a 4 entran como\n"
             L"invitados: en el aviso de perfil elige \"Continue without saving\". La voz usa el micrófono de Windows.", 204, 256, 660, 46, 0, true);
    Label(4, L"Sincronía", 204, 314, 400, 22, 0, false, g.fontBold);
    Label(4, L"Calibra una vez en el juego: Options > Calibrate Lag > Calibrate.", 204, 340, 660, 22, 0, true);
    Label(4, L"Archivos", 204, 378, 400, 22, 0, false, g.fontBold);
    Label(4, L"gh6.ini: toda la configuración (lo que cambias aquí).  gh6.log: registro de la última partida.\n"
             L"Carpeta de partidas: partidas, DLC importado y respaldos. Consérvala (y gh6.ini) al actualizar a mano.\n"
             L"Para entrar directo al juego desactiva \"Mostrar este launcher al iniciar\"; Mayús al abrir lo muestra.",
          204, 404, 660, 92, 0, true);
    // Graficos
    int ws = 0; std::string v = ini.Get("ventana", "1280x720");
    for (int i = 0; i < kNumWinSizes; i++) if (v == kWinSizesIni[i]) ws = i;
    Toggle(1, L"Pantalla completa al arrancar", 204, 94, 420, ID_FULLSCREEN, flag("pantalla_completa", false));
    Label(1, L"Tamaño de la ventana", 204, 130, 190, 22);
    Combo(1, 400, 126, 200, ID_WINSIZE, { kWinSizes[0], kWinSizes[1], kWinSizes[2], kWinSizes[3], kWinSizes[4], kWinSizes[5], kWinSizes[6], kWinSizes[7], kWinSizes[8] }, ws);
    Label(1, L"Escalado de imagen", 204, 164, 190, 22);
    Combo(1, 400, 160, 200, ID_SCALE, { L"Nítido (Catmull-Rom)", L"Suave (bilineal)" }, ini.Get("escalado") == "bilineal" ? 1 : 0);
    {   // Sesion 25: resolucion interna (el juego se dibuja a 720p en la consola)
        std::string rs = ini.Get("resolucion_interna", "1");
        Label(1, L"Resolución interna", 624, 130, 240, 22);
        Combo(1, 624, 160, 240, ID_RES, { L"720p (como la consola)", L"1440p (2x, recomendada)", L"2160p / 4K (3x, PC potente)" }, rs == "2" ? 1 : rs == "3" ? 2 : 0);
        std::string pn = ini.Get("panoramico", "16:9");
        Label(1, L"Proporción (experimental)", 640, 330, 224, 22);
        Combo(1, 640, 356, 224, ID_PANO, { L"16:9 (como la consola)", L"Panorámica del monitor (exp.)", L"21:9 (experimental)", L"32:9 (experimental)" }, pn == "auto" ? 1 : pn == "21:9" ? 2 : pn == "32:9" ? 3 : 0);
    }
    Label(1, L"Suavizado de bordes", 204, 198, 190, 22);
    Combo(1, 400, 194, 260, ID_AA, { L"FXAA (quita los dientes de sierra)", L"Desactivado" }, ini.Get("suavizado", "fxaa") == "no" ? 1 : 0);
    std::string an = ini.Get("anisotropico", "16");
    Label(1, L"Filtrado de texturas", 204, 232, 190, 22);
    Combo(1, 400, 228, 260, ID_ANISO, { L"Anisotrópico 16x (recomendado)", L"Anisotrópico 8x", L"Anisotrópico 4x", L"Trilineal (como la consola)" },
          an == "8" ? 1 : an == "4" ? 2 : an == "1" ? 3 : 0);
    std::string ni = ini.Get("nitidez", "0.4");
    Label(1, L"Nitidez", 204, 266, 190, 22);
    Combo(1, 400, 262, 260, ID_SHARP, { L"Desactivada", L"Suave", L"Media (recomendada)", L"Alta" },
          ni == "0" ? 0 : ni == "0.25" ? 1 : ni == "0.7" ? 3 : 2);
    Toggle(1, L"Sincronización vertical (VSync)", 204, 298, 420, ID_VSYNC, flag("vsync", true));
    Toggle(1, L"Contador de FPS (F3)", 640, 298, 222, ID_FPS, flag("mostrar_fps", false));
    Toggle(1, L"Desenfoque de movimiento (como la consola)", 204, 328, 420, ID_MBLUR, flag("desenfoque_movimiento", true));
    Toggle(1, L"Grano de película (como la consola)", 204, 358, 420, ID_GRAIN, flag("grano_pelicula", false));
    std::string al = ini.Get("latencia_audio", "normal");
    Label(1, L"Latencia del audio", 204, 398, 190, 22, 0, false, g.fontBold);
    Combo(1, 400, 394, 260, ID_AUDIOLAT, { L"Baja (~40 ms)", L"Normal (~55 ms)", L"Segura (~95 ms, PCs lentos)" }, al == "baja" ? 0 : al == "segura" ? 2 : 1);
    Label(1, L"Baja puede dar algún chasquido en las cargas. Para que notas y música coincidan, calibra una vez\n"
             L"en el juego: Options > Calibrate Lag > Calibrate (vídeo: toca cuando la nota pase por la línea;\n"
             L"audio: toca con los golpes).", 204, 430, 660, 70, 0, true);
    // Controles
    Label(2, L"Jugador", 204, 96, 80, 22, 0, true); Label(2, L"Instrumento", 290, 96, 130, 22, 0, true);
    Label(2, L"Mando", 432, 96, 140, 22, 0, true); Label(2, L"Estado", 584, 96, 200, 22, 0, true);
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
        Label(2, lbl, 204, 124 + p * 32, 80, 22, 0, false, g.fontBold);
        Combo(2, 290, 120 + p * 32, 132, ID_INST0 + p, { L"Guitarra", L"Batería", L"Voz" }, is);
        Combo(2, 432, 120 + p * 32, 142, ID_PAD0 + p, padItems, x >= 1 && x <= 4 ? x - 1 : 4);
        Label(2, L"", 584, 124 + p * 32, 280, 22, ID_STATUS0 + p, true);
    }
    std::vector<std::wstring> players = { L"Jugador 1", L"Jugador 2", L"Jugador 3", L"Jugador 4" };
    Label(2, L"Teclado", 204, 258, 80, 22);
    Combo(2, 290, 254, 132, ID_KBD, players, atoi(ini.Get("teclado_jugador", "1").c_str()) - 1);
    Label(2, L"Mando PlayStation", 432, 258, 140, 22);
    Combo(2, 584, 254, 132, ID_PS, players, atoi(ini.Get("mando_ps_jugador", "1").c_str()) - 1);
    Toggle(2, L"Mando normal (sin convertirlo en guitarra o batería)", 204, 288, 460, ID_PADMODE, ini.Get("mando_modo", "guitarra") == "mando");
    Button(2, L"Informe de mandos", 690, 287, 172, 28, ID_DIAG);
    Label(2, L"Botones de", 204, 328, 90, 22, 0, false, g.fontBold);
    Combo(2, 290, 324, 132, ID_MAPSET, { L"Guitarra", L"Batería" }, 0);
    Combo(2, 432, 324, 150, ID_MAPPLAYER, { L"Todos los jugadores", L"Solo jugador 1", L"Solo jugador 2", L"Solo jugador 3", L"Solo jugador 4" }, 0);
    Button(2, L"Asignar...", 592, 323, 88, 28, ID_ASSIGN);
    Button(2, L"Añadir...", 686, 323, 80, 28, ID_ADD);
    Button(2, L"Preajustes", 772, 323, 90, 28, ID_RESET);
    HWND list = Make(2, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, 204, 360, 330, 150, ID_MAPLIST);
    SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
    Label(2, L"Probar", 548, 362, 60, 22, 0, true);
    Combo(2, 610, 358, 130, ID_TESTPLAYER, players, 0);
    Make(2, L"STATIC", L"", SS_OWNERDRAW, 548, 390, 314, 120, ID_TESTPANEL);
    FillMapList();
    // Microfono
    g.mics.clear(); MicListDevices(g.mics);
    std::vector<std::wstring> micItems = { L"Predeterminado de Windows" }; int ms = 0;
    std::string devIni = ini.Get("microfono_dispositivo");
    for (size_t i = 0; i < g.mics.size(); i++) { micItems.push_back(g.mics[i].second); if (!devIni.empty() && A(g.mics[i].first, CP_UTF8) == devIni) ms = (int)i + 1; }
    Toggle(3, L"Usar un micrófono de Windows como micrófono USB de la consola", 204, 100, 560, ID_MIC_ON, flag("microfono", true));
    Label(3, L"Dispositivo", 204, 148, 120, 22);
    Combo(3, 330, 144, 530, ID_MIC_DEV, micItems, ms);
    Label(3, L"Ganancia", 204, 192, 120, 22);
    HWND tb = Make(3, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 326, 188, 430, 30, ID_MIC_GAIN);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(25, 400)); SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(tb, TBM_SETPOS, TRUE, (LPARAM)std::clamp((int)lround(atof(ini.Get("microfono_ganancia", "1.0").c_str()) * 100), 25, 400));
    Label(3, L"", 766, 192, 90, 22, ID_MIC_GAINLBL);
    Label(3, L"Nivel (habla o canta para probarlo)", 204, 240, 400, 22, 0, false, g.fontBold);
    Make(3, L"STATIC", L"", SS_OWNERDRAW, 204, 266, 656, 30, ID_MIC_METER);
    Label(3, L"El micrófono se asigna al primer jugador con instrumento Voz (pestaña Controles).\n"
             L"Si la barra apenas se mueve al cantar, sube la ganancia; si llega al rojo, bájala.", 204, 312, 660, 50, 0, true);
    UpdateGainLabel(); UpdateDlcCount(); PollPads(); UpdateStatus();
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
    ini.Set("suavizado", Sel(ID_AA) == 1 ? "no" : "fxaa");
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
void SetUpdInfo(const std::wstring& t) { SetWindowTextW(Item(ID_UPDINFO), t.c_str()); }
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
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT*)l)->itemHeight = ((MEASUREITEMSTRUCT*)l)->CtlID == ID_ACHLIST ? S(46) : S(24); return TRUE;
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)w; SetTextColor(dc, g.dimLabels.count((HWND)l) ? kDim : kText); SetBkColor(dc, kBg);
        if ((HWND)l == Item(ID_SAVED)) SetTextColor(dc, kAccent2);
        return (LRESULT)g.brBg;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    { HDC dc = (HDC)w; SetTextColor(dc, kText); SetBkColor(dc, kInput); return (LRESULT)g.brInput; }
    case WM_CTLCOLORBTN: return (LRESULT)g.brBg;
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
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(w), code = HIWORD(w); HWND c = (HWND)l;
        if (code == BN_CLICKED && g.toggles.count(c))
        {
            g.toggles[c] = !g.toggles[c]; InvalidateRect(c, nullptr, TRUE);
            if (id == ID_MIC_ON) StartMeter();
            return 0;
        }
        if (id >= ID_TAB0 && id < ID_TAB0 + kTabs && code == BN_CLICKED) { ShowTab(id - ID_TAB0); return 0; }
        switch (id)
        {
        case ID_PLAY: if (Collect(true)) { g.ini.Save(ExeDir() + "\\gh6.ini"); g.result = true; DestroyWindow(h); } return 0;
        case ID_SAVE: if (Collect(false)) { g.ini.Save(ExeDir() + "\\gh6.ini"); SetWindowTextW(Item(ID_SAVED), L"Configuración guardada"); g.savedT = GetTickCount(); } return 0;
        case ID_EXIT: DestroyWindow(h); return 0;
        case ID_ISO_BROWSE: BrowseIso(); return 0;
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
    g.font = font(14, FW_NORMAL, L"Segoe UI"); g.fontBold = font(14, FW_SEMIBOLD, L"Segoe UI"); g.fontSmall = font(12, FW_NORMAL, L"Segoe UI");
    g.fontTitle = font(34, FW_BLACK, L"Segoe UI Black"); g.fontSub = font(14, FW_SEMIBOLD, L"Segoe UI");
    g.fontTab = font(15, FW_SEMIBOLD, L"Segoe UI"); g.fontIcon = font(18, FW_NORMAL, L"Segoe MDL2 Assets");
    g.brBg = CreateSolidBrush(kBg); g.brInput = CreateSolidBrush(kInput); g.brSide = CreateSolidBrush(kSide);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = g.inst; wc.lpszClassName = L"GH6Launcher"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1)); wc.hbrBackground = g.brBg;
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{ 0, 0, S(880), S(584) }; AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(0, wc.lpszClassName, L"Guitar Hero: Warriors of Rock - Launcher", style,
                            (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, g.inst, nullptr);
    BOOL dark = TRUE; DwmSetWindowAttribute(g.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    BuildUi();
    UpdateSaveInfo();
    { wchar_t t[160]; swprintf(t, 160, L"Versión instalada: v%hs  ·  repositorio %hs", AppVersion(), Repo().c_str()); SetUpdInfo(t); }
    if (Toggled(ID_UPDAUTO) || getenv("GH6_UPDATE_API")) StartUpdateCheck(true);
    ShowWindow(g.wnd, SW_SHOW); UpdateWindow(g.wnd);
    SetTimer(g.wnd, 1, 33, nullptr);
    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g.wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.meter.Stop();
    for (HFONT f : { g.font, g.fontBold, g.fontSmall, g.fontTitle, g.fontSub, g.fontTab, g.fontIcon }) DeleteObject(f);
    UnregisterClassW(wc.lpszClassName, g.inst);
    DeleteObject(g.brBg); DeleteObject(g.brInput); DeleteObject(g.brSide);
    CoUninitialize();
    SetThreadDpiAwarenessContext(oldDpi);
    LOG("[launcher] ventana cerrada: %s", g.result ? "JUGAR" : "salir");
    return g.result;
}

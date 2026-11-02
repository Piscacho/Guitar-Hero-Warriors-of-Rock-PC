// Mandos de PlayStation por HID (sesion 23): DualSense (PS5), DualSense Edge y DualShock 4 (PS4), por USB o Bluetooth.
// No usan XInput: se leen los informes HID en un hilo propio y se traducen al esquema de botones de la 360.
// Cruz = A (verde), Circulo = B (rojo), Triangulo = Y (amarillo), Cuadrado = X (azul), L1 = LB (naranja), R1 = RB,
// Options = Start, Create/Share = Back, cruceta = cruceta (puas), sticks y gatillos analogicos.
#include "runtime.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>

namespace
{
struct PsState { bool connected = false; uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t sticks[4] = {}; };
std::mutex g_mutex;
PsState g_state;
std::atomic<bool> g_started{ false };

int16_t Axis(uint8_t v, bool invert)
{
    int a = (int(v) - 128) * 256;
    if (invert) a = -a - 1;
    return (int16_t)(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
}
uint16_t Hat(uint8_t h)
{
    static const uint16_t t[9] = { 0x1, 0x1 | 0x8, 0x8, 0x2 | 0x8, 0x2, 0x2 | 0x4, 0x4, 0x1 | 0x4, 0 };   // arriba, derecha, abajo, izquierda
    return t[h < 8 ? h : 8];
}
// Diseno comun de botones (DualSense y DualShock 4): b0 = cruceta + Cuadrado/Cruz/Circulo/Triangulo, b1 = L1 R1 L2 R2 Create Options L3 R3, b2 = PS
PsState Decode(const uint8_t* sticks, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2)
{
    PsState s; s.connected = true;
    uint16_t b = Hat(b0 & 0x0F);
    if (b0 & 0x10) b |= 0x4000;   // Cuadrado -> X
    if (b0 & 0x20) b |= 0x1000;   // Cruz -> A
    if (b0 & 0x40) b |= 0x2000;   // Circulo -> B
    if (b0 & 0x80) b |= 0x8000;   // Triangulo -> Y
    if (b1 & 0x01) b |= 0x0100;   // L1 -> LB
    if (b1 & 0x02) b |= 0x0200;   // R1 -> RB
    if (b1 & 0x10) b |= 0x0020;   // Create/Share -> Back
    if (b1 & 0x20) b |= 0x0010;   // Options -> Start
    if (b1 & 0x40) b |= 0x0040;   // L3
    if (b1 & 0x80) b |= 0x0080;   // R3
    if (b2 & 0x01) b |= 0x0400;   // PS -> Guide
    s.buttons = b; s.lt = l2; s.rt = r2;
    s.sticks[0] = Axis(sticks[0], false); s.sticks[1] = Axis(sticks[1], true);
    s.sticks[2] = Axis(sticks[2], false); s.sticks[3] = Axis(sticks[3], true);
    return s;
}
bool ParseReport(const uint8_t* r, DWORD len, bool dualsense, PsState& out)
{
    if (len < 10) return false;
    if (dualsense)
    {
        if (r[0] == 0x01 && len >= 11 && len < 20)   // Bluetooth basico (como el DS4): sticks, botones, L2/R2
        { out = Decode(r + 1, r[5], r[6], r[7], r[8], r[9]); return true; }
        if (r[0] == 0x01)                             // USB: sticks [1..4], L2 [5], R2 [6], botones [8..10]
        { out = Decode(r + 1, r[8], r[9], r[10], r[5], r[6]); return true; }
        if (r[0] == 0x31 && len >= 12)                // Bluetooth completo: igual que USB desplazado un byte
        { out = Decode(r + 2, r[9], r[10], r[11], r[6], r[7]); return true; }
        return false;
    }
    // DualShock 4: USB 0x01 (sticks [1..4], botones [5..7], L2/R2 [8..9]); Bluetooth 0x11 desplazado 2 bytes
    if (r[0] == 0x01) { out = Decode(r + 1, r[5], r[6], r[7], r[8], r[9]); return true; }
    if (r[0] == 0x11 && len >= 12) { out = Decode(r + 3, r[7], r[8], r[9], r[10], r[11]); return true; }
    return false;
}
// Busca un mando de PlayStation conectado; devuelve el handle abierto
HANDLE OpenPad(bool& dualsense, std::string& name)
{
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); i++)
    {
        DWORD need = 0; SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data(); det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{ sizeof(at) };
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C)
        {
            bool ds = at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2;              // DualSense / DualSense Edge
            bool ds4 = at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0;   // DualShock 4 (v1, v2, adaptador)
            if (ds || ds4) { found = h; dualsense = ds; name = ds ? (at.ProductID == 0x0DF2 ? "DualSense Edge" : "DualSense") : "DualShock 4"; continue; }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
void PadThread()
{
    for (;;)
    {
        bool ds = false; std::string name;
        HANDLE h = OpenPad(ds, name);
        if (h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        LOG("[input] mando de PlayStation conectado: %s", name.c_str());
        uint8_t rep[128];
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(h, rep, sizeof(rep), &got, nullptr)) break;   // desconectado
            PsState s;
            if (ParseReport(rep, got, ds, s)) { std::lock_guard l(g_mutex); g_state = s; }
        }
        CloseHandle(h);
        { std::lock_guard l(g_mutex); g_state = PsState{}; }
        LOG("[input] mando de PlayStation desconectado");
    }
}
} // namespace

// Estado del mando de PlayStation (false si no hay ninguno). GH6_NOPSPAD=1 lo desactiva.
bool PsPadGetState(uint16_t& buttons, uint8_t& lt, uint8_t& rt, int16_t sticks[4])
{
    static bool disabled = getenv("GH6_NOPSPAD") != nullptr;
    if (disabled) return false;
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) std::thread(PadThread).detach();
    std::lock_guard l(g_mutex);
    if (!g_state.connected) return false;
    buttons = g_state.buttons; lt = g_state.lt; rt = g_state.rt;
    memcpy(sticks, g_state.sticks, sizeof(g_state.sticks));
    return true;
}

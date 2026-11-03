// XAM minimo (sesion 6): memoria, perfil, mandos, audio, enumeraciones, mensajes y red.
// Comportamiento segun Xenia (BSD). Mandos: de momento el mando 1 conectado y en reposo (la entrada real llega en la sesion de entrada).
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <cstring>
#include <mutex>
#include <vector>
#include <atomic>
#include <thread>
#include <deque>
#include <mmsystem.h>
#include <Xinput.h>
bool PsPadGetState(uint16_t& buttons, uint8_t& lt, uint8_t& rt, int16_t sticks[4]);   // dualsense.cpp
#include <algorithm>
#include <cmath>

namespace
{
constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_IO_PENDING = 0x3E5, X_ERROR_NO_MORE_FILES = 0x12,
    X_ERROR_DEVICE_NOT_CONNECTED = 0x48F, X_ERROR_NO_SUCH_USER = 0x525, X_ERROR_EMPTY = 0x10D2;
constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;

// X_OVERLAPPED: +0 result, +4 length, +8 context, +0xC evento, +0x10 rutina, +0x14 contexto, +0x18 error extendido
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length = 0)
{
    if (!ov) return;
    Store32(ov + 0x00, result);
    Store32(ov + 0x04, length);
    Store32(ov + 0x18, result);
    uint32_t ev = Load32(ov + 0x0C);
    if (ev) if (auto o = GetHandleObject(ev)) SetEvent(o->waitable);
}
}

// ---- perfil: ajustes y privilegios (sesion 12) ----
// XamUserReadProfileSettings(title_id, user_index, xuid_count, xuids, setting_count, setting_ids, buffer_size_ptr, buffer_ptr, [pila] overlapped)
// Resultado: {u32 setting_count, u32 settings_ptr} + setting_count x X_USER_PROFILE_SETTING (0x28 bytes):
//   +0x00 from (0 = no establecido -> el juego usa su valor por defecto), +0x08 user_index/xuid, +0x10 setting_id, +0x18 X_USER_DATA (tipo + valor)
GUEST_IMPL(XamUserReadProfileSettings)
{
    uint32_t userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, settingCount = ctx.r7.u32, settingIds = ctx.r8.u32;
    uint32_t sizePtr = ctx.r9.u32, buf = ctx.r10.u32, overlapped = Load32(ctx.r1.u32 + 0x54);
    uint32_t entries = settingCount * (xuidCount ? xuidCount : 1);
    uint32_t need = 8 + entries * 0x28;
    uint32_t have = sizePtr ? Load32(sizePtr) : 0;
    if (!buf || have < need)
    {
        if (sizePtr) Store32(sizePtr, need);
        ctx.r3.u64 = 0x7A; // ERROR_INSUFFICIENT_BUFFER
        return;
    }
    memset(g_base + buf, 0, need);
    Store32(buf, entries); Store32(buf + 4, buf + 8);
    for (uint32_t i = 0; i < entries; i++)
    {
        uint32_t e = buf + 8 + i * 0x28;
        Store32(e + 0x00, 0);                                  // from: no establecido
        Store32(e + 0x08, userIndex);
        uint32_t sid = settingIds ? Load32(settingIds + (i % settingCount) * 4) : 0;
        Store32(e + 0x10, sid);
        g_base[e + 0x18] = (uint8_t)((sid >> 28) & 0xF);       // X_USER_DATA.type = tipo codificado en el ID (valor 0 / binario vacio)
        static int logged = 0;
        if (logged < 16) { logged++; LOG("[xam] ReadProfileSettings: setting 0x%08X (tipo %u, tamano %u)", sid, (sid >> 28) & 0xF, (sid >> 16) & 0xFFF); }
    }
    if (overlapped) { CompleteOverlapped(overlapped, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}
// ---- notificaciones del sistema (sesion 12) ----
// XamNotifyCreateListener(mask, max_version) -> handle (0 = fallo). De momento sin notificaciones pendientes.
// Listener con cola de notificaciones (como Xenia: el primer listener con mascara de sistema recibe las de arranque)
struct NotifyListener : KObject
{
    uint64_t mask = 0;
    std::mutex m;
    std::vector<std::pair<uint32_t, uint32_t>> q;
    void Enqueue(uint32_t id, uint32_t data)
    {
        uint32_t maskIndex = (id >> 25) & 0x3F;
        if (!(mask & (1ull << maskIndex))) return;
        std::lock_guard l(m); q.emplace_back(id, data); SetEvent(waitable);
    }
};
std::mutex g_listenersMutex;
std::vector<std::weak_ptr<NotifyListener>> g_listeners;
static void BroadcastNotification(uint32_t id, uint32_t data)
{
    std::lock_guard l(g_listenersMutex);
    for (auto& w : g_listeners) if (auto o = w.lock()) o->Enqueue(id, data);
}
GUEST_IMPL(XamNotifyCreateListener)
{
    static bool startupDone = false;
    auto o = std::make_shared<NotifyListener>();
    o->type = 0; o->waitable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    o->mask = ctx.r3.u64;
    static bool everyListener = getenv("GH6_NOTIFY_FIRSTONLY") == nullptr; // por defecto: a cada listener de sistema
    if ((!startupDone || everyListener) && (o->mask & 1))
    {
        startupDone = true;
        o->Enqueue(0x09, 1); o->Enqueue(0x09, 0);   // XN_SYS_UI on/off
        o->Enqueue(0x0A, 1); o->Enqueue(0x0A, 1);   // XN_SYS_SIGNINCHANGED (usuario 0)
        o->Enqueue(0x12, 0); o->Enqueue(0x12, 0);   // XN_SYS_INPUTDEVICESCHANGED
        o->Enqueue(0x13, 0); o->Enqueue(0x13, 0);   // XN_SYS_INPUTDEVICECONFIGCHANGED
    }
    // Sesion 23: avisos de almacenamiento y de contenido instalado, para que el juego busque el DLC (tipo 2)
    static bool noDlcNotify = getenv("GH6_NODLCNOTIFY") != nullptr;
    if (!noDlcNotify)
    {
        if (o->mask & 1) o->Enqueue(0x0B, 0);              // XN_SYS_STORAGEDEVICESCHANGED
        if (o->mask & 2) o->Enqueue(0x02000007, 0);        // XN_LIVE_CONTENT_INSTALLED
    }
    uint32_t h = InsertHandle(o);
    { std::lock_guard l(g_listenersMutex); g_listeners.push_back(o); }
    LOG("[xam] XamNotifyCreateListener(mask 0x%llX) -> 0x%08X", (unsigned long long)ctx.r3.u64, h);
    ctx.r3.u64 = h;
}
// XNotifyGetNext(handle, match_id, id_ptr, param_ptr) -> TRUE si habia notificacion
GUEST_IMPL(XNotifyGetNext)
{
    uint32_t matchId = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    if (paramPtr) Store32(paramPtr, 0);
    if (!idPtr) { ctx.r3.u64 = 0; return; }
    Store32(idPtr, 0);
    auto l = std::dynamic_pointer_cast<NotifyListener>(GetHandleObject(ctx.r3.u32));
    static int calls = 0;
    if (calls++ < 6) LOG("[xam] XNotifyGetNext(handle 0x%08X, match 0x%X) listener=%d cola=%zu", ctx.r3.u32, matchId, l ? 1 : 0, l ? l->q.size() : 0);
    if (!l) { ctx.r3.u64 = 0; return; }
    std::lock_guard lk(l->m);
    for (size_t i = 0; i < l->q.size(); i++)
        if (!matchId || l->q[i].first == matchId)
        {
            Store32(idPtr, l->q[i].first);
            if (paramPtr) Store32(paramPtr, l->q[i].second);
            LOG("[xam] XNotifyGetNext -> 0x%X (param %u)", l->q[i].first, l->q[i].second);
            l->q.erase(l->q.begin() + i);
            if (l->q.empty()) ResetEvent(l->waitable);
            ctx.r3.u64 = 1;
            return;
        }
    ctx.r3.u64 = 0;
}
GUEST_IMPL(XNotifyPositionUI) { ctx.r3.u64 = 0; }
// ---- contenido (partidas/DLC) - sesion 13: enumerador vacio por ahora (el DLC llega en su sesion) ----
// XamContentCreateEnumerator(user_index, device_id, content_type, content_flags, items_per_enumerate, buffer_size_ptr, handle_out)
// Sesion 16: enumera las partidas de userdata\content (un <carpeta>.xcd con el XCONTENT_DATA de 0x134 bytes por cada una).
std::string FsContentDir();
struct ContentEnum : KObject { std::vector<std::vector<uint8_t>> items; size_t next = 0; uint32_t perCall = 1; uint32_t itemSize = 0x134; };
int FsMountContent(const std::string& root, const std::string& folder, bool create);   // fs.cpp
// Lista de XCONTENT_DATA (0x134 bytes) de un tipo de contenido en userdata\content
static std::vector<std::vector<uint8_t>> ListContent(uint32_t type)
{
    std::vector<std::vector<uint8_t>> out;
    char pattern[512]; snprintf(pattern, sizeof(pattern), "%s\\%08x_*.xcd", FsContentDir().c_str(), type);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        std::vector<uint8_t> d(0x134);
        std::string path = FsContentDir() + "\\" + fd.cFileName;
        if (FILE* f = fopen(path.c_str(), "rb")) { fread(d.data(), 1, d.size(), f); fclose(f); out.push_back(d); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}
// Enumeracion por mensajes de XAM del juego (sub_826C5BD0: handle, buffer, tamano 312, items_ptr, overlapped):
// se sustituye por una lectura directa del enumerador (el original pide los datos a la aplicacion de contenido por XMsg).
PPC_FUNC_IMPL(__imp__sub_826C5BD0);
PPC_FUNC(sub_826C5BD0)
{
    auto e = std::dynamic_pointer_cast<ContentEnum>(GetHandleObject(ctx.r3.u32));
    if (!e) { __imp__sub_826C5BD0(ctx, base); return; }
    uint32_t buf = ctx.r4.u32, size = ctx.r5.u32, itemsPtr = ctx.r6.u32, ov = ctx.r7.u32;
    uint32_t n = 0;
    while (e->next < e->items.size() && (n + 1) * e->itemSize <= size && n < e->perCall)
    {
        memcpy(g_base + buf + n * e->itemSize, e->items[e->next].data(), e->itemSize);
        e->next++; n++;
    }
    if (itemsPtr) Store32(itemsPtr, n);
    uint32_t result = n ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    LOG("[xam] enumeracion de contenido (aggregate): %u elementos (%zu/%zu)", n, e->next, e->items.size());
    if (ov) { CompleteOverlapped(ov, result, n); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = result;
}
// XamContentAggregateCreateEnumerator (ordinal 633, sesion 23; el juego la obtiene con XexGetProcedureAddress para buscar el DLC):
// (xuid, device_id, content_type, flags, handle_out). Elementos XCONTENT_AGGREGATE_DATA = XCONTENT_DATA + title_id (0x138 bytes).
PPC_FUNC(gh6_XamContentAggregateCreateEnumerator)
{
    uint32_t device = ctx.r4.u32, type = ctx.r5.u32, handleOut = ctx.r7.u32;
    auto o = std::make_shared<ContentEnum>();
    o->guestPtr = GuestAllocKernel(0x40, 0x10);
    o->itemSize = 0x138; o->perCall = 1;
    if (device == 0 || device == 1)
        for (auto& d : ListContent(type))
        {
            // El juego monta cada paquete como CONT<n>: por mensajes de XAM (no por XamContentCreate): se monta aqui en el mismo orden
            std::string file; for (int i = 0; i < 42 && d[0x108 + i]; i++) file += (char)d[0x108 + i];
            char folder[96]; snprintf(folder, sizeof(folder), "%08x_%s", type, file.c_str());
            std::string fl = folder; for (auto& c : fl) c = (char)tolower((unsigned char)c);
            char root[16]; snprintf(root, sizeof(root), "CONT%zu", o->items.size());
            FsMountContent(root, fl, false);
            d.resize(0x138);
            d[0x134] = 0x41; d[0x135] = 0x56; d[0x136] = 0x08; d[0x137] = 0x83;   // title_id (BE)
            o->items.push_back(d);
        }
    if (handleOut) Store32(handleOut, InsertHandle(o));
    LOG("[xam] XamContentAggregateCreateEnumerator(dispositivo %u, tipo 0x%X) -> %zu elementos", device, type, o->items.size());
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
GUEST_IMPL(XamContentCreateEnumerator)
{
    uint32_t device = ctx.r4.u32, type = ctx.r5.u32;
    if (ctx.r8.u32) Store32(ctx.r8.u32, 0x134 * ctx.r7.u32);   // sizeof(XCONTENT_DATA) * items
    auto o = std::make_shared<ContentEnum>();
    o->guestPtr = GuestAllocKernel(0x40, 0x10);
    o->perCall = ctx.r7.u32 ? ctx.r7.u32 : 1;
    if (device == 0 || device == 1)
    {
        char pattern[512]; snprintf(pattern, sizeof(pattern), "%s\\%08x_*.xcd", FsContentDir().c_str(), type);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::vector<uint8_t> d(0x134);
                std::string path = FsContentDir() + "\\" + fd.cFileName;
                if (FILE* f = fopen(path.c_str(), "rb")) { fread(d.data(), 1, d.size(), f); fclose(f); o->items.push_back(d); }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (ctx.r9.u32) Store32(ctx.r9.u32, InsertHandle(o));
    LOG("[xam] XamContentCreateEnumerator(usuario %u, dispositivo %u, tipo 0x%X) -> %zu elementos", ctx.r3.u32, device, type, o->items.size());
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
GUEST_IMPL(XNotifyDelayUI) { ctx.r3.u64 = 0; }
// XamGetExecutionId(info_ptr): puntero a la cabecera XEX_HEADER_EXECUTION_INFO (0x40006), como Xenia. Antes era un stub que no
// escribia *info_ptr: el juego leia basura de la pila y, con 7 o mas DLC, se quedaba en "Checking for downloadable content".
GUEST_IMPL(XamGetExecutionId)
{
    uint32_t info = XexHeaderField(0x40006);
    if (ctx.r3.u32 && info) Store32(ctx.r3.u32, info);
    static bool once = false;
    if (!once) { once = true; LOG("[xam] XamGetExecutionId -> %08X", info); }
    ctx.r3.u64 = info ? X_ERROR_SUCCESS : 0xC0000225;   // STATUS_NOT_FOUND
}

// XamContentGetDeviceData(device_id, device_data_ptr). X_CONTENT_DEVICE_DATA (0x50 bytes, como Xenia):
// +0 device_id, +4 device_type (1 = disco duro), +8 total_bytes (u64), +0x10 free_bytes (u64), +0x18 nombre (UTF-16 BE, 28 caracteres)
GUEST_IMPL(XamContentGetDeviceData)
{
    uint32_t id = ctx.r3.u32, p = ctx.r4.u32;
    if (id != 1) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (p)
    {
        memset(g_base + p, 0, 0x50);
        Store32(p + 0, 1); Store32(p + 4, 1);
        Store64(p + 8, 20ull << 30); Store64(p + 0x10, 16ull << 30);   // 20 GB, 16 GB libres
        const char* name = "Hard Drive";
        for (int i = 0; name[i]; i++) Store16(p + 0x18 + i * 2, (uint16_t)name[i]);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- partidas guardadas (sesion 16): cada contenido es una carpeta en userdata\content ----
int FsMountContent(const std::string& root, const std::string& folder, bool create);
void FsUnmountContent(const std::string& root);
static std::string GuestStr(uint32_t p, size_t max = 64)
{
    std::string s; if (!p) return s;
    for (size_t i = 0; i < max && g_base[p + i]; i++) s += (char)g_base[p + i];
    return s;
}
// XamContentCreateEx(user_index, root_name, content_data, flags, disposition_ptr, license_mask_ptr, cache_size, content_size, [pila] overlapped)
// XCONTENT_DATA: +0 device_id, +4 content_type, +8 display_name (UTF-16, 128), +0x108 file_name (char[42])
static void ContentCreate(PPCContext& ctx, uint32_t overlapped)
{
    std::string root = GuestStr(ctx.r4.u32), file = GuestStr(ctx.r5.u32 + 0x108, 42);
    uint32_t type = Load32(ctx.r5.u32 + 4), flags = ctx.r6.u32, dispPtr = ctx.r7.u32, licPtr = ctx.r8.u32;
    char folderBuf[96]; snprintf(folderBuf, sizeof(folderBuf), "%08x_%s", type, file.c_str());
    std::string folder = folderBuf;
    for (auto& c : folder) c = (char)tolower((unsigned char)c);
    // flags: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING
    uint32_t mode = flags & 0xF;
    bool create = mode == 1 || mode == 2 || mode == 4;
    int r = FsMountContent(root, folder, create);
    uint32_t result = X_ERROR_SUCCESS;
    if (r < 0) result = 3;                         // X_ERROR_PATH_NOT_FOUND
    else if (r == 1 && mode == 1) result = 0xB7;   // X_ERROR_ALREADY_EXISTS
    if (r >= 0)
    {
        // Guardar el XCONTENT_DATA junto a la carpeta para poder enumerarlo despues (<carpeta>.xcd)
        std::string meta = FsContentDir() + "\\" + folder + ".xcd";
        if (FILE* f = fopen(meta.c_str(), "wb")) { Store32(ctx.r5.u32, 1); fwrite(g_base + ctx.r5.u32, 1, 0x134, f); fclose(f); }
    }
    if (dispPtr) Store32(dispPtr, r == 0 ? 1 : 2); // 1 creado, 2 abierto
    if (licPtr) Store32(licPtr, type == 2 ? 0xFFFFFFFF : 0);   // DLC (Marketplace): con licencia completa
    LOG("[xam] XamContentCreate(raiz '%s', archivo '%s', tipo 0x%X, modo %u) -> 0x%X", root.c_str(), file.c_str(), type, mode, result);
    if (overlapped) { CompleteOverlapped(overlapped, result); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = result;
}
GUEST_IMPL(XamContentCreateEx) { ContentCreate(ctx, Load32(ctx.r1.u32 + 0x54)); }
// XamContentCreateInternal (ordinal 625, por XexGetProcedureAddress): (root_name, content_data, flags, disposition_ptr,
// license_mask_ptr, cache_size, content_size, overlapped) = XamContentCreateEx sin user_index (sesion 24, como Xenia)
PPC_FUNC(gh6_XamContentCreateInternal)
{
    PPCContext c = ctx;
    c.r4 = ctx.r3; c.r5 = ctx.r4; c.r6 = ctx.r5; c.r7 = ctx.r6; c.r8 = ctx.r7;
    ContentCreate(c, ctx.r10.u32);
    ctx.r3 = c.r3;
}
// XamContentGetCreator(user_index, content_data, is_creator_ptr, creator_xuid_ptr, overlapped): las partidas son siempre del usuario 0
GUEST_IMPL(XamContentGetCreator)
{
    if (ctx.r5.u32) Store32(ctx.r5.u32, 1);
    if (ctx.r6.u32) Store64(ctx.r6.u32, kXuid);
    if (ctx.r7.u32) { CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamContentDelete(user_index, content_data, overlapped): borra la carpeta de la partida en userdata\content
GUEST_IMPL(XamContentDelete)
{
    std::string file = GuestStr(ctx.r4.u32 + 0x108, 42);
    char folderBuf[96]; snprintf(folderBuf, sizeof(folderBuf), "%08x_%s", Load32(ctx.r4.u32 + 4), file.c_str());
    std::string folder = folderBuf;
    for (auto& c : folder) c = (char)tolower((unsigned char)c);
    uint32_t result = X_ERROR_SUCCESS;
    if (file.empty() || folder.find("..") != std::string::npos || folder.find_first_of("\\/:") != std::string::npos) result = 3;
    else
    {
        std::string dir = FsContentDir() + "\\" + folder;
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str());
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir.c_str());
        DeleteFileA((dir + ".xcd").c_str());
    }
    LOG("[xam] XamContentDelete('%s') -> 0x%X", folder.c_str(), result);
    if (ctx.r5.u32) { CompleteOverlapped(ctx.r5.u32, result); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = result;
}
// XamContentClose(root_name, overlapped)
GUEST_IMPL(XamContentClose)
{
    FsUnmountContent(GuestStr(ctx.r3.u32));
    if (ctx.r4.u32) { CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamContentSetThumbnail(root_name, buffer, size, overlapped)
GUEST_IMPL(XamContentSetThumbnail)
{
    if (ctx.r6.u32) { CompleteOverlapped(ctx.r6.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- interfaces del sistema sin interfaz (sesion 23): se completan al instante para que el juego no se quede esperando ----
static void FinishUi(PPCContext& ctx, uint32_t ov, uint32_t result)
{
    BroadcastNotification(0x09, 1); BroadcastNotification(0x09, 0);   // XN_SYS_UI on/off
    if (ov) { CompleteOverlapped(ov, result); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = result;
}
// XamShowKeyboardUI(user, flags, default_text, title, description, buffer, buffer_length, overlapped): devuelve el texto por defecto
GUEST_IMPL(XamShowKeyboardUI)
{
    uint32_t def = ctx.r5.u32, buf = ctx.r8.u32, len = ctx.r9.u32, ov = ctx.r10.u32;
    if (buf && len)
    {
        uint32_t i = 0;
        if (def) for (; i + 1 < len; i++) { uint16_t c = __builtin_bswap16(*(uint16_t*)(g_base + def + i * 2)); if (!c) break; Store16(buf + i * 2, c); }
        else { const char* d = "Rocker"; for (; d[i] && i + 1 < len; i++) Store16(buf + i * 2, (uint16_t)d[i]); }
        Store16(buf + i * 2, 0);
    }
    LOG("[xam] XamShowKeyboardUI -> texto por defecto");
    FinishUi(ctx, ov, X_ERROR_SUCCESS);
}
// XamShowMessageBoxUI(user, title, text, button_count, buttons, active_button, flags, result_ptr, [pila] overlapped)
GUEST_IMPL(XamShowMessageBoxUI)
{
    uint32_t active = ctx.r8.u32, resultPtr = ctx.r10.u32, ov = Load32(ctx.r1.u32 + 0x54);
    if (resultPtr) Store32(resultPtr, active);
    LOG("[xam] XamShowMessageBoxUI -> boton %u", active);
    FinishUi(ctx, ov, X_ERROR_SUCCESS);
}
// XamShowMessageBoxUIEx(user, title, text, button_count, buttons, active_button, flags, unknown, [pila] result_ptr, overlapped)
GUEST_IMPL(XamShowMessageBoxUIEx)
{
    uint32_t active = ctx.r8.u32, resultPtr = Load32(ctx.r1.u32 + 0x54), ov = Load32(ctx.r1.u32 + 0x5C);
    if (resultPtr) Store32(resultPtr, active);
    LOG("[xam] XamShowMessageBoxUIEx -> boton %u", active);
    FinishUi(ctx, ov, X_ERROR_SUCCESS);
}
// XamUserWriteProfileSettings(title_id, user_index, setting_count, settings, overlapped)
GUEST_IMPL(XamUserWriteProfileSettings) { if (ctx.r7.u32) { CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; } else ctx.r3.u64 = X_ERROR_SUCCESS; }
// XamContentGetDeviceState(device_id, overlapped)
GUEST_IMPL(XamContentGetDeviceState)
{
    uint32_t r = (ctx.r3.u32 == 1) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    if (ctx.r4.u32) { CompleteOverlapped(ctx.r4.u32, r); ctx.r3.u64 = X_ERROR_IO_PENDING; } else ctx.r3.u64 = r;
}
// XamGetOverlappedResult(overlapped, length_ptr, wait): nuestras operaciones se completan al instante
GUEST_IMPL(XamGetOverlappedResult)
{
    uint32_t ov = ctx.r3.u32;
    if (!ov) { ctx.r3.u64 = 87; return; }
    if (ctx.r4.u32) Store32(ctx.r4.u32, Load32(ov + 4));
    ctx.r3.u64 = Load32(ov);
}
// XamShowDirtyDiscErrorUI: error de lectura del disco (no deberia ocurrir con el ISO)
GUEST_IMPL(XamShowDirtyDiscErrorUI) { LOG("[xam] XamShowDirtyDiscErrorUI: el juego informa de un error de lectura del disco"); ctx.r3.u64 = 0; }

// XamReadTileToTexture(tile_type, title_id, tile_id, user_index, buffer, stride, height, overlapped) (sesion 23):
// Sesion 24: imagen de jugador. Antes se devolvia transparente y en los menus se veia el cuadrado negro de fondo (junto a
// "STARS TO RANK"). Para las imagenes de jugador (tipos 2-5) se usa gamerpic.bmp de la carpeta del juego (BMP de 24 o 32
// bits, cualquier tamano, se escala) o, si no existe, una imagen por defecto dibujada aqui (una pua sobre un degradado).
// Formato del destino: A8R8G8B8 lineal en big endian (bytes A R G B), stride en bytes. Otros tipos: transparente.
namespace
{
struct GamerPic { int w = 0, h = 0; std::vector<uint32_t> argb; };
const GamerPic& LoadGamerPic()
{
    static GamerPic pic;
    static bool done = false;
    if (done) return pic;
    done = true;
    if (FILE* f = fopen("gamerpic.bmp", "rb"))
    {
        std::vector<uint8_t> d;
        uint8_t buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
        fclose(f);
        if (d.size() > 54 && d[0] == 'B' && d[1] == 'M')
        {
            uint32_t off = *(uint32_t*)&d[10];
            int32_t w = *(int32_t*)&d[18], h = *(int32_t*)&d[22];
            uint16_t bpp = *(uint16_t*)&d[28];
            bool topDown = h < 0; if (h < 0) h = -h;
            uint32_t rowBytes = ((uint32_t(w) * bpp / 8) + 3) & ~3u;
            if ((bpp == 24 || bpp == 32) && w > 0 && h > 0 && w <= 1024 && h <= 1024 && off + uint64_t(rowBytes) * h <= d.size())
            {
                pic.w = w; pic.h = h; pic.argb.resize(size_t(w) * h);
                for (int y = 0; y < h; y++)
                {
                    const uint8_t* row = &d[off + size_t(topDown ? y : h - 1 - y) * rowBytes];
                    for (int x = 0; x < w; x++)
                    {
                        const uint8_t* px = row + x * (bpp / 8);
                        uint32_t a = bpp == 32 ? px[3] : 255;
                        if (bpp == 32 && a == 0 && (px[0] | px[1] | px[2])) a = 255;   // BMP de 32 bits sin alfa real
                        pic.argb[size_t(y) * w + x] = (a << 24) | (uint32_t(px[2]) << 16) | (uint32_t(px[1]) << 8) | px[0];
                    }
                }
                LOG("[xam] imagen de jugador: gamerpic.bmp (%dx%d)", w, h);
                return pic;
            }
        }
        LOG("[xam] gamerpic.bmp no valido (se usa la imagen por defecto)");
    }
    // Imagen por defecto 64x64: degradado rojo-naranja con una pua blanca con borde oscuro
    const int N = 64;
    pic.w = pic.h = N; pic.argb.resize(N * N);
    auto inPick = [](float x, float y, float grow) {
        // Pua: triangulo redondeado (distancia al triangulo encogido <= radio de las esquinas)
        const float ax = 21, ay = 19, bx = 43, by = 19, cx = 32, cy = 43, rad = 7;
        auto seg = [](float px, float py, float x0, float y0, float x1, float y1) {
            float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
            float t = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
            float dx = wx - vx * t, dy = wy - vy * t; return std::sqrt(dx * dx + dy * dy);
        };
        auto side = [](float px, float py, float x0, float y0, float x1, float y1) { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
        bool inside = side(x, y, ax, ay, bx, by) >= 0 && side(x, y, bx, by, cx, cy) >= 0 && side(x, y, cx, cy, ax, ay) >= 0;
        if (inside) return true;
        float d = std::min({ seg(x, y, ax, ay, bx, by), seg(x, y, bx, by, cx, cy), seg(x, y, cx, cy, ax, ay) });
        return d <= rad + grow;
    };
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
        {
            float fx = x + 0.5f, fy = y + 0.5f;
            float g = fy / N;
            uint32_t r = uint32_t(200 + 40 * (1 - g)), gg = uint32_t(40 + 70 * (1 - g)), b = 30;
            uint32_t c = 0xFF000000u | (r << 16) | (gg << 8) | b;
            if (inPick(fx, fy, 2.0f)) c = 0xFF201010u;                       // borde
            if (inPick(fx, fy, 0.0f))
            {
                float shade = 0.85f + 0.15f * (1 - fy / N);
                uint32_t v = uint32_t(255 * shade);
                c = 0xFF000000u | (v << 16) | (v << 8) | v;
            }
            pic.argb[size_t(y) * N + x] = c;
        }
    return pic;
}
} // namespace
GUEST_IMPL(XamReadTileToTexture)
{
    uint32_t type = ctx.r3.u32, buf = ctx.r7.u32, stride = ctx.r8.u32, height = ctx.r9.u32, ov = ctx.r10.u32;
    static int logged = 0;
    if (logged++ < 8) LOG("[xam] XamReadTileToTexture(tipo %u, titulo %08X, tile %08X, usuario %u, buffer %08X, stride %u, alto %u)",
                          type, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, buf, stride, height);
    if (buf && stride && height && uint64_t(stride) * height < (16u << 20))
    {
        memset(g_base + buf, 0, size_t(stride) * height);
        static const bool noPic = getenv("GH6_NOGAMERPIC") != nullptr;
        if (!noPic && type >= 2 && type <= 5)
        {
            const GamerPic& pic = LoadGamerPic();
            uint32_t w = stride / 4;
            if (pic.w > 0 && w > 0)
                for (uint32_t y = 0; y < height; y++)
                    for (uint32_t x = 0; x < w; x++)
                    {
                        uint32_t c = pic.argb[size_t(y * pic.h / height) * pic.w + (x * pic.w / w)];
                        Store32(buf + y * stride + x * 4, c);   // big endian: bytes A R G B
                    }
        }
    }
    if (ov) { CompleteOverlapped(ov, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- selector de dispositivo de almacenamiento (sesion 16) ----
// XamShowDeviceSelectorUI(user_index, content_type, content_flags, total_requested, device_id_ptr, overlapped)
// Sin interfaz: se elige directamente el disco duro (ID 1, como el dispositivo ficticio de Xenia) y se avisa con XN_SYS_UI on/off.
GUEST_IMPL(XamShowDeviceSelectorUI)
{
    uint32_t devPtr = ctx.r7.u32, ov = ctx.r8.u32;
    LOG("[xam] XamShowDeviceSelectorUI(usuario %u, tipo 0x%X, flags 0x%X) -> disco duro", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    if (devPtr) Store32(devPtr, 1);
    BroadcastNotification(0x09, 1);
    BroadcastNotification(0x09, 0);
    if (ov) { CompleteOverlapped(ov, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- chat de voz: no disponible (como Xenia) ----
// XamVoiceCreate(user_index, unk, voice_ptr_out)
GUEST_IMPL(XamVoiceCreate) { if (ctx.r5.u32) Store32(ctx.r5.u32, 0); ctx.r3.u64 = 5; } // ERROR_ACCESS_DENIED
// XamUserCheckPrivilege(user_index, privilege_mask, out_result)
GUEST_IMPL(XamUserCheckPrivilege) { if (ctx.r5.u32) Store32(ctx.r5.u32, 1); ctx.r3.u64 = X_ERROR_SUCCESS; }
GUEST_IMPL(XMsgCancelIORequest) { ctx.r3.u64 = X_ERROR_SUCCESS; }

// ---- memoria ----
// XamAlloc(flags, size, out_ptr)
GUEST_IMPL(XamAlloc)
{
    uint32_t p = GuestAllocVirtual(ctx.r4.u32 ? ctx.r4.u32 : 1, 0x10);
    if (ctx.r5.u32) Store32(ctx.r5.u32, p);
    ctx.r3.u64 = p ? X_ERROR_SUCCESS : 0x8007000E; // E_OUTOFMEMORY
}
GUEST_IMPL(XamFree) { ctx.r3.u64 = X_ERROR_SUCCESS; }

// ---- perfil ----
// 1 = sesion local iniciada, 2 = sesion en Live. GH6_SIGNIN=<n> fuerza el estado del usuario 0 (diagnostico, sesion 24)
GUEST_IMPL(XamUserGetSigninState)
{
    static int st = getenv("GH6_SIGNIN") ? atoi(getenv("GH6_SIGNIN")) : 1;
    static int logged = 0;
    if (getenv("GH6_CFUNCLOG") && logged < 10) { logged++; LOG("[xam] XamUserGetSigninState(%u)", ctx.r3.u32); }
    ctx.r3.u64 = ctx.r3.u32 == 0 ? st : 0;
}
// XamUserGetSigninInfo(user, flags, X_USER_SIGNIN_INFO*) {xuid u64, unk08, signin_state, unk10, unk14, name[16]}
GUEST_IMPL(XamUserGetSigninInfo)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    uint32_t p = ctx.r5.u32;
    memset(g_base + p, 0, 0x28);
    Store64(p, kXuid); Store32(p + 0x0C, 1);
    memcpy(g_base + p + 0x18, "Player", 7);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamUserGetXUID(user, type_mask, xuid_ptr)
GUEST_IMPL(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (ctx.r5.u32) Store64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamUserGetName(user, buffer, size)
GUEST_IMPL(XamUserGetName)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (ctx.r4.u32 && ctx.r5.u32) { uint32_t n = ctx.r5.u32 < 7 ? ctx.r5.u32 : 7; memcpy(g_base + ctx.r4.u32, "Player", n); g_base[ctx.r4.u32 + n - 1] = 0; }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamShowSigninUI(users, flags): sesion 24, varios jugadores. Los jugadores 2-4 juegan como invitados (sin perfil: sus partidas
// no pisan la del jugador 1). Se avisa de que la interfaz se abrio y se cerro sin cambios para que el juego vuelva a su menu.
GUEST_IMPL(XamShowSigninUI)
{
    LOG("[xam] XamShowSigninUI(usuarios %u, flags 0x%X): sin perfiles extra, se cierra", ctx.r3.u32, ctx.r4.u32);
    BroadcastNotification(0x09, 1); BroadcastNotification(0x09, 0);   // XN_SYS_UI abierta / cerrada
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// ---- microfono USB (sesion 24) ----
// El juego busca el microfono: XamUserGetDeviceContext(usuario, 4, &ctx) -> ctx = (familia << 28) | puerto, familia 6 = MicDeviceRequest,
// 5 = RmcDeviceRequest (sub_82769F58). Despues habla con el por MicDeviceRequest con peticiones {u16 tipo, u16 puerto, ...}.
// GH6_MIC=0 lo desactiva; GH6_MIC_USER = usuario al que se asocia (0 por defecto).
static bool MicEnabled() { static const bool on = !(getenv("GH6_MIC") && atoi(getenv("GH6_MIC")) == 0); return on; }
int MicPlayer(); bool InputPlayerConnected(int u);   // mas abajo, con la entrada: el primer jugador con instrumento voz
static uint32_t MicUser() { return (uint32_t)MicPlayer(); }
GUEST_IMPL(XamUserGetDeviceContext)
{
    uint32_t user = ctx.r3.u32 & 0xFF, type = ctx.r4.u32, out = ctx.r5.u32;
    uint32_t v = 0;
    if (MicEnabled() && type == 4 && user == MicUser()) v = 0x60000001;   // microfono USB en el puerto 1
    if (out) Store32(out, v);
    static int logged = 0;
    if (logged < 12) { logged++; LOG("[mic] XamUserGetDeviceContext(usuario %u, tipo %u) -> %08X", user, type, v); }
    ctx.r3.u64 = (v || user == 0 || InputPlayerConnected((int)user)) ? 0 : X_ERROR_DEVICE_NOT_CONNECTED;
}
// Diagnostico (GH6_MICTRACE=1): objeto microfono del juego (sub_824387B0, actualizacion por frame) y arranque de lectura (sub_824380E0)
PPC_FUNC_IMPL(__imp__sub_824387B0);
PPC_FUNC(sub_824387B0)
{
    static const bool tr = getenv("GH6_MICTRACE") != nullptr;
    uint32_t o = ctx.r3.u32;
    if (tr)
    {
        static uint32_t last[5] = { ~0u, ~0u, ~0u, ~0u, ~0u }; static int n = 0;
        uint32_t cur[5] = { Load32(o), Load32(o + 4), Load32(o + 8), g_base[o + 12], g_base[o + 13] };
        if (memcmp(cur, last, sizeof(cur)) && n++ < 60)
        {
            uint32_t idx = cur[1] < 4 ? cur[1] : 0;
            LOG("[mic] objeto %08X: estado %u indice %d handle %08X activo %u leyendo %u | flags[%u]=%X", o, cur[0], (int)cur[1], cur[2], cur[3], cur[4], idx, Load32(0x829C5DAC + idx * 4));
            memcpy(last, cur, sizeof(cur));
        }
    }
    __imp__sub_824387B0(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_8276B1B8);
PPC_FUNC(sub_8276B1B8)
{   // lectura del microfono (handle, n buffers, tabla de buffers, tamanos, contexto)
    uint32_t h = ctx.r3.u32, n = ctx.r4.u32, b = ctx.r5.u32, s = ctx.r6.u32, c7 = ctx.r7.u32;
    __imp__sub_8276B1B8(ctx, base);
    static int logged = 0;
    if (logged++ < 20) LOG("[mic] leer(handle %08X, %u buffers, tabla %08X, %08X, %08X) -> %u", h, n, b, s, c7, ctx.r3.u32);
}
PPC_FUNC_IMPL(__imp__sub_824380E0);
PPC_FUNC(sub_824380E0)
{
    LOG("[mic] arranque de lectura (sub_824380E0) objeto %08X", ctx.r3.u32);
    __imp__sub_824380E0(ctx, base);
}
// Completa una peticion: llama a su callback (peticion+16) con (peticion, estado) y devuelve STATUS_PENDING al juego, como si el
// "hardware" hubiera terminado al instante. El callback puede encadenar la siguiente peticion (maquina de estados de sub_8276A808).
static void MicComplete(PPCContext& ctx, uint32_t req, uint32_t status)
{
    uint32_t cb = Load32(req + 16);
    PPCFunc* fn = cb ? PPC_LOOKUP_FUNC(g_base, cb) : nullptr;
    if (fn)
    {
        PPCContext c = ctx;
        c.r1.u64 = ctx.r1.u64 - 512;   // margen bajo la pila del llamador
        c.r3.u64 = req; c.r4.u64 = status;
        fn(c, g_base);
    }
    ctx.r3.u64 = fn ? 259 : status;   // STATUS_PENDING si ya se aviso por el callback
}
// Lecturas del microfono (tipo 7): +20 datos (n paquetes seguidos de bytes/ms), +24 u32 n. Se completan a ritmo real (n ms cada una)
// desde un hilo propio llamando al callback (sub_8276ADE8) con (peticion, estado, estados u32[n], longitudes u16[n]).
void MicCaptureStart();                      // mic_capture.cpp
void MicCaptureRead(int16_t* out, size_t n);
static std::atomic<bool> g_micOpen{ false };
static std::mutex g_micMutex;
static std::deque<uint32_t> g_micReads;
static std::atomic<bool> g_micThread{ false };
static void MicThread()
{
    auto* c = new PPCContext{};
    InitGuestThread(*c, 0x10000);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    uint32_t scratch = GuestAllocKernel(128, 16);   // estados u32[16] + longitudes u16[16]
    LARGE_INTEGER freq, now; QueryPerformanceFrequency(&freq);
    double nextDue = 0;
    std::vector<int16_t> pcm;
    timeBeginPeriod(1);
    uint64_t delivered = 0;
    for (;;)
    {
        uint32_t req = 0;
        { std::lock_guard l(g_micMutex); if (!g_micReads.empty()) req = g_micReads.front(); }
        QueryPerformanceCounter(&now); double t = now.QuadPart * 1000.0 / freq.QuadPart;
        if (!req || t < nextDue) { Sleep(1); continue; }
        { std::lock_guard l(g_micMutex); g_micReads.pop_front(); }
        uint32_t n = std::clamp<uint32_t>(Load32(req + 24), 1, 16), data = Load32(req + 20);
        uint32_t rctx = Load32(req + 12), rec = rctx ? Load32(rctx + 40) : 0;
        uint32_t bpp = rec ? Load32(rec + 8) : 96; if (bpp == 0 || bpp > 1024) bpp = 96;   // bytes por paquete (1 ms)
        pcm.resize(size_t(n) * bpp / 2);
        MicCaptureRead(pcm.data(), pcm.size());
        if (data) memcpy(g_base + data, pcm.data(), pcm.size() * 2);   // PCM s16 little endian, como USB
        for (uint32_t i = 0; i < n; i++) { Store32(scratch + i * 4, 0); Store16(scratch + 64 + i * 2, (uint16_t)bpp); }
        uint32_t cb = Load32(req + 16);
        if (PPCFunc* fn = cb ? PPC_LOOKUP_FUNC(g_base, cb) : nullptr)
        {
            uint64_t r1 = c->r1.u64, r13 = c->r13.u64;
            c->r3.u64 = req; c->r4.u64 = 0; c->r5.u64 = scratch; c->r6.u64 = scratch + 64;
            fn(*c, g_base);
            c->r1.u64 = r1; c->r13.u64 = r13;
        }
        nextDue = std::max(nextDue, t - 50.0) + n;   // n paquetes de 1 ms
        if (++delivered == 1 || delivered % 2500 == 0) LOG("[mic] lecturas entregadas: %llu (%u paquetes de %u bytes)", (unsigned long long)delivered, n, bpp);
    }
}
static void MicQueueRead(uint32_t req)
{
    { std::lock_guard l(g_micMutex); g_micReads.push_back(req); }
    bool expected = false;
    if (g_micThread.compare_exchange_strong(expected, true)) std::thread(MicThread).detach();
}
// MicDeviceRequest(peticion): +0 u16 tipo, +2 u16 puerto, +4 u32 resultado, +12 contexto, +16 callback(peticion, estado),
// +20 buffer, +24 u16 longitud, +28 paquete de setup USB (bmRequestType, bRequest, wValue, wIndex, wLength)
GUEST_IMPL(MicDeviceRequest)
{
    uint32_t r = ctx.r3.u32;
    uint16_t type = Load16(r), port = Load16(r + 2);
    static int logged = 0, loggedQuery = 0, loggedRead = 0;
    if (type == 0 ? loggedQuery++ < 3 : type == 7 ? loggedRead++ < 4 : logged++ < 60)
    {
        char hex[3 * 64 + 1] = {}; for (int i = 0; i < 64; i++) snprintf(hex + i * 3, 4, "%02X ", g_base[r + i]);
        LOG("[mic] MicDeviceRequest(%08X) tipo %u puerto %u lr=%08X | %s", r, type, port, (uint32_t)ctx.lr, hex);
    }
    if (!MicEnabled() || port != 1) { ctx.r3.u64 = 0xC000009D; return; }   // STATUS_DEVICE_NOT_CONNECTED
    switch (type)
    {
    // estado: 3 = conectado y configurado (el juego lo inicializa: sub_82438660), 5 = abierto y transmitiendo (se puede leer)
    case 0: Store32(r + 4, g_micOpen ? 5 : 3); ctx.r3.u64 = 0; return;
    case 8: Store32(r + 20, 0x140); MicComplete(ctx, r, 0); return;   // frecuencias admitidas: 32 kHz (0x40) y 48 kHz (0x100)
    case 1:   // abrir a la frecuencia elegida (sub_8276B6A0); el callback sigue con la negociacion de volumen
        g_micOpen = true; MicCaptureStart();
        LOG("[mic] microfono abierto por el juego");
        MicComplete(ctx, r, 0); return;
    case 7: MicQueueRead(r); ctx.r3.u64 = 259; return;   // lectura de n paquetes de 1 ms: la completa el hilo del microfono
    case 4: case 3: case 5: case 6: case 9:
        MicComplete(ctx, r, 0); return;
    case 2:
    {   // transferencia de control USB (clase audio): volumen del microfono
        uint8_t bmType = g_base[r + 28], bReq = g_base[r + 29];
        uint32_t buf = Load32(r + 20); uint16_t len = Load16(r + 24);
        if ((bmType & 0x80) && buf && len >= 2)
        {   // GET_CUR 0x81 / GET_MIN 0x82 / GET_MAX 0x83 / GET_RES 0x84, en 1/256 dB (little endian, como USB)
            static int16_t cur = 0;
            int16_t v = bReq == 0x82 ? (int16_t)-0x0C00 : bReq == 0x83 ? (int16_t)0x0C00 : bReq == 0x84 ? (int16_t)0x0100 : cur;
            g_base[buf] = (uint8_t)(v & 0xFF); g_base[buf + 1] = (uint8_t)(v >> 8);
        }
        MicComplete(ctx, r, 0);
        return;
    }
    default: ctx.r3.u64 = 0xC0000001; return;           // aun sin implementar (STATUS_UNSUCCESSFUL)
    }
}

// ---- mandos ----
// Sesion 20: estado comun de botones = teclado + guion (GH6_INPUTSCRIPT) + GH6_AUTOSTART.
// Teclado: flechas = cruceta (arriba/abajo = puas de la guitarra), Enter = START, Retroceso = BACK, Espacio = A, Esc = B, X, Y,
//          1-5 = trastes verde/rojo/amarillo/azul/naranja (A, B, Y, X, LB), Q/E = gatillos.
// Guion: archivo de texto con lineas "<segundos> <BOTON>[+BOTON...] [ms]" o "<segundos> DUMP" (captura shot_<segundos>.bmp).
// Botones: UP DOWN LEFT RIGHT START BACK A B X Y LB RB GREEN RED YELLOW BLUE ORANGE. Los tiempos cuentan desde el arranque del proceso.
extern std::atomic<int> g_dumpRequest;   // gpu_window.cpp
extern std::atomic<int> g_dumpSeq;       // gpu_window.cpp: frames seguidos que quedan por volcar
namespace
{
const DWORD g_processStart = GetTickCount();
struct ScriptStep { DWORD ms; uint16_t mask; DWORD hold; bool dump; bool fired = false; int player = 0; bool inst = false; };
std::vector<ScriptStep> LoadInputScript()
{
    std::vector<ScriptStep> v;
    const char* path = getenv("GH6_INPUTSCRIPT");
    if (!path) return v;
    FILE* f = fopen(path, "r");
    if (!f) { LOG("[input] no se pudo abrir el guion %s", path); return v; }
    static const struct { const char* n; uint16_t m; } names[] = {
        { "UP", 0x0001 }, { "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 }, { "START", 0x0010 }, { "BACK", 0x0020 },
        { "LB", 0x0100 }, { "RB", 0x0200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
        { "GREEN", 0x1000 }, { "RED", 0x2000 }, { "YELLOW", 0x8000 }, { "BLUE", 0x4000 }, { "ORANGE", 0x0100 } };
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        float secs; char btn[128] = {}; unsigned hold = 150;
        if (line[0] == '#' || sscanf(line, "%f %127s %u", &secs, btn, &hold) < 2) continue;
        ScriptStep s{ (DWORD)(secs * 1000), 0, hold, false };
        if (!strcmp(btn, "DUMP")) s.dump = true;
        else if (!strcmp(btn, "DUMP8")) { s.dump = true; s.hold = 0xFFFF; }   // 8 frames seguidos (sesion 23)
        else
        {   // sesion 24: prefijo "P2:" = jugador 2 (por defecto jugador 1)
            char* b = btn;
            if ((b[0] == 'P' || b[0] == 'p') && b[1] >= '1' && b[1] <= '4' && b[2] == ':') { s.player = b[1] - '1'; b += 3; }
            for (char* tok = strtok(b, "+"); tok; tok = strtok(nullptr, "+"))
            {
                if (!strcmp(tok, "INST")) s.inst = true;   // cambia al siguiente instrumento (como F6)
                for (auto& n : names) if (!strcmp(tok, n.n)) s.mask |= n.m;
            }
        }
        v.push_back(s);
    }
    fclose(f);
    LOG("[input] guion %s: %zu pasos", path, v.size());
    return v;
}
// ---- mandos reales por XInput (sesion 23; sesion 24: hasta 4 jugadores) ----
// El esquema de bits de XInput es el mismo que el de la 360; una guitarra de Xbox 360 en PC llega con su subtipo (6/7/8...)
// y la palanca de tremolo/inclinacion en el stick derecho, igual que en la consola.
constexpr int kPlayers = 4;
struct PadState { bool connected = false; XINPUT_STATE st{}; BYTE subtype = 0; };
PadState PollPad(int idx)
{
    static PadState s[4];
    static DWORD lastTry[4] = {};
    static bool disabled = getenv("GH6_NOXINPUT") != nullptr;
    if (disabled || idx < 0 || idx > 3) return PadState{};
    PadState& p = s[idx];
    DWORD now = GetTickCount();
    if (!p.connected && lastTry[idx] && now - lastTry[idx] < 2000) return p;   // sin mando: reintentar cada 2 s (la consulta es lenta)
    lastTry[idx] = now;
    bool was = p.connected;
    p.connected = XInputGetState(idx, &p.st) == ERROR_SUCCESS;
    if (p.connected && !was)
    {
        XINPUT_CAPABILITIES caps{};
        if (XInputGetCapabilities(idx, 0, &caps) == ERROR_SUCCESS) p.subtype = caps.SubType;
        LOG("[input] mando XInput %d conectado (subtipo %u)", idx + 1, p.subtype);
    }
    else if (!p.connected && was) LOG("[input] mando XInput %d desconectado", idx + 1);
    return p;
}
// ---- asignacion de dispositivos a jugadores (sesion 24) ----
// jugadorN_xinput = mando XInput (1-4, 0 = ninguno; por defecto N), mando_ps_jugador / teclado_jugador = jugador (por defecto 1)
static int EnvInt(const char* k, int def) { const char* v = getenv(k); return v && *v ? atoi(v) : def; }
static int XInputSlot(int u) { char k[32]; snprintf(k, sizeof(k), "GH6_JUGADOR%d_XINPUT", u + 1); static int c[4] = { -2, -2, -2, -2 }; if (c[u] == -2) c[u] = EnvInt(k, u + 1) - 1; return c[u]; }
static int PsPlayer() { static int v = std::clamp(EnvInt("GH6_MANDO_PS_JUGADOR", 1), 1, 4) - 1; return v; }
static int KeyboardPlayer() { static int v = std::clamp(EnvInt("GH6_TECLADO_JUGADOR", 1), 1, 4) - 1; return v; }
// ---- mando normal como guitarra (sesion 24, v1.0) ----
// El juego espera una guitarra (subtipo 6). Un mando de Xbox o PlayStation se presenta como guitarra y cada funcion se asigna
// desde gh6.ini (mando_verde=A, mando_rasgueo_abajo=DOWN|LS_DOWN, mando_palanca=RT...). mando_modo=mando lo deja como mando normal.
// Una guitarra real (XInput subtipo 6-8) pasa sin cambios.
enum : uint32_t { SRC_LT = 1u << 16, SRC_RT = 1u << 17, SRC_LS_UP = 1u << 18, SRC_LS_DOWN = 1u << 19, SRC_LS_LEFT = 1u << 20,
                  SRC_LS_RIGHT = 1u << 21, SRC_RS_UP = 1u << 22, SRC_RS_DOWN = 1u << 23, SRC_RS_LEFT = 1u << 24, SRC_RS_RIGHT = 1u << 25 };
static uint32_t ParseSources(const char* s)
{
    static const struct { const char* n; uint32_t m; } names[] = {
        { "UP", 0x0001 }, { "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 }, { "START", 0x0010 }, { "BACK", 0x0020 },
        { "LS", 0x0040 }, { "RS", 0x0080 }, { "LB", 0x0100 }, { "RB", 0x0200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
        { "LT", SRC_LT }, { "RT", SRC_RT }, { "LS_UP", SRC_LS_UP }, { "LS_DOWN", SRC_LS_DOWN }, { "LS_LEFT", SRC_LS_LEFT }, { "LS_RIGHT", SRC_LS_RIGHT },
        { "RS_UP", SRC_RS_UP }, { "RS_DOWN", SRC_RS_DOWN }, { "RS_LEFT", SRC_RS_LEFT }, { "RS_RIGHT", SRC_RS_RIGHT },
        // nombres de PlayStation
        { "CRUZ", 0x1000 }, { "CIRCULO", 0x2000 }, { "CUADRADO", 0x4000 }, { "TRIANGULO", 0x8000 }, { "L1", 0x0100 }, { "R1", 0x0200 },
        { "L2", SRC_LT }, { "R2", SRC_RT }, { "OPTIONS", 0x0010 }, { "CREATE", 0x0020 }, { "SHARE", 0x0020 } };
    uint32_t m = 0; std::string v = s;
    for (auto& c : v) c = (char)toupper((unsigned char)c);
    size_t pos = 0;
    while (pos <= v.size())
    {
        size_t e = v.find_first_of("|+, ", pos); if (e == std::string::npos) e = v.size();
        std::string tok = v.substr(pos, e - pos); pos = e + 1;
        for (auto& n : names) if (tok == n.n) m |= n.m;
    }
    return m;
}
static uint32_t SourceBits(uint16_t b, uint8_t lt, uint8_t rt, const int16_t st[4])
{
    const int16_t dz = 16000;
    uint32_t s = b;
    if (lt > 100) s |= SRC_LT; if (rt > 100) s |= SRC_RT;
    if (st[1] > dz) s |= SRC_LS_UP; if (st[1] < -dz) s |= SRC_LS_DOWN; if (st[0] < -dz) s |= SRC_LS_LEFT; if (st[0] > dz) s |= SRC_LS_RIGHT;
    if (st[3] > dz) s |= SRC_RS_UP; if (st[3] < -dz) s |= SRC_RS_DOWN; if (st[2] < -dz) s |= SRC_RS_LEFT; if (st[2] > dz) s |= SRC_RS_RIGHT;
    return s;
}
// Sesion 24: botones por jugador. jugadorN_mando_verde=... (GH6_JUGADORN_MANDO_VERDE) tiene prioridad sobre mando_verde=...
static const char* PlayerEnv(int u, const char* key)
{
    char k[96]; snprintf(k, sizeof(k), "GH6_JUGADOR%d_%s", u + 1, key + 4);   // key empieza por "GH6_"
    const char* v = getenv(k);
    return v && *v ? v : getenv(key);
}
struct GuitarMap { uint32_t src[11]; uint16_t dst[11]; uint32_t whammy, tilt; bool padMode; };
static const GuitarMap& GetGuitarMap(int u = 0)
{
    if (u < 0 || u >= kPlayers) u = 0;
    static GuitarMap maps[kPlayers]; static std::once_flag once[kPlayers];
    std::call_once(once[u], [u] { maps[u] = [u] {
        GuitarMap m{};
        static const struct { const char* key; const char* def; uint16_t dst; } f[11] = {
            { "GH6_MANDO_VERDE", "A", 0x1000 }, { "GH6_MANDO_ROJO", "B", 0x2000 }, { "GH6_MANDO_AMARILLO", "Y", 0x8000 },
            { "GH6_MANDO_AZUL", "X", 0x4000 }, { "GH6_MANDO_NARANJA", "LB", 0x0100 },
            { "GH6_MANDO_RASGUEO_ARRIBA", "UP|LS_UP", 0x0001 }, { "GH6_MANDO_RASGUEO_ABAJO", "DOWN|LS_DOWN", 0x0002 },
            { "GH6_MANDO_IZQUIERDA", "LEFT|LS_LEFT", 0x0004 }, { "GH6_MANDO_DERECHA", "RIGHT|LS_RIGHT", 0x0008 },
            { "GH6_MANDO_START", "START", 0x0010 }, { "GH6_MANDO_PODER_ESTRELLA", "BACK|RB", 0x0020 } };
        for (int i = 0; i < 11; i++) { const char* v = PlayerEnv(u, f[i].key); m.src[i] = ParseSources(v && *v ? v : f[i].def); m.dst[i] = f[i].dst; }
        const char* w = PlayerEnv(u, "GH6_MANDO_PALANCA"); m.whammy = ParseSources(w && *w ? w : "RT");
        // Sesion 24: inclinacion (poder estrella al levantar el mastil) para guitarras que llegan como mando normal: en la guitarra
        // de 360 va en el eje Y del stick derecho. mando_inclinacion=<botones> (vacio = ninguno)
        const char* ti = PlayerEnv(u, "GH6_MANDO_INCLINACION"); m.tilt = ti && *ti ? ParseSources(ti) : 0;
        const char* mode = PlayerEnv(u, "GH6_MANDO_MODO"); m.padMode = mode && (!_stricmp(mode, "mando") || !_stricmp(mode, "pad"));
        LOG("[input] jugador %d: mando como %s (verde %X rojo %X amarillo %X azul %X naranja %X rasgueo %X/%X estrella %X palanca %X inclinacion %X)", u + 1,
            m.padMode ? "mando normal" : "guitarra", m.src[0], m.src[1], m.src[2], m.src[3], m.src[4], m.src[5], m.src[6], m.src[10], m.whammy, m.tilt);
        return m; }(); });
    return maps[u];
}
// Estado crudo de un mando (botones XInput + gatillos + sticks) -> estado de guitarra
static void PadToGuitar(int u, uint16_t b, uint8_t lt, uint8_t rt, const int16_t st[4], uint16_t& ob, int16_t& whammy, int16_t& tiltY)
{
    const GuitarMap& g = GetGuitarMap(u);
    uint32_t s = SourceBits(b, lt, rt, st);
    for (int i = 0; i < 11; i++) if (s & g.src[i]) ob |= g.dst[i];
    // Palanca (whammy) analogica: eje X del stick derecho de la guitarra, -32768 en reposo
    int v = 0;
    if (g.whammy & SRC_RT) v = std::max(v, (int)rt);
    if (g.whammy & SRC_LT) v = std::max(v, (int)lt);
    if (g.whammy & (0x0080 | SRC_RS_UP | SRC_RS_DOWN | SRC_RS_LEFT | SRC_RS_RIGHT))   // "RS" = stick derecho
        v = std::max(v, std::min(255, (std::max(abs(st[2]), abs(st[3])) >> 7)));
    whammy = (int16_t)std::clamp(-32768 + v * 257, -32768, 32767);
    tiltY = (g.tilt && (s & g.tilt)) ? 32767 : 0;
}
static bool RealGuitar(const PadState& p) { return p.connected && ((p.subtype >= 6 && p.subtype <= 8) || p.subtype == 11); }
// ---- instrumento de cada jugador (sesion 24) ----
// 0 = guitarra (subtipo 6), 1 = bateria (subtipo 8), 2 = voz (mando normal, subtipo 1: el juego le asigna el microfono).
// jugadorN_instrumento en gh6.ini (jugador 1: tambien mando_instrumento). F6 recorre los del jugador del teclado.
static std::atomic<int> g_inst[kPlayers] = { -1, -1, -1, -1 };
static int ParseInstrument(const char* v)
{
    if (!v || !*v) return -1;
    if (!_stricmp(v, "bateria") || !_stricmp(v, "drums")) return 1;
    if (!_stricmp(v, "voz") || !_stricmp(v, "vocals")) return 2;
    return 0;
}
static int Instrument(int u)
{
    if (u < 0 || u >= kPlayers) u = 0;
    if (g_inst[u] < 0)
    {
        char k[40]; snprintf(k, sizeof(k), "GH6_JUGADOR%d_INSTRUMENTO", u + 1);
        int v = ParseInstrument(getenv(k));
        if (v < 0 && u == 0) v = ParseInstrument(getenv("GH6_MANDO_INSTRUMENTO"));
        g_inst[u] = v < 0 ? 0 : v;
    }
    return g_inst[u];
}
// El juego solo lee el tipo de mando (subtipo) cuando se conecta. Al cambiar de instrumento el mando de ese jugador se
// "desconecta" 700 ms y se avisa con XN_SYS_INPUTDEVICESCHANGED, para que lo vuelva a detectar con el instrumento nuevo
// (en el lobby: sal del hueco con Atras si hace falta y vuelve a entrar). GH6_NORECONECTAR=1 lo desactiva.
static std::atomic<DWORD> g_reconnectAt[kPlayers] = {};
static std::atomic<bool> g_reconnectPending[kPlayers] = {};
static void SetInstrument(int u, int v, const char* why)
{
    if (u < 0 || u >= kPlayers) return;
    g_inst[u] = v;
    static const char* nm[] = { "guitarra", "bateria", "voz" };
    LOG("[input] jugador %d: %s (%s)", u + 1, nm[v], why);
    static bool off = getenv("GH6_NORECONECTAR") != nullptr;
    if (off) return;
    g_reconnectAt[u] = GetTickCount() | 1; g_reconnectPending[u] = true;
    BroadcastNotification(0x12, 0);   // XN_SYS_INPUTDEVICESCHANGED
}
static bool Reconnecting(int u)
{
    DWORD t = g_reconnectAt[u];
    if (!t) return false;
    if (GetTickCount() - t < 700) return true;
    if (g_reconnectPending[u].exchange(false)) { BroadcastNotification(0x12, 0); LOG("[input] jugador %d reconectado", u + 1); }
    return false;
}
static bool DrumMode(int u) { return Instrument(u) == 1; }
static bool VoiceMode(int u) { return Instrument(u) == 2; }
// ---- bateria (sesion 24) ----
// Bateria de Guitar Hero de 360 por XInput (subtipo 8): verde=A, rojo=B, amarillo=Y, azul=X, platillo naranja=RB, bombo=LB.
// La fuerza de cada golpe va en los sticks: LY = rojo (byte alto) | verde (bajo), RX = azul | amarillo, RY = bombo | naranja.
struct DrumMap { uint32_t src[10]; uint16_t dst[10]; };
static const DrumMap& GetDrumMap(int u)
{
    if (u < 0 || u >= kPlayers) u = 0;
    static DrumMap maps[kPlayers]; static std::once_flag once[kPlayers];
    std::call_once(once[u], [u] { maps[u] = [u] {
        DrumMap m{};
        static const struct { const char* key; const char* def; uint16_t dst; } f[10] = {
            { "GH6_BATERIA_VERDE", "A", 0x1000 }, { "GH6_BATERIA_ROJO", "B", 0x2000 }, { "GH6_BATERIA_AMARILLO", "Y", 0x8000 },
            { "GH6_BATERIA_AZUL", "X", 0x4000 }, { "GH6_BATERIA_NARANJA", "RB", 0x0200 }, { "GH6_BATERIA_BOMBO", "LB|LT|RT", 0x0100 },
            { "GH6_BATERIA_ARRIBA", "UP|LS_UP", 0x0001 }, { "GH6_BATERIA_ABAJO", "DOWN|LS_DOWN", 0x0002 },
            { "GH6_BATERIA_START", "START", 0x0010 }, { "GH6_BATERIA_BACK", "BACK", 0x0020 } };
        for (int i = 0; i < 10; i++) { const char* v = PlayerEnv(u, f[i].key); m.src[i] = ParseSources(v && *v ? v : f[i].def); m.dst[i] = f[i].dst; }
        return m; }(); });
    return maps[u];
}
static void PadToDrums(int u, uint16_t b, uint8_t lt, uint8_t rt, const int16_t st[4], uint16_t& ob)
{
    const DrumMap& g = GetDrumMap(u);
    uint32_t s = SourceBits(b, lt, rt, st);
    for (int i = 0; i < 10; i++) if (s & g.src[i]) ob |= g.dst[i];
}
// Fuerza de los golpes a partir de los pads pulsados (golpe fuerte fijo)
static void DrumVelocities(uint16_t b, int16_t st[4])
{
    const uint8_t v = 0x7F;
    auto pk = [](bool hi, bool lo, uint8_t v) { return (int16_t)(((hi ? v : 0) << 8) | (lo ? v : 0)); };
    st[0] = 0;
    st[1] = pk(b & 0x2000, b & 0x1000, v);   // rojo | verde
    st[2] = pk(b & 0x4000, b & 0x8000, v);   // azul | amarillo
    st[3] = pk(b & 0x0100, b & 0x0200, v);   // bombo | naranja
}
// ---- estado de cada jugador ----
static std::vector<ScriptStep>& Script() { static std::vector<ScriptStep> s = LoadInputScript(); return s; }
static bool ScriptUsesPlayer(int u) { for (auto& s : Script()) if (!s.dump && s.player == u) return true; return false; }
// Tecla de juego pulsada en la ventana del juego (para seguir con teclado tras perder el mando)
static bool KeyboardActive()
{
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    static const int keys[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE, VK_BACK, '1', '2', '3', '4', '5' };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}
// GH6_DESCONECTAR=<jugador>:<desde s>:<hasta s> simula que el mando de ese jugador se desconecta (pruebas)
static int g_forcedPlayer = 0; static DWORD g_forced0 = 0, g_forced1 = 0;
static bool ForcedDisconnect(int u)
{
    int& fp = g_forcedPlayer; DWORD& f0 = g_forced0; DWORD& f1 = g_forced1;
    static bool init = [] { if (const char* v = getenv("GH6_DESCONECTAR")) { unsigned a, b, c; if (sscanf(v, "%u:%u:%u", &a, &b, &c) == 3) { fp = (int)a; f0 = b * 1000; f1 = c * 1000; } } return true; }();
    (void)init;
    DWORD t = GetTickCount() - g_processStart;
    return fp == u + 1 && t >= f0 && t < f1;
}
static bool PlayerConnected(int u)
{
    if (u < 0 || u >= kPlayers) return false;
    if (Reconnecting(u)) return false;
    uint16_t pb; uint8_t plt, prt; int16_t ps[4];
    bool forced = ForcedDisconnect(u);
    bool simulated = g_forcedPlayer == u + 1;   // con GH6_DESCONECTAR el mando simulado existe fuera de la ventana
    bool pad = !forced && (simulated || PollPad(XInputSlot(u)).connected || (u == PsPlayer() && PsPadGetState(pb, plt, prt, ps)));
    if (u == KeyboardPlayer())
    {
        // Sesion 24: el jugador del teclado siempre esta, salvo si tenia mando y se le desconecta: entonces el juego lo ve
        // desconectado (muestra su aviso y pausa) hasta que vuelva el mando o se pulse una tecla para seguir con el teclado.
        static bool had[kPlayers], lost[kPlayers];
        if (pad) { had[u] = true; if (lost[u]) { lost[u] = false; LOG("[input] jugador %d: mando reconectado", u + 1); } return true; }
        if (had[u] && !lost[u]) { lost[u] = true; LOG("[input] jugador %d: mando desconectado (el juego avisa; una tecla sigue con el teclado)", u + 1); }
        if (lost[u])
        {
            if (!forced && KeyboardActive()) { lost[u] = false; had[u] = false; LOG("[input] jugador %d: sigue con el teclado", u + 1); return true; }
            return false;
        }
        return true;
    }
    if (pad) return true;
    if (forced) return false;
    return ScriptUsesPlayer(u);
}
static bool PlayerRealGuitar(int u) { return RealGuitar(PollPad(XInputSlot(u))); }
struct PlayerSticks { bool valid = false; int16_t v[4] = {}; };
static PlayerSticks g_sticks[kPlayers];
uint16_t CurrentButtons(int u, uint8_t* lt = nullptr, uint8_t* rt = nullptr)
{
    if (u < 0 || u >= kPlayers) u = 0;
    uint16_t buttons = 0;
    {
        const GuitarMap& gm = GetGuitarMap(u);
        bool sticksSet = false;
        auto addRaw = [&](uint16_t b, uint8_t l, uint8_t r, const int16_t st[4], bool passThrough) {
            if (passThrough)
            {
                buttons |= b;
                if (lt) *lt = std::max<uint8_t>(*lt, l);
                if (rt) *rt = std::max<uint8_t>(*rt, r);
                if (!sticksSet) { memcpy(g_sticks[u].v, st, sizeof(g_sticks[u].v)); sticksSet = true; }
            }
            else if (DrumMode(u))
            {
                uint16_t ob = 0; PadToDrums(u, b, l, r, st, ob);
                buttons |= ob;   // las fuerzas se calculan al final (DrumVelocities)
            }
            else
            {
                uint16_t ob = 0; int16_t wh = -32768;
                int16_t tilt = 0;
                PadToGuitar(u, b, l, r, st, ob, wh, tilt);
                buttons |= ob;
                if (!sticksSet) { g_sticks[u].v[0] = g_sticks[u].v[1] = 0; g_sticks[u].v[2] = wh; g_sticks[u].v[3] = tilt; sticksSet = true; }
            }
        };
        PadState pad = PollPad(XInputSlot(u));
        if (pad.connected)
        {
            int16_t st[4] = { pad.st.Gamepad.sThumbLX, pad.st.Gamepad.sThumbLY, pad.st.Gamepad.sThumbRX, pad.st.Gamepad.sThumbRY };
            addRaw(pad.st.Gamepad.wButtons, pad.st.Gamepad.bLeftTrigger, pad.st.Gamepad.bRightTrigger, st, gm.padMode || VoiceMode(u) || RealGuitar(pad));
        }
        // Mando de PlayStation por HID (dualsense.cpp), en el jugador mando_ps_jugador
        uint16_t pb; uint8_t plt, prt; int16_t ps[4];
        if (u == PsPlayer() && PsPadGetState(pb, plt, prt, ps)) addRaw(pb, plt, prt, ps, gm.padMode || VoiceMode(u));
        g_sticks[u].valid = sticksSet;
    }
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (u == KeyboardPlayer() && pid == GetCurrentProcessId())
    {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_UP)) buttons |= 0x0001; if (down(VK_DOWN)) buttons |= 0x0002;
        if (down(VK_LEFT)) buttons |= 0x0004; if (down(VK_RIGHT)) buttons |= 0x0008;
        if (down(VK_RETURN)) buttons |= 0x0010; if (down(VK_BACK)) buttons |= 0x0020;
        if (down(VK_SPACE)) buttons |= 0x1000; if (down(VK_ESCAPE)) buttons |= 0x2000;
        if (down('X')) buttons |= 0x4000; if (down('Y')) buttons |= 0x8000;
        if (down('1')) buttons |= 0x1000; if (down('2')) buttons |= 0x2000; if (down('3')) buttons |= 0x8000;
        if (down('4')) buttons |= 0x4000;
        // Sesion 24: en bateria, 5 = platillo naranja (RB) y K / B = bombo (LB); en guitarra, 5 = traste naranja (LB)
        if (DrumMode(u)) { if (down('5')) buttons |= 0x0200; if (down('K') || down('B')) buttons |= 0x0100; }
        else if (down('5')) buttons |= 0x0100;
        {   // F6: recorre guitarra -> bateria -> voz para el jugador del teclado
            static bool f6 = false; bool now6 = down(VK_F6);
            if (now6 && !f6) SetInstrument(u, (Instrument(u) + 1) % 3, "F6");
            f6 = now6;
        }
        if (lt && down('Q')) *lt = 255; if (rt && down('E')) *rt = 255;
    }
    static bool autoStart = getenv("GH6_AUTOSTART") != nullptr;
    if (autoStart && u == 0) { DWORD t = GetTickCount() / 250 % 24; if (t == 0) buttons |= 0x0010; else if (t == 12) buttons |= 0x1000; }
    static std::mutex m;
    auto& steps = Script();
    if (!steps.empty())
    {
        std::lock_guard l(m);
        DWORD now = GetTickCount() - g_processStart;
        for (auto& s : steps)
        {
            if (now < s.ms) continue;
            if (s.dump) { if (!s.fired && u == 0) { s.fired = true; if (s.hold == 0xFFFF) g_dumpSeq = 8; else g_dumpRequest = (int)(s.ms / 1000); } continue; }
            if (s.player != u) continue;
            if (s.inst && !s.fired) SetInstrument(u, (Instrument(u) + 1) % 3, "guion");
            if (now < s.ms + s.hold) buttons |= s.mask;
            if (!s.fired) { s.fired = true; LOG("[input] guion t=%.1f s: jugador %d botones 0x%04X", s.ms / 1000.0, u + 1, s.mask); }
        }
    }
    return buttons;
}
} // namespace
// Para el launcher (launcher_ui.cpp): mismos nombres de botones y misma lectura de gatillos/sticks que el juego
uint32_t InputParseSources(const char* s) { return ParseSources(s); }
uint32_t InputSourceBits(uint16_t b, uint8_t lt, uint8_t rt, const int16_t st[4]) { return SourceBits(b, lt, rt, st); }
// Jugador al que se asocia el microfono: el primero con instrumento voz; si no hay, GH6_MIC_USER (por defecto 0)
int MicPlayer()
{
    for (int u = 0; u < kPlayers; u++) if (VoiceMode(u) && PlayerConnected(u)) return u;
    static const int def = EnvInt("GH6_MIC_USER", 0);
    return def;
}
bool InputPlayerConnected(int u) { return PlayerConnected(u); }
// Indice de usuario de las funciones XamInput*: 0-3; 0xFF = "cualquiera" (se trata como el jugador 1)
static int InputUser(uint32_t user) { return user == 0xFF ? 0 : (user < 4 ? (int)user : -1); }
// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*) {type u8, sub_type u8, flags u16, gamepad[12], vibration[4]}
GUEST_IMPL(XamInputGetCapabilities)
{
    int u = InputUser(ctx.r3.u32);
    static std::atomic<uint64_t> calls{ 0 };
    uint64_t c = ++calls;
    if (c == 1 || c % 4000 == 0) LOG("[input] XamInputGetCapabilities llamada %llu (usuario %u, flags %u)", (unsigned long long)c, ctx.r3.u32, ctx.r4.u32);
    if (u < 0 || !PlayerConnected(u)) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    uint32_t p = ctx.r5.u32;
    memset(g_base + p, 0, 0x14);
    // subtipo 6 = guitarra, 8 = bateria, 1 = mando normal (voz o mando_modo=mando), o el de una guitarra/bateria real
    static bool pad = getenv("GH6_PAD") != nullptr;
    PadState real = PollPad(XInputSlot(u));
    BYTE sub = DrumMode(u) ? 8 : 6;
    if (RealGuitar(real)) sub = real.subtype == 8 ? 8 : 6;   // guitarra alternativa / bajo -> guitarra
    else if (pad || GetGuitarMap(u).padMode || VoiceMode(u)) sub = 1;
    g_base[p] = 1; g_base[p + 1] = sub;
    { static int lastSub[kPlayers] = { -1, -1, -1, -1 }; if (lastSub[u] != sub) { lastSub[u] = sub; LOG("[input] jugador %d: subtipo %u", u + 1, sub); } }
    Store16(p + 4, 0xFFFF); g_base[p + 6] = 0xFF; g_base[p + 7] = 0xFF;
    Store16(p + 8, 0xFFC0); Store16(p + 10, 0xFFC0); Store16(p + 12, 0xFFC0); Store16(p + 14, 0xFFC0);
    Store16(p + 16, 0xFFFF); Store16(p + 18, 0xFFFF);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamInputGetState(user, flags, X_INPUT_STATE*) {packet u32, buttons u16, lt u8, rt u8, lx, ly, rx, ry s16}
GUEST_IMPL(XamInputGetState)
{
    int u = InputUser(ctx.r3.u32);
    static std::atomic<uint64_t> calls{ 0 };
    uint64_t c = ++calls;
    if (c == 1 || c % 8000 == 0) LOG("[input] XamInputGetState llamada %llu (usuario %u, flags %u)", (unsigned long long)c, ctx.r3.u32, ctx.r4.u32);
    if (u < 0 || !PlayerConnected(u)) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    uint32_t p = ctx.r5.u32;
    if (!p) { ctx.r3.u64 = X_ERROR_SUCCESS; return; }
    uint8_t lt = 0, rt = 0;
    uint16_t buttons = CurrentButtons(u, &lt, &rt);
    static uint32_t packet[kPlayers] = {}; static uint16_t lastButtons[kPlayers] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    if (buttons != lastButtons[u])
    {
        // GH6_LATLOG=1: muestra de audio en la que el juego ve cada pulsacion (con GH6_WAVDUMP se mide la latencia del sonido)
        static bool latLog = getenv("GH6_LATLOG") != nullptr;
        uint16_t pressed = buttons & ~lastButtons[u];
        if (latLog && pressed && lastButtons[u] != 0xFFFF) { extern uint64_t AudioSubmitted(); LOG("[lat] pulsacion 0x%04X en la muestra %llu", pressed, (unsigned long long)AudioSubmitted()); }
        packet[u]++; lastButtons[u] = buttons;
    }
    memset(g_base + p, 0, 0x10);
    Store32(p, packet[u]); Store16(p + 4, buttons); g_base[p + 6] = lt; g_base[p + 7] = rt;
    // Sticks (en la guitarra: palanca = stick derecho X, inclinacion = stick derecho Y). Con un mando normal convertido, la palanca
    // sale del control elegido en mando_palanca (CurrentButtons rellena g_sticks).
    if (DrumMode(u) && !PlayerRealGuitar(u) && !GetGuitarMap(u).padMode)
    {   // bateria emulada: fuerza de golpe en los sticks segun los pads pulsados
        int16_t st[4]; DrumVelocities(buttons, st);
        Store16(p + 8, (uint16_t)st[0]); Store16(p + 10, (uint16_t)st[1]); Store16(p + 12, (uint16_t)st[2]); Store16(p + 14, (uint16_t)st[3]);
    }
    else if (g_sticks[u].valid)
    {
        const int16_t* sv = g_sticks[u].v;
        Store16(p + 8, (uint16_t)sv[0]); Store16(p + 10, (uint16_t)sv[1]); Store16(p + 12, (uint16_t)sv[2]); Store16(p + 14, (uint16_t)sv[3]);
        static int16_t lastSticks[kPlayers][4] = {};
        if (memcmp(lastSticks[u], sv, 8)) { memcpy(lastSticks[u], sv, 8); packet[u]++; Store32(p, packet[u]); }
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
GUEST_IMPL(XamInputSetState) { int u = InputUser(ctx.r3.u32); ctx.r3.u64 = (u >= 0 && PlayerConnected(u)) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED; }
// XamInputGetKeystrokeEx(user_index_ptr, flags, X_INPUT_KEYSTROKE*) {u16 vk, u16 unicode, u16 flags (1 down, 2 up), u8 user, u8 hid}
// Sesion 13: eventos generados comparando el estado de los botones. Sesion 24: por jugador (0xFF = de cualquier jugador).
GUEST_IMPL(XamInputGetKeystrokeEx)
{
    static const struct { uint16_t mask, vk; } map[] = {
        { 0x0010, 0x5814 }, { 0x1000, 0x5800 }, { 0x2000, 0x5801 }, { 0x4000, 0x5802 }, { 0x8000, 0x5803 },
        { 0x0001, 0x5810 }, { 0x0002, 0x5811 }, { 0x0004, 0x5812 }, { 0x0008, 0x5813 }, { 0x0020, 0x5815 },
        { 0x0100, 0x5804 }, { 0x0200, 0x5805 } };   // LB / RB (VK_PAD_LSHOULDER / RSHOULDER)
    static uint16_t last[kPlayers] = {};
    uint32_t userPtr = ctx.r3.u32, p = ctx.r5.u32;
    uint32_t user = userPtr ? Load32(userPtr) : 0;
    int first = 0, lastU = kPlayers - 1;
    if (user != 0xFF) { if (user >= kPlayers) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; } first = lastU = (int)user; }
    bool any = false;
    for (int u = first; u <= lastU; u++)
    {
        if (!PlayerConnected(u)) continue;
        any = true;
        uint16_t buttons = CurrentButtons(u);
        uint16_t changed = buttons ^ last[u];
        for (auto& m : map)
            if (changed & m.mask)
            {
                bool isDown = (buttons & m.mask) != 0;
                last[u] ^= m.mask;
                if (p) { memset(g_base + p, 0, 8); Store16(p, m.vk); Store16(p + 4, isDown ? 1 : 2); g_base[p + 6] = (uint8_t)u; }
                if (userPtr) Store32(userPtr, (uint32_t)u);
                ctx.r3.u64 = X_ERROR_SUCCESS;
                return;
            }
    }
    ctx.r3.u64 = any ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

// ---- audio ----
GUEST_IMPL(XAudioGetSpeakerConfig) { if (ctx.r3.u32) Store32(ctx.r3.u32, 0x00010001); ctx.r3.u64 = X_ERROR_SUCCESS; }
// ---- motor de audio (sesion 10): el "hardware" pide un frame de 256 muestras cada ~5,33 ms llamando al callback del juego ----
// Las muestras enviadas con XAudioSubmitRenderDriverFrame se descartan por ahora (la salida por WASAPI llega en la sesion de audio).
namespace
{
std::atomic<uint32_t> g_audioCb{ 0 }, g_audioArg{ 0 };
std::atomic<uint64_t> g_audioFrames{ 0 }, g_audioSubmits{ 0 };
std::atomic<bool> g_audioThread{ false };
}
size_t AudioQueuedFrames();     // audio_wasapi.cpp
void AudioSetTargetFrames(uint32_t f);
uint64_t AudioUnderruns();
bool AudioOutputActive();
namespace
{
void AudioPump()
{
    auto* actx = new PPCContext{};
    InitGuestThread(*actx, 0x10000);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // el "hardware" de audio no debe esperar a los demas hilos (sesion 23)
    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    timeBeginPeriod(1);
    static bool clocked = getenv("GH6_NOAUDIO") != nullptr;   // sin salida de audio: ritmo por reloj
    // Sesion 25 (desfase de audio): el juego genera siempre al ritmo del reloj del PC (n * 256 / 48000 s desde el arranque), igual que
    // el reloj de las notas; audio_wasapi.cpp adapta ese flujo a la tarjeta. Tras un paron se llama al juego seguido hasta ponerse al
    // dia (sin saltar audio: si se saltara, la musica quedaria atrasada para siempre); solo con mas de 5 s de retraso se reinicia.
    // GH6_AUDIOTARJETA=1 = ritmo marcado por la cola de la tarjeta (antes).
    static const bool cardClock = getenv("GH6_AUDIOTARJETA") != nullptr;
    for (uint64_t n = 1;; n++)
    {
        if (!clocked && !cardClock && AudioOutputActive())
        {
            static const size_t minMs = (size_t)std::clamp(getenv("GH6_AUDIOMS") ? atoi(getenv("GH6_AUDIOMS")) : 20, 5, 250);
            static const size_t maxMs = std::max(minMs, (size_t)std::clamp(getenv("GH6_AUDIOMAX") ? atoi(getenv("GH6_AUDIOMAX")) : 80, 5, 250));
            static const bool fixedQ = getenv("GH6_AUDIOFIJO") != nullptr;
            static size_t curMs = minMs; static uint64_t lastUnder = 0; static DWORD lastChange = GetTickCount();
            if (!fixedQ)
            {
                uint64_t u = AudioUnderruns(); DWORD nowT = GetTickCount();
                if (u != lastUnder)
                {
                    lastUnder = u;
                    if (curMs < maxMs && nowT - lastChange > 1000) { curMs = std::min(maxMs, curMs + 5); lastChange = nowT; LOG("[audio] corte de audio: cola %zu ms", curMs); }
                }
                // con el reloj del PC la cola baja mas despacio (20 s sin cortes, luego 1 ms cada 2 s): si no, los microcortes se repiten
                else if (curMs > minMs && nowT - lastChange > 20000) { curMs--; lastChange = nowT - 18000; }
            }
            AudioSetTargetFrames((uint32_t)(48 * curMs));
            LONGLONG target = t0.QuadPart + (LONGLONG)(n * 256 * freq.QuadPart / 48000);
            QueryPerformanceCounter(&now);
            if (now.QuadPart - target > 5 * freq.QuadPart)   // solo con parones enormes (depurador, suspension)
            { LOG("[audio] %lld ms de retraso: se reinicia el reloj de audio", (long long)((now.QuadPart - target) * 1000 / freq.QuadPart)); t0 = now; n = 1; }
            else for (;;) { QueryPerformanceCounter(&now); LONGLONG left = target - now.QuadPart; if (left <= 0) break; if (left > freq.QuadPart / 500) Sleep(1); else YieldProcessor(); }
        }
        else if (clocked || !AudioOutputActive())
        {
            // siguiente instante: n * 256 / 48000 s
            LONGLONG target = t0.QuadPart + (LONGLONG)(n * 256 * freq.QuadPart / 48000);
            for (;;) { QueryPerformanceCounter(&now); LONGLONG left = target - now.QuadPart; if (left <= 0) break; if (left > freq.QuadPart / 500) Sleep(1); else YieldProcessor(); }
        }
        else
        {
            // Sesion 21: ritmo marcado por la tarjeta de sonido; se genera mientras la cola tenga menos de ~50 ms
            // Sesion 24 (retraso): margen GH6_AUDIOMS (por defecto 40 ms). Antes 120 ms fijos (sesion 23: con 50 ms habia huecos,
            // cuando el juego iba a ~30 fps con el hilo de la GPU saturado).
            // Sesion 24: cola adaptativa. GH6_AUDIOMS = minimo (ms); si hay cortes (la cola se vacia: cargas, picos de CPU) sube 5 ms
            // hasta GH6_AUDIOMAX (por defecto 80; como mucho 1 vez por segundo) y, tras 2 s sin cortes, baja 1 ms cada 0,3 s. GH6_AUDIOFIJO=1 = fija.
            static const size_t minMs = (size_t)std::clamp(getenv("GH6_AUDIOMS") ? atoi(getenv("GH6_AUDIOMS")) : 20, 5, 250);
            static const size_t maxMs = std::max(minMs, (size_t)std::clamp(getenv("GH6_AUDIOMAX") ? atoi(getenv("GH6_AUDIOMAX")) : 80, 5, 250));
            static const bool fixedQ = getenv("GH6_AUDIOFIJO") != nullptr;
            static size_t curMs = minMs; static uint64_t lastUnder = 0; static DWORD lastChange = GetTickCount();
            if (!fixedQ)
            {
                uint64_t u = AudioUnderruns(); DWORD nowT = GetTickCount();
                if (u != lastUnder)
                {
                    lastUnder = u;
                    if (curMs < maxMs && nowT - lastChange > 1000) { curMs = std::min(maxMs, curMs + 5); lastChange = nowT; LOG("[audio] corte de audio: cola %zu ms", curMs); }
                }
                else if (curMs > minMs && nowT - lastChange > 2000) { curMs--; lastChange = nowT - 1700; }
            }
            const size_t queueTarget = 48 * curMs;
            while (AudioQueuedFrames() >= queueTarget) Sleep(1);
            QueryPerformanceCounter(&t0); n = 0;
        }
        uint32_t cb = g_audioCb.load();
        if (!cb) continue;
        PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, cb);
        if (!fn) continue;
        uint64_t r1 = actx->r1.u64, r13 = actx->r13.u64;
        uint64_t before = g_audioSubmits.load();
        actx->r3.u64 = g_audioArg.load();
        fn(*actx, g_base);
        actx->r1.u64 = r1; actx->r13.u64 = r13;
        if (g_audioSubmits.load() == before) Sleep(5);   // el juego no envio frame: no girar en vacio
        if (++g_audioFrames % 1875 == 0) LOG("[audio] %llu frames pedidos, %llu enviados por el juego", (unsigned long long)g_audioFrames.load(), (unsigned long long)g_audioSubmits.load());
    }
}
}
// XAudioRegisterRenderDriverClient(callback_ptr -> {callback, arg}, driver_ptr)
GUEST_IMPL(XAudioRegisterRenderDriverClient)
{
    uint32_t cb = ctx.r3.u32 ? Load32(ctx.r3.u32) : 0, arg = ctx.r3.u32 ? Load32(ctx.r3.u32 + 4) : 0;
    LOG("[audio] XAudioRegisterRenderDriverClient callback=0x%08X arg=0x%08X", cb, arg);
    // Como Xenia: el callback recibe un PUNTERO a una palabra de invitado que contiene el argumento
    uint32_t wrapped = GuestAllocKernel(4, 4);
    Store32(wrapped, arg);
    g_audioArg = wrapped; g_audioCb = cb;
    bool expected = false;
    if (g_audioThread.compare_exchange_strong(expected, true)) std::thread(AudioPump).detach();
    if (ctx.r4.u32) Store32(ctx.r4.u32, 0x41550000);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
GUEST_IMPL(XAudioUnregisterRenderDriverClient) { g_audioCb = 0; ctx.r3.u64 = X_ERROR_SUCCESS; }
// XAudioSubmitRenderDriverFrame(driver, samples_ptr): 256 muestras x 6 canales float BE -> WASAPI (audio_wasapi.cpp, sesion 21)
void AudioSubmitXenonFrame(const uint8_t* src);
GUEST_IMPL(XAudioSubmitRenderDriverFrame)
{
    g_audioSubmits++;
    if (ctx.r4.u32) AudioSubmitXenonFrame(g_base + ctx.r4.u32);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
GUEST_IMPL(XAudioOverrideSpeakerConfig) { ctx.r3.u64 = X_ERROR_SUCCESS; }
// XMACreateContext / XMAReleaseContext: en xma.cpp
uint64_t AudioFramesRequested() { return g_audioFrames.load(); }
uint64_t AudioFramesSubmitted() { return g_audioSubmits.load(); }
GUEST_IMPL(XAudioGetVoiceCategoryVolume)
{
    float v = 1.0f; uint32_t b; memcpy(&b, &v, 4);
    if (ctx.r4.u32) Store32(ctx.r4.u32, b);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- enumeraciones (contenido, partidas): de momento vacias ----
// XamCreateEnumeratorHandle(user, app_id, open_msg, close_msg, extra_size, item_count, flags, out_handle)
GUEST_IMPL(XamCreateEnumeratorHandle)
{
    auto o = std::make_shared<KObject>();
    o->guestPtr = GuestAllocKernel((ctx.r7.u32 ? ctx.r7.u32 : 4) + 0x40, 0x10);
    if (ctx.r10.u32) Store32(ctx.r10.u32, InsertHandle(o));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XamGetPrivateEnumStructureFromHandle(handle, out_ptr)
GUEST_IMPL(XamGetPrivateEnumStructureFromHandle)
{
    auto o = GetHandleObject(ctx.r3.u32);
    if (ctx.r4.u32) Store32(ctx.r4.u32, o ? o->guestPtr : 0);
    ctx.r3.u64 = o ? X_ERROR_SUCCESS : 6; // ERROR_INVALID_HANDLE
}
// XamEnumerate(handle, flags, buffer, buffer_size, items_returned_ptr, overlapped)
GUEST_IMPL(XamEnumerate)
{
    if (ctx.r7.u32) Store32(ctx.r7.u32, 0);
    if (auto e = std::dynamic_pointer_cast<ContentEnum>(GetHandleObject(ctx.r3.u32)))
    {
        uint32_t buf = ctx.r5.u32, cap = ctx.r6.u32 / e->itemSize, n = 0;
        LOG("[xam] XamEnumerate(handle %08X, buffer %u bytes, overlapped %08X): %zu/%zu elementos", ctx.r3.u32, ctx.r6.u32, ctx.r8.u32, e->next, e->items.size());
        while (e->next < e->items.size() && n < cap && n < e->perCall)
        {
            memcpy(g_base + buf + n * e->itemSize, e->items[e->next].data(), e->itemSize);
            e->next++; n++;
        }
        if (n)
        {
            if (ctx.r7.u32) Store32(ctx.r7.u32, n);
            if (ctx.r8.u32) { CompleteOverlapped(ctx.r8.u32, X_ERROR_SUCCESS, n); ctx.r3.u64 = X_ERROR_IO_PENDING; }
            else ctx.r3.u64 = X_ERROR_SUCCESS;
            return;
        }
    }
    if (ctx.r8.u32) { CompleteOverlapped(ctx.r8.u32, X_ERROR_NO_MORE_FILES); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_NO_MORE_FILES;
}

// ---- mensajes XMsg ----
// Sesion 24: registro de los mensajes (app 0xFB = xlivebase: logros, perfil, sesiones) para estudiar los logros
static void LogXMsg(const char* fn, uint32_t app, uint32_t msg, uint32_t buf, uint32_t size)
{
    static int n = 0;
    if (n++ < 60) LOG("[xmsg] %s(app 0x%X, mensaje 0x%08X, buffer %08X, %u bytes)", fn, app, msg, buf, size);
}
GUEST_IMPL(XMsgInProcessCall) { LogXMsg("XMsgInProcessCall", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); ctx.r3.u64 = X_ERROR_SUCCESS; }
// XMsgStartIORequestEx(app, message, overlapped, buffer, buffer_size, unk)
GUEST_IMPL(XMsgStartIORequestEx)
{
    LogXMsg("XMsgStartIORequestEx", ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
    if (ctx.r5.u32) { CompleteOverlapped(ctx.r5.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}
// XMsgStartIORequest(app, message, overlapped, buffer, buffer_size)
GUEST_IMPL(XMsgStartIORequest)
{
    LogXMsg("XMsgStartIORequest", ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
    if (ctx.r5.u32) { CompleteOverlapped(ctx.r5.u32, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}

// NetDll_XNetGetTitleXnAddr(caller, XNADDR*): como Xenia, loopback y XNET_GET_XNADDR_STATIC (antes devolvia 0 = PENDIENTE para siempre)
// XNADDR: ina(4) inaOnline(4) wPortOnline(2) abEnet[6] abOnline[20]
GUEST_IMPL(NetDll_XNetGetTitleXnAddr)
{
    uint32_t a = ctx.r4.u32;
    if (a)
    {
        memset(g_base + a, 0, 36);
        Store32(a, 0x7F000001);
        memset(g_base + a + 10, 0xCC, 6);
    }
    ctx.r3.u64 = 0x04; // XNET_GET_XNADDR_STATIC
}
GUEST_IMPL(XCustomRegisterDynamicActions) { ctx.r3.u64 = 0; }
// Avatares: como Xenia, XamAvatarInitialize FALLA (~0) para que el juego no los use (con "exito" se quedaba esperando
// XamAvatarGetManifestLocalUser / XamAvatarGetMetadataRandom, que nadie completa).
GUEST_IMPL(XamAvatarInitialize) { ctx.r3.u64 = 0xFFFFFFFF; }
GUEST_IMPL(NetDll_XNetUnregisterInAddr) { ctx.r3.u64 = 0; }

// ---- red: sin conexion ----
GUEST_IMPL(NetDll_socket) { ctx.r3.u64 = 0xFFFFFFFF; }        // INVALID_SOCKET
GUEST_IMPL(NetDll_bind) { ctx.r3.u64 = 0xFFFFFFFF; }          // SOCKET_ERROR
GUEST_IMPL(NetDll_ioctlsocket) { ctx.r3.u64 = 0xFFFFFFFF; }
GUEST_IMPL(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; } // sin enlace

// ---- Sesion 24: diagnostico de funciones de script del lobby (GH6_CFUNCLOG=1) ----
// Tabla de CFuncs del juego {nombre, funcion}: se registran sus resultados (r3) cuando cambian, para ver que espera la interfaz.
namespace {
struct CFuncStat { uint64_t calls = 0; uint64_t last = ~0ull; int changes = 0; uint64_t dumped = 0; const char* name = nullptr; bool reg = false; };
std::mutex g_cfMutex; std::vector<CFuncStat*> g_cfAll;
void CFuncLog(const char* name, CFuncStat& st, uint64_t r3)
{
    static bool on = getenv("GH6_CFUNCLOG") != nullptr;
    if (!on) return;
    st.calls++;
    if (!st.reg) { std::lock_guard l(g_cfMutex); if (!st.reg) { st.reg = true; st.name = name; g_cfAll.push_back(&st); } }
    // Resumen cada 10 s: funciones llamadas en el intervalo, con su ultimo valor
    static std::atomic<DWORD> lastDump{ GetTickCount() };
    DWORD now = GetTickCount(), prev = lastDump.load();
    if (now - prev > 10000 && lastDump.compare_exchange_strong(prev, now))
    {
        std::lock_guard l(g_cfMutex);
        std::string line;
        for (CFuncStat* s : g_cfAll) if (s->calls != s->dumped) { char b[128]; snprintf(b, sizeof(b), " %s=%llu x%llu;", s->name, (unsigned long long)s->last, (unsigned long long)(s->calls - s->dumped)); line += b; s->dumped = s->calls; }
        LOG("[cfunc-resumen] t=%lus%s", now / 1000, line.c_str());
    }
    if (r3 != st.last && st.changes < 40) { st.changes++; if (0) LOG("[cfunc] %s -> %llu (llamada %llu, t=%lus)", name, (unsigned long long)r3, (unsigned long long)st.calls, GetTickCount() / 1000); } st.last = r3;
}
}
#define GH6_CFUNC_HOOK(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static CFuncStat st; __imp__sub_##addr(ctx, base); CFuncLog(label, st, ctx.r3.u64); }
GH6_CFUNC_HOOK(82518488, "IsPendingPlayer")
GH6_CFUNC_HOOK(823F1198, "AllPlayersAreReady")
GH6_CFUNC_HOOK(82511E78, "IsAssetLoaded")
GH6_CFUNC_HOOK(82176020, "GamerPicTextureGetStatus")
GH6_CFUNC_HOOK(8250F728, "IsTextureLoaded")
GH6_CFUNC_HOOK(82435CE0, "PendingPakManLoads")
GH6_CFUNC_HOOK(824C1B90, "IsFileCacheFinished")
GH6_CFUNC_HOOK(8245CF10, "VerifyCAPTextures")
// Sesion 24: igual que GH6_CFUNC_HOOK pero vuelca los argumentos (CStruct de script: lista de componentes
// {u8, u8 tipo, u16, u32 nombre, u32 valor, u32 siguiente}) cuando GH6_CFUNCLOG esta activo.
static void CFuncDumpArgs(const char* name, uint32_t st)
{
    static bool on = getenv("GH6_CFUNCLOG") != nullptr;
    static int n = 0;
    if (!on || n > 400 || !st) return;
    n++;
    std::string line; char b[96];
    uint32_t c = Load32(st);
    for (int i = 0; i < 16 && c && c < 0xC0000000u; i++)
    {
        snprintf(b, sizeof(b), " [%08X %08X %08X %08X]", Load32(c), Load32(c + 4), Load32(c + 8), Load32(c + 12)); line += b;
        c = Load32(c + 12);
    }
    LOG("[cfunc-args] %s st=%08X raw [%08X %08X %08X %08X]%s", name, st, Load32(st), Load32(st + 4), Load32(st + 8), Load32(st + 12), line.c_str());
}
#define GH6_CFUNC_HOOK_ARGS(addr, label)     PPC_FUNC_IMPL(__imp__sub_##addr);     PPC_FUNC(sub_##addr) { static CFuncStat st; CFuncDumpArgs(label, ctx.r3.u32); __imp__sub_##addr(ctx, base); CFuncLog(label, st, ctx.r3.u64); }
#include "cfunc_hooks.inc"
// Sesion 24: argumentos reales de la creacion de sustituciones de textura por viewport (CreateViewportTextureOverride)
PPC_FUNC_IMPL(__imp__sub_824E88E0);
PPC_FUNC(sub_824E88E0)
{
    LOG("[vptex] crear sustitucion: id=%08X texdict=%08X texture=%08X viewport?=%08X", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    static uint32_t skip = getenv("GH6_SKIPVPTEX") ? (uint32_t)strtoul(getenv("GH6_SKIPVPTEX"), nullptr, 16) : 0;   // diagnostico
    if (skip && ctx.r3.u32 == skip) { LOG("[vptex]   (omitida por GH6_SKIPVPTEX)"); ctx.r3.u64 = 1; return; }
    __imp__sub_824E88E0(ctx, base);
    LOG("[vptex]   -> %08X", ctx.r3.u32);
}
// Sesion 24: aplicacion de sustituciones textura<-viewport (estado: 3 aplicada, 2 descartada)
PPC_FUNC_IMPL(__imp__sub_8253F028);
PPC_FUNC(sub_8253F028)
{
    uint32_t node = ctx.r3.u32;
    uint32_t id = node ? Load32(node + 24) : 0, tex = node ? Load32(node + 32) : 0, vp = node ? Load32(node + 36) : 0;
    uint32_t tobj = node ? Load32(node + 40) : 0, vobj = node ? Load32(node + 44) : 0;
    auto hdr = [](uint32_t o) { std::string r; char b[16]; for (int i = 0; i < 14 && o; i++) { snprintf(b, sizeof(b), " %08X", Load32(o + i * 4)); r += b; } return r; };
    std::string before = hdr(tobj);
    __imp__sub_8253F028(ctx, base);
    static int n = 0;
    if (n++ < 200) LOG("[vptex] aplicar nodo %08X id=%08X textura=%08X viewport=%08X tobj=%08X vobj=%08X -> estado %u | antes:%s | despues:%s",
        node, id, tex, vp, tobj, vobj, ctx.r3.u32, before.c_str(), hdr(tobj).c_str());
}

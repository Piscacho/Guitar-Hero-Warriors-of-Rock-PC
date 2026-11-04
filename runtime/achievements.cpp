// Logros (sesion 25). El juego desbloquea logros con XUserWriteAchievements (sub_822CA650): XMsgStartIORequest a XGI (app 0xFB),
// mensaje 0x000B0008, buffer {numero, puntero a XUSER_ACHIEVEMENT[] = {indice de usuario, id}}. La lista de logros (nombre,
// descripcion, puntos) esta en la base XDBF/SPA del propio ejecutable (VA 82BC0000): espacio 1 'XACH' = logros, espacio 3 =
// tablas de cadenas por idioma (5 = espanol, 1 = ingles).
// Se guardan en <carpeta de datos>\logros.txt (desbloqueados) y logros_lista.txt (lista completa para el launcher) y se avisa en
// pantalla. GH6_LOGRO=<id> desbloquea uno al arrancar (prueba).
#include "runtime.h"
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <ctime>

std::string FsContentDir();                            // fs.cpp: <carpeta de datos>ntent
void OverlayToast(const char* utf8, double seconds);   // gpu_d3d12.cpp

namespace
{
constexpr uint32_t kXdbf = 0x82BC0000;
struct Ach { uint16_t id = 0, gs = 0; std::string name, desc, howto; };
std::vector<Ach> g_cat;
std::map<uint16_t, std::string> g_unlocked;   // id -> fecha
std::mutex g_mutex;
bool g_loaded = false;

uint16_t Be16(uint32_t a) { return (uint16_t)((g_base[a] << 8) | g_base[a + 1]); }
uint32_t Be32(uint32_t a) { return Load32(a); }

std::map<uint16_t, std::string> Strings(const std::map<uint64_t, std::pair<uint32_t, uint32_t>>& ent, uint32_t lang)
{
    std::map<uint16_t, std::string> t;
    auto it = ent.find((3ull << 48) | lang);
    if (it == ent.end()) return t;
    uint32_t o = it->second.first;
    if (Be32(o) != 0x58535452) return t;   // 'XSTR'
    uint16_t cnt = Be16(o + 12); uint32_t p = o + 14;
    for (uint16_t i = 0; i < cnt; i++)
    {
        uint16_t sid = Be16(p), sl = Be16(p + 2);
        t[sid] = std::string((const char*)g_base + p + 4, sl);
        p += 4 + sl;
    }
    return t;
}

void LoadCatalog()
{
    if (Be32(kXdbf) != 0x58444246) { LOG("[logros] no se encontro la base XDBF en %08X", kXdbf); return; }   // 'XDBF'
    uint32_t etl = Be32(kXdbf + 8), ec = Be32(kXdbf + 12), ftl = Be32(kXdbf + 16);
    if (ec > 4096 || etl > 4096) return;
    uint32_t data = kXdbf + 24 + etl * 18 + ftl * 8;
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> ent;   // (espacio << 48 | id) -> (direccion, tamano)
    for (uint32_t i = 0; i < ec; i++)
    {
        uint32_t e = kXdbf + 24 + i * 18;
        uint64_t id = ((uint64_t)Be32(e + 2) << 32) | Be32(e + 6);
        ent[((uint64_t)Be16(e) << 48) | (id & 0xFFFFFFFFFFFFull)] = { data + Be32(e + 10), Be32(e + 14) };
    }
    auto es = Strings(ent, 5), en = Strings(ent, 1);
    auto str = [&](uint16_t id) { auto i = es.find(id); if (i != es.end()) return i->second; auto j = en.find(id); return j != en.end() ? j->second : std::string(); };
    auto it = ent.find((1ull << 48) | 0x58414348);   // 'XACH'
    if (it == ent.end()) { LOG("[logros] la base XDBF no tiene logros"); return; }
    uint32_t o = it->second.first;
    uint16_t cnt = Be16(o + 12); uint32_t p = o + 14;
    for (uint16_t i = 0; i < cnt && i < 512; i++, p += 36)
    {
        Ach a; a.id = Be16(p); a.name = str(Be16(p + 2)); a.desc = str(Be16(p + 4)); a.howto = str(Be16(p + 6)); a.gs = Be16(p + 12);
        g_cat.push_back(a);
    }
    LOG("[logros] %zu logros en la base del juego", g_cat.size());
}

std::string Dir() { std::string c = FsContentDir(); size_t p = c.rfind("\\content"); return p != std::string::npos && p > 0 ? c.substr(0, p) : std::string("userdata"); }

void LoadState()
{
    FILE* f = fopen((Dir() + "\\logros.txt").c_str(), "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == ';' || line[0] == '#') continue;
        unsigned id = 0; char date[64] = {};
        if (sscanf(line, "%u %63[^\r\n]", &id, date) >= 1 && id < 65536) g_unlocked[(uint16_t)id] = date;
    }
    fclose(f);
}

void SaveFiles()
{
    CreateDirectoryA(Dir().c_str(), nullptr);
    if (FILE* f = fopen((Dir() + "\\logros.txt").c_str(), "wb"))
    {
        fprintf(f, "; Logros desbloqueados (id y fecha). Borra una linea para volver a bloquear ese logro.\n");
        for (auto& [id, date] : g_unlocked) fprintf(f, "%u %s\n", id, date.c_str());
        fclose(f);
    }
    if (g_cat.empty()) return;
    // Lista completa para el launcher: id|puntos|fecha o 0|nombre|descripcion|como conseguirlo (UTF-8)
    if (FILE* f = fopen((Dir() + "\\logros_lista.txt").c_str(), "wb"))
    {
        for (auto& a : g_cat)
        {
            auto u = g_unlocked.find(a.id);
            auto clean = [](std::string s) { for (auto& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' '; return s; };
            fprintf(f, "%u|%u|%s|%s|%s|%s\n", a.id, a.gs, u != g_unlocked.end() ? clean(u->second).c_str() : "0",
                    clean(a.name).c_str(), clean(a.desc).c_str(), clean(a.howto).c_str());
        }
        fclose(f);
    }
}
} // namespace

void AchievementUnlock(uint32_t id);

// Primera llamada: con la carpeta de datos ya configurada (el juego habla con XGI al iniciar sesion)
void AchievementsInit()
{
    {
        std::lock_guard l(g_mutex);
        if (g_loaded) return;
        g_loaded = true;
        LoadCatalog();
        LoadState();
        SaveFiles();
    }
    if (const char* t = getenv("GH6_LOGRO")) AchievementUnlock((uint32_t)atoi(t));
}

void AchievementUnlock(uint32_t id)
{
    AchievementsInit();
    std::string toast;
    {
        std::lock_guard l(g_mutex);
        if (id > 0xFFFF || g_unlocked.count((uint16_t)id)) return;
        char date[32]; time_t now = time(nullptr); tm lt{}; localtime_s(&lt, &now);
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &lt);
        g_unlocked[(uint16_t)id] = date;
        const Ach* a = nullptr;
        for (auto& c : g_cat) if (c.id == id) a = &c;
        uint32_t total = 0, pts = 0;
        for (auto& c : g_cat) if (g_unlocked.count(c.id)) { total++; pts += c.gs; }
        LOG("[logros] desbloqueado %u: %s (%u G) - %u/%zu logros, %u puntos", id, a ? a->name.c_str() : "?", a ? a->gs : 0, total, g_cat.size(), pts);
        toast = a ? "Logro: " + a->name + " (" + std::to_string(a->gs) + " G)" : "Logro desbloqueado (" + std::to_string(id) + ")";
        SaveFiles();
    }
    OverlayToast(toast.c_str(), 6.0);
}

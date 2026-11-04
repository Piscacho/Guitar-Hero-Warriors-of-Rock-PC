// Actualizador (sesion 24): busca la ultima release de GitHub, descarga el .zip, lo descomprime con tar.exe de Windows e
// instala los archivos nuevos junto al ejecutable conservando la configuracion, las partidas y el DLC. El ejecutable en uso
// (y las DLL cargadas) se renombran a *.old, que se borran en el siguiente arranque.
// GH6_UPDATE_API=<url> sustituye la URL de la API (pruebas con un servidor local).
#include "runtime.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

const char* AppVersion() { return "1.3.0"; }   // mantener igual que project/gh6.rc

namespace
{
std::wstring Wide(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n - 1, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }
std::string Narrow(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n - 1, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s; }
fs::path ExePath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return fs::path(p); }
fs::path TempDir() { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); return fs::path(t) / L"GH6WoR_actualizacion"; }

// GET por HTTP(S). Si file != INVALID_HANDLE_VALUE escribe ahi; si no, en body. prog(leido, total) opcional.
bool HttpGet(const std::wstring& url, std::string* body, HANDLE file, const std::function<void(uint64_t, uint64_t)>& prog, bool api, std::string& err)
{
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[4096] = {}, extra[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "URL no valida"; return false; }
    HINTERNET s = WinHttpOpen(L"GH6WoR-Launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHTTP no disponible"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    const wchar_t* hdr = api ? L"Accept: application/vnd.github+json\r\n" : L"Accept: application/octet-stream\r\n";
    if (r && WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
    {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        uint64_t total = 0; wchar_t cl[32] = {}; DWORD cls = sizeof(cl);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cls, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(cl, nullptr, 10);
        if (status == 200)
        {
            uint64_t got = 0; std::vector<char> buf(1 << 16); ok = true;
            for (;;)
            {
                DWORD avail = 0, rd = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; err = "conexion interrumpida"; break; }
                if (!avail) break;
                if (!WinHttpReadData(r, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &rd)) { ok = false; err = "conexion interrumpida"; break; }
                if (file != INVALID_HANDLE_VALUE) { DWORD wr = 0; if (!WriteFile(file, buf.data(), rd, &wr, nullptr) || wr != rd) { ok = false; err = "no se pudo escribir el archivo"; break; } }
                else body->append(buf.data(), rd);
                got += rd; if (prog) prog(got, total);
            }
        }
        else if (status == 404) err = "no hay releases publicadas en el repositorio (404)";
        else if (status == 403) err = "GitHub limito las consultas; prueba mas tarde (403)";
        else err = "respuesta HTTP " + std::to_string(status);
    }
    else err = "sin conexion con el servidor (" + std::to_string(GetLastError()) + ")";
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return ok;
}
// Valor de una clave de texto del JSON de GitHub (sin parser completo: las claves que usamos son unicas o vale la primera)
std::string JsonString(const std::string& j, const std::string& key, size_t from = 0, size_t* endPos = nullptr)
{
    size_t k = j.find("\"" + key + "\"", from); if (k == std::string::npos) return "";
    size_t c = j.find(':', k); if (c == std::string::npos) return "";
    size_t q = j.find_first_not_of(" \t\r\n", c + 1); if (q == std::string::npos || j[q] != '"') return "";
    std::string out;
    for (size_t i = q + 1; i < j.size(); i++)
    {
        char ch = j[i];
        if (ch == '\\' && i + 1 < j.size())
        {
            char e = j[++i];
            if (e == 'n') out += '\n'; else if (e == 'r') {} else if (e == 't') out += '\t';
            else if (e == 'u' && i + 4 < j.size()) { unsigned v = 0; sscanf(j.substr(i + 1, 4).c_str(), "%4x", &v); i += 4; if (v < 0x80) out += (char)v; else { std::wstring w(1, (wchar_t)v); out += Narrow(w); } }
            else out += e;
        }
        else if (ch == '"') { if (endPos) *endPos = i; break; }
        else out += ch;
    }
    return out;
}
void ParseVersion(const std::string& s, int v[3])
{
    v[0] = v[1] = v[2] = 0; int k = 0; size_t i = 0;
    while (i < s.size() && k < 3) { if (isdigit((unsigned char)s[i])) { v[k++] = atoi(s.c_str() + i); while (i < s.size() && isdigit((unsigned char)s[i])) i++; } else i++; }
}
bool Newer(const std::string& tag, const std::string& cur)
{
    int a[3], b[3]; ParseVersion(tag, a); ParseVersion(cur, b);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}
// Sustituye dst por src; si dst esta en uso (exe/DLL cargados) lo renombra a .old
bool ReplaceOne(const fs::path& src, const fs::path& dst, std::string& err)
{
    if (fs::exists(dst) && !DeleteFileW(dst.c_str()))
    {
        fs::path old = dst; old += L".old";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { err = "no se pudo reemplazar " + Narrow(dst.filename().wstring()); return false; }
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { err = "no se pudo copiar " + Narrow(dst.filename().wstring()); return false; }
    return true;
}
} // namespace

struct UpdateInfo { bool ok = false, newer = false; std::string error, tag, zipUrl, notes, page; };
UpdateInfo UpdateCheck(const std::string& repo)
{
    UpdateInfo u;
    std::string url = getenv("GH6_UPDATE_API") ? getenv("GH6_UPDATE_API") : "https://api.github.com/repos/" + repo + "/releases/latest";
    std::string body;
    if (!HttpGet(Wide(url), &body, INVALID_HANDLE_VALUE, nullptr, true, u.error)) { LOG("[actualizar] %s: %s", url.c_str(), u.error.c_str()); return u; }
    u.tag = JsonString(body, "tag_name");
    u.notes = JsonString(body, "body");
    u.page = JsonString(body, "html_url");
    for (size_t pos = 0;;)
    {
        size_t end = 0; size_t k = body.find("\"browser_download_url\"", pos); if (k == std::string::npos) break;
        std::string a = JsonString(body, "browser_download_url", k, &end); pos = end ? end : k + 1;
        std::string low = a; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0) { u.zipUrl = a; break; }
    }
    if (u.tag.empty()) { u.error = "respuesta de GitHub sin version"; return u; }
    u.ok = true; u.newer = Newer(u.tag, AppVersion());
    if (u.newer && u.zipUrl.empty()) { u.ok = false; u.error = "la release " + u.tag + " no tiene un .zip"; }
    LOG("[actualizar] ultima release %s (actual %s)%s", u.tag.c_str(), AppVersion(), u.newer ? ": hay version nueva" : "");
    return u;
}
// Descarga e instala. preserve = nombres de primer nivel que no se tocan (ademas de los fijos). progress(porcentaje, fase).
bool UpdateInstall(const UpdateInfo& u, const std::vector<std::wstring>& preserve, const std::function<void(int, int)>& progress, std::string& err)
{
    std::error_code ec;
    fs::path tmp = TempDir(); fs::remove_all(tmp, ec); fs::create_directories(tmp / L"x", ec);
    fs::path zip = tmp / L"paquete.zip";
    HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = "no se pudo crear el archivo temporal"; return false; }
    bool ok = HttpGet(Wide(u.zipUrl), nullptr, f, [&](uint64_t got, uint64_t total) { if (total) progress((int)(got * 100 / total), 0); }, false, err);
    CloseHandle(f);
    if (!ok) return false;
    // Descomprimir con tar.exe (incluido en Windows 10 1803+)
    progress(0, 1);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + (tmp / L"x").wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { err = "no se encontro tar.exe para descomprimir"; return false; }
    WaitForSingleObject(pi.hProcess, 120000); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (code != 0) { err = "el .zip descargado esta danado"; return false; }
    // Raiz del paquete: la carpeta que contiene el .exe (el zip puede traer una carpeta superior)
    fs::path root = tmp / L"x";
    for (int depth = 0; depth < 3; depth++)
    {
        bool hasExe = false; std::vector<fs::path> dirs;
        for (auto& e : fs::directory_iterator(root, ec)) { if (e.is_directory()) dirs.push_back(e.path()); else if (e.path().extension() == L".exe") hasExe = true; }
        if (hasExe) break;
        if (dirs.size() != 1) { err = "el paquete no contiene el juego"; return false; }
        root = dirs[0];
    }
    fs::path exe = ExePath(), dir = exe.parent_path(), newExe;
    for (auto& e : fs::directory_iterator(root, ec))
        if (!e.is_directory() && e.path().extension() == L".exe") { if (newExe.empty() || _wcsicmp(e.path().filename().c_str(), exe.filename().c_str()) == 0) newExe = e.path(); }
    if (newExe.empty()) { err = "el paquete no contiene el juego"; return false; }
    std::vector<std::wstring> keep = { L"gh6.ini", L"save", L"userdata", L"dlc", L"shadercache", L"shaders", L"gh6.log" };
    keep.insert(keep.end(), preserve.begin(), preserve.end());
    auto kept = [&](const fs::path& p) { for (auto& k : keep) if (_wcsicmp(p.filename().c_str(), k.c_str()) == 0) return true; return p.extension() == L".old"; };
    progress(0, 2);
    int n = 0;
    for (auto& e : fs::directory_iterator(root, ec))
    {
        if (kept(e.path())) continue;
        if (e.is_directory())
        {
            fs::copy(e.path(), dir / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) { err = "no se pudo copiar la carpeta " + Narrow(e.path().filename().wstring()); return false; }
        }
        else if (!ReplaceOne(e.path(), e.path() == newExe ? exe : dir / e.path().filename(), err)) return false;
        n++;
    }
    LOG("[actualizar] instalada %s (%d elementos)", u.tag.c_str(), n);
    return true;
}
// Al arrancar: borra los .old de una actualizacion anterior y la carpeta temporal
void UpdaterCleanup()
{
    std::error_code ec;
    fs::path dir = ExePath().parent_path();
    for (auto& e : fs::directory_iterator(dir, ec)) if (!e.is_directory() && e.path().extension() == L".old") DeleteFileW(e.path().c_str());
    fs::remove_all(TempDir(), ec);
}
// Reinicia el programa (mostrando el launcher) tras actualizar
bool UpdaterRelaunch()
{
    fs::path exe = ExePath();
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --launcher";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) return false;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

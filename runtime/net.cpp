// Red y Xbox LIVE (sesion 27): online P2P sin servidores.
// Por defecto se comporta como antes (sin red). Variables:
//   GH6_NETLOG=1   registra cada llamada con sus argumentos.
//   GH6_NETFAKE=1  solo finge cable y direccion en linea (para estudiar el menu online).
//   GH6_NET=1      red real (implica NETFAKE; usar con GH6_SIGNIN=2): sockets del juego sobre un unico puerto UDP propio,
//                  direcciones de Xbox LIVE traducidas a direcciones reales, QoS, sesiones XSession y busqueda de partidas en
//                  la red local (multicast) o en los pares de GH6_NETPEER=ip:puerto[,ip:puerto...]. GH6_NETPORT = puerto UDP
//                  (3074 por defecto; si esta ocupado, el siguiente libre: dos copias en el mismo PC).
//
// Protocolo propio (todo dentro del puerto UDP): cabecera 'GH6N' + version + tipo + id del par (6 bytes).
//   DATA   puerto destino/origen del juego (2+2) + datos: los sockets del juego son colas virtuales por puerto.
//   QUERY  busqueda de partidas (multicast 239.255.66.6:37360 o directa) -> ADVERT con cada sesion de matchmaking abierta.
//   HELLO/HELLOACK  presentacion (XNADDR) y medida del ping.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <algorithm>
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <vector>
#include <thread>
#include <random>
#include <string>

namespace
{
bool NetLogOn() { static const bool on = getenv("GH6_NETLOG") != nullptr; return on; }
bool NetOn() { static const bool on = getenv("GH6_NET") && atoi(getenv("GH6_NET")) > 0; return on; }
bool NetFake() { static const bool on = NetOn() || (getenv("GH6_NETFAKE") && atoi(getenv("GH6_NETFAKE")) > 0); return on; }
constexpr uint32_t kTitleId = 0x41560883;
std::atomic<int> g_netLogCount{ 0 };
}
#define NETLOG(...) do { if (NetLogOn() && g_netLogCount++ < 20000) LOG(__VA_ARGS__); } while (0)
#define NETARGS(name) NETLOG("[net] " name "(%08X, %08X, %08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32)

namespace
{
// ---------------------------------------------------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint16_t kDiscPort = 37360;
constexpr uint32_t kDiscGroup = 0xEFFF4206;   // 239.255.66.6
constexpr uint8_t kVer = 1;
enum : uint8_t { T_DATA = 1, T_QUERY = 2, T_ADVERT = 3, T_HELLO = 4, T_HELLOACK = 5 };
constexpr uint32_t kSelfVaddr = 0x7F000001;
constexpr int WSAE_WOULDBLOCK = 10035, WSAE_INVAL = 10022, WSAE_NOTSOCK = 10038, WSAE_OPNOTSUPP = 10045, WSAE_TIMEDOUT = 10060;

std::mutex g_m;
std::condition_variable g_cv;
SOCKET g_mux = INVALID_SOCKET, g_disc = INVALID_SOCKET;
uint16_t g_muxPort = 0;
uint32_t g_lanIp = 0x7F000001;   // orden de host
uint8_t g_myId[6]{};
uint8_t g_myXna[36]{};
bool g_started = false;
thread_local int t_lastError = 0;
std::mt19937_64 g_rng{ std::random_device{}() };

uint64_t NowMs() { return GetTickCount64(); }

struct Peer
{
    uint8_t id[6]{};
    uint8_t xna[36]{};
    bool haveXna = false;
    sockaddr_in ep{};
    uint32_t vaddr = 0;
    bool connected = false;
    uint64_t lastSeen = 0, helloSent = 0;
    int rttMs = 30;
    uint8_t kid[8]{};
};
std::deque<Peer> g_peers;   // deque: las referencias no se invalidan al anadir

struct Pkt { uint32_t fromVaddr; uint16_t fromPort; std::vector<uint8_t> data; };
struct GSock
{
    uint32_t type = 2, proto = 17;
    uint16_t port = 0;
    bool bound = false, nonblock = false;
    uint32_t rcvTimeoutMs = 0;
    uint32_t connVaddr = 0; uint16_t connPort = 0;
    std::deque<Pkt> q;
};
std::map<uint32_t, GSock> g_socks;
uint32_t g_nextSock = 0x4000;
uint16_t g_nextEphemeral = 49152;

struct Sess
{
    uint32_t obj = 0, flags = 0, pub = 0, priv = 0, filledPub = 0, filledPriv = 0;
    bool host = false;
    uint8_t info[60]{};   // XSESSION_INFO tal como esta en memoria del juego: XNKID(8) + XNADDR del anfitrion(36) + XNKEY(16)
};
std::map<uint32_t, Sess> g_sess;
std::map<uint32_t, uint32_t> g_ctx;                    // XUserSetContext
std::map<uint32_t, std::vector<uint8_t>> g_prop;       // XUserSetProperty (valor crudo)
std::map<uint64_t, std::vector<uint8_t>> g_qosData;    // XNetQosListen por XNKID

struct Advert
{
    uint8_t peerId[6]{};
    uint8_t info[60]{};
    uint32_t flags = 0, pub = 0, priv = 0, filledPub = 0, filledPriv = 0;
    std::vector<std::pair<uint32_t, uint32_t>> ctx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> props;
    std::vector<uint8_t> qos;
    uint64_t seen = 0;
};
std::vector<Advert> g_adverts;
uint64_t g_searchUntil = 0, g_lastQuery = 0;
std::atomic<uint64_t> g_matchTick{ 0 };   // para los guiones de prueba: partida formada
std::vector<sockaddr_in> g_manualPeers;

uint64_t Kid64(const uint8_t* k) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | k[i]; return v; }
std::string Ep(const sockaddr_in& a) { char b[32]; inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b)); return std::string(b) + ":" + std::to_string(ntohs(a.sin_port)); }
std::string Hex(const uint8_t* p, int n) { std::string s; char b[4]; for (int i = 0; i < n; i++) { snprintf(b, 4, "%02X", p[i]); s += b; } return s; }

Peer* PeerById(const uint8_t* id) { for (auto& p : g_peers) if (!memcmp(p.id, id, 6)) return &p; return nullptr; }
Peer* PeerByVaddr(uint32_t v) { for (auto& p : g_peers) if (p.vaddr == v) return &p; return nullptr; }
Peer& AddPeer(const uint8_t* id)
{
    if (Peer* p = PeerById(id)) return *p;
    Peer p; memcpy(p.id, id, 6);
    p.vaddr = 0x0A400001 + (uint32_t)g_peers.size();   // 10.64.0.x: direccion "segura" que ve el juego
    g_peers.push_back(p);
    NETLOG("[net] par nuevo %s -> %u.%u.%u.%u", Hex(id, 6).c_str(), p.vaddr >> 24, (p.vaddr >> 16) & 255, (p.vaddr >> 8) & 255, p.vaddr & 255);
    return g_peers.back();
}
void SetPeerXna(Peer& p, const uint8_t* xna)
{
    memcpy(p.xna, xna, 36); p.haveXna = true;
    if (!p.ep.sin_port)
    {   // direccion inicial: la que anuncia su XNADDR (ina + puerto); luego se corrige con la de origen real de sus paquetes
        p.ep.sin_family = AF_INET;
        memcpy(&p.ep.sin_addr, xna, 4);
        p.ep.sin_port = htons((uint16_t)((xna[8] << 8) | xna[9]));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Envio
// ---------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> Header(uint8_t type)
{
    std::vector<uint8_t> b = { 'G', 'H', '6', 'N', kVer, type };
    b.insert(b.end(), g_myId, g_myId + 6);
    return b;
}
void SendRaw(const std::vector<uint8_t>& b, const sockaddr_in& to)
{
    if (g_mux != INVALID_SOCKET && to.sin_port) sendto(g_mux, (const char*)b.data(), (int)b.size(), 0, (const sockaddr*)&to, sizeof(to));
}
void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> (i * 8))); }
void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

void SendHello(Peer& p, uint8_t type, uint64_t echoTs = 0)
{
    auto b = Header(type);
    b.insert(b.end(), g_myXna, g_myXna + 36);
    uint64_t ts = type == T_HELLO ? NowMs() : echoTs;
    for (int i = 7; i >= 0; i--) b.push_back((uint8_t)(ts >> (i * 8)));
    if (type == T_HELLO) p.helloSent = NowMs();
    SendRaw(b, p.ep);
}
void SendQuery()
{
    auto b = Header(T_QUERY);
    sockaddr_in g{}; g.sin_family = AF_INET; g.sin_addr.s_addr = htonl(kDiscGroup); g.sin_port = htons(kDiscPort);
    SendRaw(b, g);
    for (auto& m : g_manualPeers) SendRaw(b, m);
    g_lastQuery = NowMs();
}
void SendAdverts(const sockaddr_in& to)
{
    for (auto& [obj, s] : g_sess)
    {
        if (!s.host || !(s.flags & 0x8)) continue;   // solo sesiones de matchmaking de las que somos anfitrion
        if (s.filledPub >= s.pub) continue;
        auto b = Header(T_ADVERT);
        b.insert(b.end(), s.info, s.info + 60);
        Put32(b, s.flags); Put32(b, s.pub); Put32(b, s.priv); Put32(b, s.filledPub); Put32(b, s.filledPriv);
        Put16(b, (uint16_t)g_ctx.size()); Put16(b, (uint16_t)g_prop.size());
        for (auto& [id, v] : g_ctx) { Put32(b, id); Put32(b, v); }
        for (auto& [id, v] : g_prop) { Put32(b, id); Put16(b, (uint16_t)v.size()); b.insert(b.end(), v.begin(), v.end()); }
        auto q = g_qosData.find(Kid64(s.info));
        uint16_t ql = q != g_qosData.end() ? (uint16_t)q->second.size() : 0;
        Put16(b, ql); if (ql) b.insert(b.end(), q->second.begin(), q->second.end());
        SendRaw(b, to);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Recepcion (hilo propio)
// ---------------------------------------------------------------------------------------------------------------------
void HandlePacket(const uint8_t* d, int n, const sockaddr_in& from, bool viaDisc)
{
    if (n < 12 || memcmp(d, "GH6N", 4) || d[4] != kVer) return;
    uint8_t type = d[5]; const uint8_t* id = d + 6;
    if (!memcmp(id, g_myId, 6)) return;   // nuestro propio multicast
    const uint8_t* p = d + 12; int len = n - 12;
    std::lock_guard l(g_m);
    if (type == T_QUERY) { SendAdverts(from); if (!viaDisc && g_peers.size() < 64) { Peer& q = AddPeer(id); q.ep = from; q.lastSeen = NowMs(); } return; }
    if (!PeerById(id) && g_peers.size() >= 64) return;   // limite ante paquetes de desconocidos
    Peer& peer = AddPeer(id);
    if (!viaDisc) { peer.ep = from; peer.lastSeen = NowMs(); }
    switch (type)
    {
    case T_DATA:
    {
        if (len < 4) return;
        uint16_t dst = Get16(p), src = Get16(p + 2);
        if (!peer.haveXna && NowMs() - peer.helloSent > 1000) SendHello(peer, T_HELLO);
        for (auto& [h, s] : g_socks)
            if (s.bound && s.port == dst)
            {
                if (s.q.size() < 512) s.q.push_back({ peer.vaddr, src, std::vector<uint8_t>(p + 4, p + len) });
                g_cv.notify_all();
                break;
            }
        break;
    }
    case T_HELLO: case T_HELLOACK:
    {
        if (len < 44) return;
        SetPeerXna(peer, p);
        peer.connected = true;
        uint64_t ts = 0; for (int i = 0; i < 8; i++) ts = (ts << 8) | p[36 + i];
        if (type == T_HELLO) SendHello(peer, T_HELLOACK, ts);
        else { peer.rttMs = (int)std::max<uint64_t>(1, NowMs() - ts); }
        g_cv.notify_all();
        break;
    }
    case T_ADVERT:
    {
        if (len < 60 + 20 + 4) return;
        Advert a; memcpy(a.peerId, id, 6); memcpy(a.info, p, 60);
        const uint8_t* q = p + 60;
        a.flags = Get32(q); a.pub = Get32(q + 4); a.priv = Get32(q + 8); a.filledPub = Get32(q + 12); a.filledPriv = Get32(q + 16); q += 20;
        uint16_t nc = Get16(q), np = Get16(q + 2); q += 4;
        const uint8_t* end = p + len;
        for (int i = 0; i < nc && q + 8 <= end; i++, q += 8) a.ctx.push_back({ Get32(q), Get32(q + 4) });
        for (int i = 0; i < np && q + 6 <= end; i++)
        {
            uint32_t pid = Get32(q); uint16_t sz = Get16(q + 4); q += 6;
            if (q + sz > end) break;
            a.props.push_back({ pid, std::vector<uint8_t>(q, q + sz) }); q += sz;
        }
        if (q + 2 <= end) { uint16_t ql = Get16(q); q += 2; if (q + ql <= end) a.qos.assign(q, q + ql); }
        a.seen = NowMs();
        SetPeerXna(peer, a.info + 8);
        memcpy(peer.kid, a.info, 8);
        bool found = false;
        for (auto& o : g_adverts) if (!memcmp(o.peerId, id, 6) && !memcmp(o.info, a.info, 8)) { o = a; found = true; }
        if (!found)
        {
            g_adverts.push_back(a);
            std::string cs; for (auto& c : a.ctx) { char t[32]; snprintf(t, sizeof(t), " %X=%u", c.first, c.second); cs += t; }
            NETLOG("[net] partida encontrada: %s en %s (%u/%u) contextos:%s", Hex(a.info, 8).c_str(), Ep(peer.ep).c_str(), a.filledPub, a.pub, cs.c_str());
        }
        break;
    }
    }
}

bool HandleStun(const uint8_t* d, int n);
void RecvThread()
{
    std::vector<uint8_t> buf(65536);
    for (;;)
    {
        fd_set rs; FD_ZERO(&rs);
        FD_SET(g_mux, &rs); if (g_disc != INVALID_SOCKET) FD_SET(g_disc, &rs);
        timeval tv{ 0, 100000 };
        int r = select(0, &rs, nullptr, nullptr, &tv);
        if (r > 0)
            for (SOCKET s : { g_mux, g_disc })
                if (s != INVALID_SOCKET && FD_ISSET(s, &rs))
                {
                    sockaddr_in from{}; int fl = sizeof(from);
                    int n = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
                    if (n > 0 && !(s == g_mux && HandleStun(buf.data(), n))) HandlePacket(buf.data(), n, from, s == g_disc);
                }
        std::lock_guard l(g_m);
        uint64_t now = NowMs();
        if (now < g_searchUntil && now - g_lastQuery > 1000) SendQuery();
        static uint64_t lastPunch = 0;
        if (!g_manualPeers.empty() && now - lastPunch > 2000)
        {   // pares por codigo: un paquete cada 2 s abre (y mantiene abierto) el camino en los dos routers
            lastPunch = now;
            auto b = Header(T_QUERY);
            for (auto& m : g_manualPeers) SendRaw(b, m);
        }
        for (auto& p : g_peers) if (p.ep.sin_port && !p.connected && p.helloSent && now - p.helloSent > 1000) SendHello(p, T_HELLO);
    }
}

uint32_t LocalIp()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    uint32_t ip = 0x7F000001;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53); inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (s != INVALID_SOCKET && connect(s, (sockaddr*)&a, sizeof(a)) == 0)
    {
        sockaddr_in me{}; int l = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &l) == 0) ip = ntohl(me.sin_addr.s_addr);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return ip;
}

// ---------------------------------------------------------------------------------------------------------------------
// Internet (sesion 27): UPnP (puerto abierto en el router con concesion de 2 h, se renueva y se borra al salir), IP publica por
// STUN publico si no hay UPnP, y codigo de sala = IP publica + puerto (12 caracteres). Los pares de GH6_NETPEER (codigos de los
// amigos) reciben un paquete cada 2 s: asi los dos routers dejan pasar el trafico aunque ninguno tenga UPnP (si los dos ponen el
// codigo del otro).
// ---------------------------------------------------------------------------------------------------------------------
struct InetState { bool busy = false, done = false, upnp = false, cgnat = false; uint32_t pubIp = 0; uint16_t pubPort = 0; uint32_t stunIp = 0; uint16_t stunPort = 0; };
InetState g_inet;
std::string g_upnpControl, g_upnpService;
uint8_t g_stunTx[12]{};

bool HttpReq(const std::wstring& url, const wchar_t* verb, const std::wstring& headers, const std::string& body, std::string& out, DWORD& status)
{
    URL_COMPONENTS uc{ sizeof(uc) }; wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET ses = WinHttpOpen(L"GH6WoR", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 2000, 2000, 3000, 3000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool ok = req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1,
                                        body.empty() ? nullptr : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
              && WinHttpReceiveResponse(req, nullptr);
    if (ok)
    {
        DWORD sz = sizeof(status); WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        char b[4096]; DWORD n = 0;
        while (WinHttpReadData(req, b, sizeof(b), &n) && n) out.append(b, n);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}
std::string XmlTag(const std::string& x, const std::string& tag, size_t from = 0)
{
    size_t a = x.find("<" + tag + ">", from); if (a == std::string::npos) return "";
    a += tag.size() + 2; size_t b = x.find("</" + tag + ">", a);
    return b == std::string::npos ? "" : x.substr(a, b - a);
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string IpStr(uint32_t ip) { char b[20]; snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }
// IP privada / CGNAT (10/8, 172.16/12, 192.168/16, 100.64/10, 127/8)
bool PrivateIp(uint32_t ip) { return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == 0x191 || (ip >> 24) == 127 || ip == 0; }

bool UpnpSoap(const char* action, const std::string& args, std::string& out)
{
    std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) + " xmlns:u=\"" + g_upnpService + "\">" +
        args + "</u:" + action + "></s:Body></s:Envelope>";
    std::wstring hdr = L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + Wide(g_upnpService) + L"#" + Wide(action) + L"\"\r\n";
    DWORD st = 0;
    return HttpReq(Wide(g_upnpControl), L"POST", hdr, body, out, st) && st == 200;
}
std::string UpnpMapArgs(int lease)
{
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(g_muxPort) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
           "<NewInternalPort>" + std::to_string(g_muxPort) + "</NewInternalPort><NewInternalClient>" + IpStr(g_lanIp) + "</NewInternalClient>"
           "<NewEnabled>1</NewEnabled><NewPortMappingDescription>GH WoR PC</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
}
bool UpnpMap()
{
    std::string out;
    if (UpnpSoap("AddPortMapping", UpnpMapArgs(7200), out)) return true;
    out.clear(); return UpnpSoap("AddPortMapping", UpnpMapArgs(0), out);   // algunos routers solo admiten concesiones permanentes
}
void UpnpUnmap()
{
    if (g_upnpControl.empty() || !g_inet.upnp) return;
    std::string out;
    UpnpSoap("DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(g_muxPort) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>", out);
}
// Busca el router (SSDP) y abre el puerto. Devuelve la IP publica que dice el router (1 si no la dice, 0 si no hay UPnP).
uint32_t UpnpDiscover()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    std::vector<std::string> locs;
    for (const char* st : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1" })
    {
        std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, m.data(), (int)m.size(), 0, (sockaddr*)&to, sizeof(to));
    }
    uint64_t end = NowMs() + 2500;
    char buf[2048];
    while (NowMs() < end)
    {
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs); timeval tv{ 0, 200000 };
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        int n = recv(s, buf, sizeof(buf) - 1, 0); if (n <= 0) continue; buf[n] = 0;
        std::string r = buf, low = r; for (auto& c : low) c = (char)tolower((unsigned char)c);
        size_t k = low.find("\nlocation:"); if (k == std::string::npos) continue;
        size_t a = k + 10; while (a < r.size() && r[a] == ' ') a++;
        size_t b = r.find_first_of("\r\n", a);
        std::string loc = r.substr(a, b - a);
        if (std::find(locs.begin(), locs.end(), loc) == locs.end()) locs.push_back(loc);
    }
    closesocket(s);
    for (auto& loc : locs)
    {
        std::string desc; DWORD st = 0;
        if (!HttpReq(Wide(loc), L"GET", L"", "", desc, st) || st != 200) continue;
        for (const char* svc : { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1" })
        {
            size_t k = desc.find(svc); if (k == std::string::npos) continue;
            std::string ctl = XmlTag(desc, "controlURL", k); if (ctl.empty()) continue;
            if (ctl.find("://") == std::string::npos)
            {   // URL relativa: se resuelve con la base de la descripcion
                std::string base = XmlTag(desc, "URLBase");
                if (base.empty()) { size_t p = loc.find('/', loc.find("://") + 3); base = loc.substr(0, p); }
                while (!base.empty() && base.back() == '/') base.pop_back();
                ctl = base + (ctl[0] == '/' ? "" : "/") + ctl;
            }
            g_upnpControl = ctl; g_upnpService = svc;
            if (!UpnpMap()) { LOG("[net] UPnP: el router %s no acepto abrir el puerto", loc.c_str()); continue; }
            std::string out; uint32_t ip = 0;
            if (UpnpSoap("GetExternalIPAddress", "", out)) { in_addr a{}; if (inet_pton(AF_INET, XmlTag(out, "NewExternalIPAddress").c_str(), &a) == 1) ip = ntohl(a.s_addr); }
            return ip ? ip : 1;
        }
    }
    return 0;
}
void SendStun()
{
    static const char* servers[][2] = { { "stun.l.google.com", "19302" }, { "stun.cloudflare.com", "3478" } };
    for (auto& sv : servers)
    {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; addrinfo* res = nullptr;
        if (getaddrinfo(sv[0], sv[1], &hints, &res) != 0 || !res) continue;
        uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
        memcpy(req + 8, g_stunTx, 12);
        sendto(g_mux, (const char*)req, 20, 0, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
    }
}
// Respuesta STUN (RFC 5389) en el puerto propio: XOR-MAPPED-ADDRESS / MAPPED-ADDRESS
bool HandleStun(const uint8_t* d, int n)
{
    if (n < 20 || Get32(d + 4) != 0x2112A442 || memcmp(d + 8, g_stunTx, 12) || Get16(d) != 0x0101) return false;
    int len = std::min<int>(Get16(d + 2), n - 20);
    for (int o = 20; o + 4 <= 20 + len;)
    {
        uint16_t t = Get16(d + o), l = Get16(d + o + 2);
        const uint8_t* v = d + o + 4;
        if ((t == 0x0020 || t == 0x0001) && l >= 8 && v[1] == 1)
        {
            uint16_t port = Get16(v + 2); uint32_t ip = Get32(v + 4);
            if (t == 0x0020) { port ^= 0x2112; ip ^= 0x2112A442; }
            std::lock_guard l2(g_m);
            g_inet.stunIp = ip; g_inet.stunPort = port;
            return true;
        }
        o += 4 + ((l + 3) & ~3);
    }
    return true;
}
void InetThread()
{
    uint32_t ip = UpnpDiscover();
    { std::lock_guard l(g_m); if (ip) { g_inet.upnp = true; if (ip > 1) g_inet.pubIp = ip; g_inet.pubPort = g_muxPort; } }
    if (ip) { LOG("[net] UPnP: puerto UDP %u abierto en el router (IP publica %s)", g_muxPort, IpStr(ip).c_str()); atexit([] { UpnpUnmap(); }); }
    for (int i = 0; i < 12 && !g_inet.stunIp; i++) { if (i % 4 == 0) SendStun(); Sleep(250); }
    {
        std::lock_guard l(g_m);
        if (g_inet.upnp && ip > 1 && PrivateIp(ip)) g_inet.cgnat = true;   // el router tiene una IP privada: hay otro NAT delante
        if (g_inet.stunIp && g_inet.upnp && ip > 1 && g_inet.stunIp != ip) g_inet.cgnat = true;
        // el codigo usa la direccion que ve internet (STUN) si el router no tiene IP publica o no hay UPnP
        if (g_inet.stunIp && (!g_inet.pubIp || g_inet.cgnat)) { g_inet.pubIp = g_inet.stunIp; g_inet.pubPort = g_inet.stunPort; }
        g_inet.done = true; g_inet.busy = false;
    }
    LOG("[net] internet: codigo con %s:%u (router UPnP %s, STUN %s:%u)%s", IpStr(g_inet.pubIp).c_str(), g_inet.pubPort, g_inet.upnp ? IpStr(ip).c_str() : "no",
        IpStr(g_inet.stunIp).c_str(), g_inet.stunPort, g_inet.cgnat ? ", CGNAT" : "");
    while (g_inet.upnp) { Sleep(3600 * 1000); UpnpMap(); }   // renovar la concesion cada hora
}

std::mutex g_openM;
bool g_opened = false, g_peersLoaded = false;
// Abre el puerto UDP propio, la busqueda local y el hilo de recepcion (lo usa tambien el launcher para enseñar el codigo)
void NetOpen()
{
    std::lock_guard lo(g_openM);
    if (g_opened) return;
    g_opened = true;
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    g_lanIp = LocalIp();
    uint64_t r = g_rng() ^ (uint64_t(GetCurrentProcessId()) << 32) ^ NowMs();
    for (int i = 0; i < 6; i++) g_myId[i] = (uint8_t)(r >> (i * 8));
    g_myId[0] &= 0xFE;   // como una MAC unicast
    for (int i = 0; i < 12; i++) g_stunTx[i] = (uint8_t)g_rng();
    int base = getenv("GH6_NETPORT") ? atoi(getenv("GH6_NETPORT")) : 3074;
    for (int i = 0; i < 16 && g_mux == INVALID_SOCKET; i++)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((uint16_t)(base + i));
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) { g_mux = s; g_muxPort = (uint16_t)(base + i); }
        else closesocket(s);
    }
    if (g_mux == INVALID_SOCKET) { LOG("[net] no se pudo abrir el puerto UDP %d-%d", base, base + 15); return; }
    int ttl = 1; setsockopt(g_mux, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    BOOL loop = TRUE; setsockopt(g_mux, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    // busqueda en la red local: varias copias pueden escuchar el mismo puerto (SO_REUSEADDR) y el grupo multicast
    if (!getenv("GH6_NETNOLAN"))
    {
        g_disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL reuse = TRUE; setsockopt(g_disc, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in d{}; d.sin_family = AF_INET; d.sin_addr.s_addr = INADDR_ANY; d.sin_port = htons(kDiscPort);
        ip_mreq mr{}; mr.imr_multiaddr.s_addr = htonl(kDiscGroup); mr.imr_interface.s_addr = INADDR_ANY;
        if (bind(g_disc, (sockaddr*)&d, sizeof(d)) != 0 || setsockopt(g_disc, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr)) != 0)
        { LOG("[net] busqueda local no disponible (%d)", WSAGetLastError()); closesocket(g_disc); g_disc = INVALID_SOCKET; }
    }
    // nuestro XNADDR: ina/inaOnline = IP local, wPortOnline = puerto UDP, abEnet = id del par, abOnline = id + relleno
    memset(g_myXna, 0, 36);
    for (int i = 0; i < 4; i++) { g_myXna[i] = (uint8_t)(g_lanIp >> (24 - i * 8)); g_myXna[4 + i] = g_myXna[i]; }
    g_myXna[8] = (uint8_t)(g_muxPort >> 8); g_myXna[9] = (uint8_t)g_muxPort;
    memcpy(g_myXna + 10, g_myId, 6);
    for (int i = 0; i < 20; i++) g_myXna[16 + i] = g_myId[i % 6] ^ (uint8_t)(i * 37);
    LOG("[net] red P2P abierta: id %s, %s:%u, busqueda local %s", Hex(g_myId, 6).c_str(), IpStr(g_lanIp).c_str(), g_muxPort, g_disc != INVALID_SOCKET ? "si" : "no");
    std::thread(RecvThread).detach();
    if (!getenv("GH6_NETNOINET")) { g_inet.busy = true; std::thread(InetThread).detach(); }
}
void LoadManualPeers()
{
    if (g_peersLoaded) return;
    g_peersLoaded = true;
    if (const char* mp = getenv("GH6_NETPEER"))
    {
        std::string s = mp; size_t pos = 0;
        while (pos < s.size())
        {
            size_t c = s.find(',', pos); std::string e = s.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
            size_t k = e.find(':');
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(k != std::string::npos ? atoi(e.c_str() + k + 1) : 3074));
            if (inet_pton(AF_INET, e.substr(0, k).c_str(), &a.sin_addr) == 1) g_manualPeers.push_back(a);
            if (c == std::string::npos) break; pos = c + 1;
        }
    }
    LOG("[net] %zu pares por codigo", g_manualPeers.size());
}
void StartNet()
{
    if (g_started || !NetOn()) return;
    g_started = true;
    g_m.unlock(); NetOpen(); g_m.lock();   // NetOpen no toma g_m (desde el juego se llega con g_m tomado)
    LoadManualPeers();
}

void SignalGuestEvent(uint32_t h) { if (h) if (auto o = GetHandleObject(h)) if (o->waitable) SetEvent(o->waitable); }

// sockaddr del juego: familia (BE16), puerto (BE16), direccion (BE32)
void WriteSockaddr(uint32_t p, uint32_t vaddr, uint16_t port) { if (!p) return; memset(g_base + p, 0, 16); Store16(p, 2); Store16(p + 2, port); Store32(p + 4, vaddr); }

int DeliverLocal(uint16_t dstPort, uint16_t srcPort, const uint8_t* data, uint32_t len)
{
    for (auto& [h, s] : g_socks) if (s.bound && s.port == dstPort) { s.q.push_back({ kSelfVaddr, srcPort, std::vector<uint8_t>(data, data + len) }); g_cv.notify_all(); break; }
    return (int)len;
}
} // namespace


// ---- codigo de sala y funciones para el launcher (sesion 27) ----
// Codigo = IP publica (4) + puerto (2) + control (1) en base 32 de Crockford: 12 caracteres "XXXX-XXXX-XXXX".
static const char kB32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
std::string NetCodeEncode(uint32_t ip, uint16_t port)
{
    uint8_t b[7] = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip, (uint8_t)(port >> 8), (uint8_t)port, 0 };
    uint8_t c = 0xA5; for (int i = 0; i < 6; i++) c = (uint8_t)((c << 1 | c >> 7) ^ b[i]); b[6] = c;
    uint64_t v = 0; for (int i = 0; i < 7; i++) v = (v << 8) | b[i];   // 56 bits -> 12 x 5 bits (4 de relleno arriba)
    std::string out;
    for (int i = 11; i >= 0; i--) { out += kB32[(v >> (i * 5)) & 31]; if (i == 8 || i == 4) out += '-'; }
    return out;
}
bool NetCodeDecode(const std::string& code, uint32_t& ip, uint16_t& port)
{
    uint64_t v = 0; int n = 0;
    for (char ch : code)
    {
        char c = (char)toupper((unsigned char)ch);
        if (c == '-' || c == ' ') continue;
        if (c == 'O') c = '0'; if (c == 'I' || c == 'L') c = '1';
        const char* f = strchr(kB32, c); if (!f || !c) return false;
        v = (v << 5) | (uint64_t)(f - kB32); n++;
    }
    if (n != 12) return false;
    uint8_t b[7]; for (int i = 6; i >= 0; i--) { b[i] = (uint8_t)v; v >>= 8; }
    uint8_t c = 0xA5; for (int i = 0; i < 6; i++) c = (uint8_t)((c << 1 | c >> 7) ^ b[i]);
    if (c != b[6]) return false;
    ip = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; port = (uint16_t)((b[4] << 8) | b[5]);
    return ip != 0 && port != 0;
}
// Codigos (o ip:puerto) separados por comas/espacios -> "ip:puerto,ip:puerto" para GH6_NETPEER
std::string NetPeersFromCodes(const std::string& codes)
{
    std::string out, cur;
    auto flush = [&] {
        if (cur.empty()) return;
        uint32_t ip; uint16_t port; std::string e;
        if (NetCodeDecode(cur, ip, port)) e = IpStr(ip) + ":" + std::to_string(port);
        else { in_addr a{}; std::string h = cur.substr(0, cur.find(':')); if (inet_pton(AF_INET, h.c_str(), &a) == 1) e = cur; }
        if (!e.empty()) out += (out.empty() ? "" : ",") + e;
        cur.clear();
    };
    for (char c : codes) { if (c == ',' || c == ';' || c == '\n' || c == '\r') flush(); else if (c != ' ' && c != '\t') cur += c; }
    flush();
    return out;
}
// Launcher: abre la red (mismo puerto que usara el juego en este proceso) y busca la salida a internet
void NetLauncherStart() { NetOpen(); }
// Devuelve true cuando ya se sabe todo. code = codigo de sala (vacio si no hay IP publica); status = texto para el usuario
bool NetLauncherInfo(std::string& code, std::wstring& status)
{
    std::lock_guard l(g_m);
    if (!g_opened || g_mux == INVALID_SOCKET) { code.clear(); status = L"No se pudo abrir el puerto UDP de red."; return true; }
    if (!g_inet.done) { code.clear(); status = L"Comprobando la conexión a internet..."; return false; }
    code = g_inet.pubIp ? NetCodeEncode(g_inet.pubIp, g_inet.pubPort) : "";
    wchar_t t[400];
    if (!g_inet.pubIp) swprintf(t, 400, L"Sin acceso a internet detectado: solo red local (o LAN virtual). Puerto UDP %u.", g_muxPort);
    else if (g_inet.cgnat) swprintf(t, 400, L"Tu proveedor usa CGNAT: puede que no te puedan conectar. Si falla, usad una LAN virtual (Radmin VPN, ZeroTier o Tailscale).");
    else if (g_inet.upnp) swprintf(t, 400, L"Router: puerto UDP %u abierto automáticamente (UPnP). Basta con que tu amigo ponga tu código.", g_muxPort);
    else swprintf(t, 400, L"Router sin UPnP: poned los dos el código del otro (o abre el puerto UDP %u en el router).", g_muxPort);
    status = t;
    return true;
}

// ---- Winsock del Xbox (el primer argumento es el tipo de llamador XNCALLER, salvo en WSAGetLastError/__WSAFDIsSet/inet_addr) ----
GUEST_IMPL(NetDll_WSAStartup) { NETARGS("WSAStartup"); if (NetOn()) { std::lock_guard l(g_m); StartNet(); } ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_WSACleanup) { NETARGS("WSACleanup"); ctx.r3.u64 = 0; }
// socket(caller, af, type, protocol): protocolo 254 = VDP (datos de juego + voz), 17 = UDP, 6 = TCP
GUEST_IMPL(NetDll_socket)
{
    NETARGS("socket");
    if (!NetOn()) { ctx.r3.u64 = 0xFFFFFFFF; return; }
    std::lock_guard l(g_m); StartNet();
    if (ctx.r5.u32 != 2) { t_lastError = WSAE_OPNOTSUPP; ctx.r3.u64 = 0xFFFFFFFF; NETLOG("[net]   socket de tipo %u no soportado", ctx.r5.u32); return; }
    uint32_t h = g_nextSock++;
    GSock s; s.type = ctx.r5.u32; s.proto = ctx.r6.u32;
    g_socks[h] = s;
    ctx.r3.u64 = h;
}
GUEST_IMPL(NetDll_closesocket) { NETARGS("closesocket"); std::lock_guard l(g_m); g_socks.erase(ctx.r4.u32); ctx.r3.u64 = 0; }
// ioctlsocket(caller, s, cmd, argp): FIONBIO 0x8004667E, FIONREAD 0x4004667F
GUEST_IMPL(NetDll_ioctlsocket)
{
    NETARGS("ioctlsocket");
    if (!NetOn()) { ctx.r3.u64 = 0xFFFFFFFF; return; }
    std::lock_guard l(g_m);
    auto it = g_socks.find(ctx.r4.u32);
    if (it == g_socks.end()) { t_lastError = WSAE_NOTSOCK; ctx.r3.u64 = 0xFFFFFFFF; return; }
    if (ctx.r5.u32 == 0x8004667E && ctx.r6.u32) it->second.nonblock = Load32(ctx.r6.u32) != 0;
    else if (ctx.r5.u32 == 0x4004667F && ctx.r6.u32) Store32(ctx.r6.u32, it->second.q.empty() ? 0 : (uint32_t)it->second.q.front().data.size());
    ctx.r3.u64 = 0;
}
// setsockopt(caller, s, level, optname, optval, optlen): SO_RCVTIMEO (0xFFFF, 0x1006)
GUEST_IMPL(NetDll_setsockopt)
{
    NETARGS("setsockopt");
    std::lock_guard l(g_m);
    auto it = g_socks.find(ctx.r4.u32);
    if (it != g_socks.end() && ctx.r5.u32 == 0xFFFF && ctx.r6.u32 == 0x1006 && ctx.r7.u32) it->second.rcvTimeoutMs = Load32(ctx.r7.u32);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_getsockopt)
{
    NETARGS("getsockopt");
    if (ctx.r7.u32 && ctx.r8.u32) { uint32_t n = Load32(ctx.r8.u32); if (n >= 4) Store32(ctx.r7.u32, 0); }
    ctx.r3.u64 = 0;
}
// bind(caller, s, sockaddr, len)
GUEST_IMPL(NetDll_bind)
{
    NETARGS("bind");
    if (!NetOn()) { ctx.r3.u64 = 0xFFFFFFFF; return; }
    std::lock_guard l(g_m);
    auto it = g_socks.find(ctx.r4.u32);
    if (it == g_socks.end() || !ctx.r5.u32) { t_lastError = WSAE_NOTSOCK; ctx.r3.u64 = 0xFFFFFFFF; return; }
    uint16_t port = Load16(ctx.r5.u32 + 2);
    if (!port) port = g_nextEphemeral++;
    it->second.port = port; it->second.bound = true;
    NETLOG("[net]   socket %X -> puerto del juego %u", ctx.r4.u32, port);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_connect)
{
    NETARGS("connect");
    std::lock_guard l(g_m);
    auto it = g_socks.find(ctx.r4.u32);
    if (it == g_socks.end() || !ctx.r5.u32) { t_lastError = WSAE_NOTSOCK; ctx.r3.u64 = 0xFFFFFFFF; return; }
    it->second.connVaddr = Load32(ctx.r5.u32 + 4); it->second.connPort = Load16(ctx.r5.u32 + 2);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_listen) { NETARGS("listen"); t_lastError = WSAE_OPNOTSUPP; ctx.r3.u64 = 0xFFFFFFFF; }
GUEST_IMPL(NetDll_accept) { NETARGS("accept"); t_lastError = WSAE_OPNOTSUPP; ctx.r3.u64 = 0xFFFFFFFF; }

namespace
{
// x_fd_set: {u32 fd_count; u32 fd_array[64]}
bool SockReadable(uint32_t h) { auto it = g_socks.find(h); return it != g_socks.end() && !it->second.q.empty(); }
}
// select(caller, nfds, readfds, writefds, exceptfds, timeout{sec, usec})
GUEST_IMPL(NetDll_select)
{
    uint32_t rd = ctx.r5.u32, wr = ctx.r6.u32, ex = ctx.r7.u32, to = ctx.r8.u32;
    std::unique_lock l(g_m);
    auto count = [&]() {
        int n = 0;
        if (rd) { uint32_t c = std::min<uint32_t>(Load32(rd), 64); for (uint32_t i = 0; i < c; i++) if (SockReadable(Load32(rd + 4 + i * 4))) n++; }
        return n;
    };
    int64_t waitMs = to ? int64_t(Load32(to)) * 1000 + Load32(to + 4) / 1000 : -1;
    if (count() == 0 && waitMs != 0)
    {
        auto pred = [&] { return count() > 0; };
        if (waitMs < 0) while (!g_cv.wait_for(l, std::chrono::milliseconds(100), pred)) {}
        else g_cv.wait_for(l, std::chrono::milliseconds(waitMs), pred);
    }
    int total = 0;
    if (rd)
    {
        uint32_t c = std::min<uint32_t>(Load32(rd), 64), k = 0;
        for (uint32_t i = 0; i < c; i++) { uint32_t h = Load32(rd + 4 + i * 4); if (SockReadable(h)) Store32(rd + 4 + (k++) * 4, h); }
        Store32(rd, k); total += k;
    }
    if (wr) total += Load32(wr);   // siempre se puede escribir
    if (ex) Store32(ex, 0);
    ctx.r3.u64 = (uint32_t)total;
}

namespace
{
// Saca un paquete del socket (espera si es bloqueante). Devuelve bytes, o -1 con t_lastError.
int RecvCommon(uint32_t h, uint32_t buf, uint32_t len, uint32_t from, uint32_t fromLen)
{
    std::unique_lock l(g_m);
    auto it = g_socks.find(h);
    if (it == g_socks.end()) { t_lastError = WSAE_NOTSOCK; return -1; }
    if (it->second.q.empty())
    {
        if (it->second.nonblock) { t_lastError = WSAE_WOULDBLOCK; return -1; }
        uint32_t tmo = it->second.rcvTimeoutMs;
        auto pred = [&] { auto j = g_socks.find(h); return j == g_socks.end() || !j->second.q.empty(); };
        if (tmo) { if (!g_cv.wait_for(l, std::chrono::milliseconds(tmo), pred)) { t_lastError = WSAE_TIMEDOUT; return -1; } }
        else while (!g_cv.wait_for(l, std::chrono::milliseconds(100), pred)) {}
        it = g_socks.find(h);
        if (it == g_socks.end()) { t_lastError = WSAE_NOTSOCK; return -1; }
    }
    Pkt p = std::move(it->second.q.front()); it->second.q.pop_front();
    uint32_t n = std::min<uint32_t>(len, (uint32_t)p.data.size());
    if (buf && n) memcpy(g_base + buf, p.data.data(), n);
    if (from) WriteSockaddr(from, p.fromVaddr, p.fromPort);
    if (fromLen) Store32(fromLen, 16);
    return (int)n;
}
int SendCommon(uint32_t h, uint32_t buf, uint32_t len, uint32_t vaddr, uint16_t port, bool useConn)
{
    std::lock_guard l(g_m);
    auto it = g_socks.find(h);
    if (it == g_socks.end()) { t_lastError = WSAE_NOTSOCK; return -1; }
    GSock& s = it->second;
    if (!s.bound) { s.port = g_nextEphemeral++; s.bound = true; }
    if (useConn) { vaddr = s.connVaddr; port = s.connPort; }
    const uint8_t* data = g_base + buf;
    if (vaddr == kSelfVaddr) return DeliverLocal(port, s.port, data, len);
    Peer* p = PeerByVaddr(vaddr);
    if (!p || !p->ep.sin_port)
    {
        static int warned = 0; if (warned++ < 20) NETLOG("[net]   envio a %08X:%u sin par conocido (descartado)", vaddr, port);
        return (int)len;
    }
    auto b = Header(T_DATA);
    Put16(b, port); Put16(b, s.port);
    b.insert(b.end(), data, data + len);
    SendRaw(b, p->ep);
    return (int)len;
}
}
GUEST_IMPL(NetDll_recv) { int r = RecvCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0); ctx.r3.u64 = (uint32_t)r; }
// recvfrom(caller, s, buf, len, flags, from, fromlen)
GUEST_IMPL(NetDll_recvfrom) { int r = RecvCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32, ctx.r9.u32); ctx.r3.u64 = (uint32_t)r; }
GUEST_IMPL(NetDll_send) { int r = SendCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, true); ctx.r3.u64 = (uint32_t)r; }
// sendto(caller, s, buf, len, flags, to, tolen)
GUEST_IMPL(NetDll_sendto)
{
    uint32_t to = ctx.r8.u32;
    int r = SendCommon(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, to ? Load32(to + 4) : 0, to ? Load16(to + 2) : 0, to == 0);
    ctx.r3.u64 = (uint32_t)r;
}
// inet_addr(cadena): en el Xbox (big endian) el resultado en orden de red es el numero de la IP tal cual
GUEST_IMPL(NetDll_inet_addr)
{
    NETARGS("inet_addr");
    in_addr a{};
    const char* s = ctx.r3.u32 ? (const char*)(g_base + ctx.r3.u32) : "";
    ctx.r3.u64 = (*s && inet_pton(AF_INET, s, &a) == 1) ? ntohl(a.s_addr) : (*s ? 0xFFFFFFFF : 0);
}
GUEST_IMPL(NetDll_WSAGetLastError) { ctx.r3.u64 = (uint32_t)t_lastError; }
// __WSAFDIsSet(s, set)
GUEST_IMPL(NetDll___WSAFDIsSet)
{
    uint32_t set = ctx.r4.u32, c = set ? std::min<uint32_t>(Load32(set), 64) : 0, r = 0;
    for (uint32_t i = 0; i < c; i++) if (Load32(set + 4 + i * 4) == ctx.r3.u32) r = 1;
    ctx.r3.u64 = r;
}

// ---- XNet ----
GUEST_IMPL(NetDll_XNetStartup) { NETARGS("XNetStartup"); if (NetOn()) { std::lock_guard l(g_m); StartNet(); } ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetCleanup) { NETARGS("XNetCleanup"); ctx.r3.u64 = 0; }
// XNetRandom(caller, buffer, size)
GUEST_IMPL(NetDll_XNetRandom)
{
    NETARGS("XNetRandom");
    std::lock_guard l(g_m);
    for (uint32_t i = 0; i < ctx.r5.u32 && ctx.r4.u32; i++) g_base[ctx.r4.u32 + i] = (uint8_t)g_rng();
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetUnregisterKey) { NETARGS("XNetUnregisterKey"); ctx.r3.u64 = 0; }
// XNetXnAddrToInAddr(caller, XNADDR*, XNKID*, IN_ADDR* out)
GUEST_IMPL(NetDll_XNetXnAddrToInAddr)
{
    NETARGS("XNetXnAddrToInAddr");
    if (!NetOn() || !ctx.r4.u32 || !ctx.r6.u32) { ctx.r3.u64 = NetOn() ? WSAE_INVAL : 0; return; }
    std::lock_guard l(g_m);
    const uint8_t* xna = g_base + ctx.r4.u32;
    if (!memcmp(xna + 10, g_myId, 6)) { Store32(ctx.r6.u32, kSelfVaddr); ctx.r3.u64 = 0; return; }
    Peer& p = AddPeer(xna + 10);
    SetPeerXna(p, xna);
    if (ctx.r5.u32) memcpy(p.kid, g_base + ctx.r5.u32, 8);
    Store32(ctx.r6.u32, p.vaddr);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetServerToInAddr) { NETARGS("XNetServerToInAddr"); ctx.r3.u64 = NetOn() ? WSAE_INVAL : 0; }   // sin servidores
GUEST_IMPL(NetDll_XNetTsAddrToInAddr) { NETARGS("XNetTsAddrToInAddr"); ctx.r3.u64 = NetOn() ? WSAE_INVAL : 0; }
// XNetInAddrToXnAddr(caller, IN_ADDR, XNADDR* out, XNKID* out)
GUEST_IMPL(NetDll_XNetInAddrToXnAddr)
{
    if (!NetOn()) { ctx.r3.u64 = 0; return; }
    std::lock_guard l(g_m);
    uint32_t v = ctx.r4.u32;
    if (v == kSelfVaddr) { if (ctx.r5.u32) memcpy(g_base + ctx.r5.u32, g_myXna, 36); ctx.r3.u64 = 0; return; }
    Peer* p = PeerByVaddr(v);
    if (!p || !p->haveXna) { ctx.r3.u64 = WSAE_INVAL; return; }
    if (ctx.r5.u32) memcpy(g_base + ctx.r5.u32, p->xna, 36);
    if (ctx.r6.u32) memcpy(g_base + ctx.r6.u32, p->kid, 8);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetInAddrToString) { NETARGS("XNetInAddrToString"); ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetUnregisterInAddr) { NETARGS("XNetUnregisterInAddr"); ctx.r3.u64 = 0; }
// XNetConnect(caller, IN_ADDR): presentacion con el par
GUEST_IMPL(NetDll_XNetConnect)
{
    NETARGS("XNetConnect");
    if (NetOn()) { std::lock_guard l(g_m); if (Peer* p = PeerByVaddr(ctx.r4.u32)) SendHello(*p, T_HELLO); }
    ctx.r3.u64 = 0;
}
// XNetGetConnectStatus(caller, ina): 0 inactiva, 1 pendiente, 2 conectada, 3 perdida
GUEST_IMPL(NetDll_XNetGetConnectStatus)
{
    if (!NetOn()) { ctx.r3.u64 = NetFake() ? 2 : 0; return; }
    std::lock_guard l(g_m);
    if (ctx.r4.u32 == kSelfVaddr) { ctx.r3.u64 = 2; return; }
    Peer* p = PeerByVaddr(ctx.r4.u32);
    if (!p) { ctx.r3.u64 = 0; return; }
    if (!p->connected && !p->helloSent) SendHello(*p, T_HELLO);
    ctx.r3.u64 = p->connected ? 2 : 1;
}
// XNetDnsLookup(caller, host, event, XNDNS** out): sin DNS (como Xenia: estado de error)
GUEST_IMPL(NetDll_XNetDnsLookup)
{
    NETARGS("XNetDnsLookup");
    if (ctx.r6.u32) { uint32_t d = GuestAllocKernel(0x28, 0x10); memset(g_base + d, 0, 0x28); Store32(d, 1); Store32(ctx.r6.u32, d); }
    SignalGuestEvent(ctx.r5.u32);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetDnsRelease) { NETARGS("XNetDnsRelease"); ctx.r3.u64 = 0; }
// XNetQosListen(caller, XNKID*, data, size, bitsPerSec, flags): 1 activar, 2 desactivar, 4 datos, 8 ancho de banda, 0x10 liberar
GUEST_IMPL(NetDll_XNetQosListen)
{
    NETARGS("XNetQosListen");
    if (NetOn() && ctx.r4.u32)
    {
        std::lock_guard l(g_m);
        uint64_t k = Kid64(g_base + ctx.r4.u32);
        uint32_t fl = ctx.r8.u32;
        if (fl & 0x4) g_qosData[k] = std::vector<uint8_t>(g_base + ctx.r5.u32, g_base + ctx.r5.u32 + (ctx.r5.u32 ? ctx.r6.u32 : 0));
        if (fl & 0x10) g_qosData.erase(k);
    }
    ctx.r3.u64 = 0;
}
// XNetQosLookup(caller, cxna, apxna, apxnkid, apxnkey, cina, aina, adwServiceId, [pila] cProbes, bitsPerSec, flags, hEvent, XNQOS**)
// XNQOS = {cxnqos, cxnqosPending, XNQOSINFO[cxnqos]}; XNQOSINFO (24 bytes) = flags, reservado, probes enviados (16), recibidos (16),
// tamano de datos (16), puntero a datos, rtt minimo (16), rtt medio (16), subida (32), bajada (32)
GUEST_IMPL(NetDll_XNetQosLookup)
{
    NETARGS("XNetQosLookup");
    uint32_t sp = ctx.r1.u32;
    uint32_t cxna = ctx.r4.u32, apxna = ctx.r5.u32, cProbes = Load32(sp + 0x54), ev = Load32(sp + 0x6C), out = Load32(sp + 0x74);
    std::lock_guard l(g_m);
    uint32_t n = std::max<uint32_t>(cxna, 1), dataBytes = 0;
    std::vector<const Advert*> ads(cxna, nullptr);
    std::vector<Peer*> peers(cxna, nullptr);
    for (uint32_t i = 0; i < cxna && apxna; i++)
    {
        uint32_t xp = Load32(apxna + i * 4); if (!xp) continue;
        const uint8_t* xna = g_base + xp;
        peers[i] = PeerById(xna + 10);
        for (auto& a : g_adverts) if (!memcmp(a.peerId, xna + 10, 6)) { ads[i] = &a; }
        if (ads[i]) dataBytes += ((uint32_t)ads[i]->qos.size() + 3) & ~3u;
    }
    uint32_t q = GuestAllocKernel(8 + 24 * n + dataBytes + 16, 0x10);
    memset(g_base + q, 0, 8 + 24 * n);
    Store32(q, cxna); Store32(q + 4, 0);
    uint32_t dp = q + 8 + 24 * n;
    for (uint32_t i = 0; i < cxna; i++)
    {
        uint32_t e = q + 8 + i * 24;
        bool contacted = NetOn() && peers[i] && (ads[i] || peers[i]->connected);
        g_base[e] = contacted ? (0x01 | 0x02 | (ads[i] && !ads[i]->qos.empty() ? 0x08 : 0)) : 0x01;   // completo | contactado | datos
        Store16(e + 2, (uint16_t)cProbes); Store16(e + 4, contacted ? (uint16_t)cProbes : 0);
        if (contacted && ads[i] && !ads[i]->qos.empty())
        {
            Store16(e + 6, (uint16_t)ads[i]->qos.size()); Store32(e + 8, dp);
            memcpy(g_base + dp, ads[i]->qos.data(), ads[i]->qos.size()); dp += ((uint32_t)ads[i]->qos.size() + 3) & ~3u;
        }
        uint16_t rtt = contacted ? (uint16_t)peers[i]->rttMs : 0;
        Store16(e + 12, rtt); Store16(e + 14, rtt);
        Store32(e + 16, contacted ? 10000000 : 0); Store32(e + 20, contacted ? 10000000 : 0);
    }
    if (out) Store32(out, q);
    SignalGuestEvent(ev);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetQosRelease) { NETARGS("XNetQosRelease"); ctx.r3.u64 = 0; }
// NetDll_XNetGetTitleXnAddr(caller, XNADDR*): XNADDR = ina(4) inaOnline(4) wPortOnline(2) abEnet[6] abOnline[20]
GUEST_IMPL(NetDll_XNetGetTitleXnAddr)
{
    NETARGS("XNetGetTitleXnAddr");
    uint32_t a = ctx.r4.u32;
    if (NetOn()) { std::lock_guard l(g_m); StartNet(); if (a) memcpy(g_base + a, g_myXna, 36); ctx.r3.u64 = 0x02 | 0x04 | 0x20 | 0x40 | 0x80; return; }
    if (a)
    {
        memset(g_base + a, 0, 36);
        Store32(a, NetFake() ? 0xC0A80164 : 0x7F000001);
        if (NetFake()) { Store32(a + 4, 0xC0A80164); Store16(a + 8, 3074); }
        memset(g_base + a + 10, 0xCC, 6);
    }
    ctx.r3.u64 = NetFake() ? 0x02 | 0x04 | 0x20 | 0x40 | 0x80 : 0x04;   // ETHERNET|STATIC|GATEWAY|DNS|ONLINE
}
// XNetGetEthernetLinkStatus(caller): 1 activo | 2 100 Mbps | 8 duplex completo
GUEST_IMPL(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = NetFake() ? 0x0B : 0; }
GUEST_IMPL(XNetLogonGetTitleID) { NETARGS("XNetLogonGetTitleID"); ctx.r3.u64 = NetFake() ? kTitleId : 0; }

// ---- sesiones ----
// XamSessionCreateHandle(handle*): objeto de sesion propio; XamSessionRefObjByHandle(handle, obj*) da su puntero (el que llega
// despues en los mensajes XSession como primer argumento)
GUEST_IMPL(XamSessionCreateHandle)
{
    NETARGS("XamSessionCreateHandle");
    auto o = std::make_shared<KObject>();
    o->guestPtr = GuestAllocKernel(0x100, 0x10);
    memset(g_base + o->guestPtr, 0, 0x100);
    uint32_t h = InsertHandle(o);
    if (ctx.r3.u32) Store32(ctx.r3.u32, h);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(XamSessionRefObjByHandle)
{
    NETARGS("XamSessionRefObjByHandle");
    auto o = GetHandleObject(ctx.r3.u32);
    if (ctx.r4.u32) Store32(ctx.r4.u32, o ? o->guestPtr : 0);
    ctx.r3.u64 = o ? 0 : 6;
}

// Mensajes XGI (app 0xFB) de contexto, propiedades y sesiones. Lo llama XMsgStartIORequest(Ex) de xam.cpp; devuelve true si lo
// ha tratado (result = resultado del overlapped).
bool NetXMsg(uint32_t app, uint32_t msg, uint32_t buf, uint32_t size, uint32_t& result)
{
    if (app != 0xFB || !buf) return false;
    result = 0;
    std::lock_guard l(g_m);
    switch (msg)
    {
    case 0x000B0006: if (size >= 24) g_ctx[Load32(buf + 16)] = Load32(buf + 20); return false;   // XUserSetContext
    case 0x000B0007:                                                                               // XUserSetProperty
        if (size >= 28) { uint32_t sz = std::min<uint32_t>(Load32(buf + 20), 256), vp = Load32(buf + 24); if (vp) g_prop[Load32(buf + 16)] = std::vector<uint8_t>(g_base + vp, g_base + vp + sz); }
        return false;
    }
    if (!NetOn() || msg < 0x000B0010 || msg > 0x000B003F) return false;
    StartNet();
    uint32_t obj = Load32(buf);
    switch (msg)
    {
    case 0x000B0010:   // XSessionCreate {obj, flags, publicas, privadas, usuario, XSESSION_INFO*, nonce*}
    {
        Sess s; s.obj = obj; s.flags = Load32(buf + 4); s.pub = Load32(buf + 8); s.priv = Load32(buf + 12);
        uint32_t info = Load32(buf + 20), nonce = Load32(buf + 24);
        s.host = (s.flags & 1) != 0;
        if (s.host)
        {
            uint64_t k = g_rng();
            for (int i = 0; i < 8; i++) s.info[i] = (uint8_t)(k >> (i * 8));
            s.info[0] = (uint8_t)(0x80 | (s.info[0] & 0x0F));   // XNKID de par en linea
            memcpy(s.info + 8, g_myXna, 36);
            uint64_t k1 = g_rng(), k2 = g_rng();
            for (int i = 0; i < 8; i++) { s.info[44 + i] = (uint8_t)(k1 >> (i * 8)); s.info[52 + i] = (uint8_t)(k2 >> (i * 8)); }
            if (info) memcpy(g_base + info, s.info, 60);
        }
        else if (info) memcpy(s.info, g_base + info, 60);
        if (nonce) { uint64_t nn = g_rng(); Store32(nonce, (uint32_t)(nn >> 32)); Store32(nonce + 4, (uint32_t)nn); }
        g_sess[obj] = s;
        NETLOG("[net] XSessionCreate obj %08X flags %X %s, kid %s, %u+%u plazas", obj, s.flags, s.host ? "ANFITRION" : "unirse", Hex(s.info, 8).c_str(), s.pub, s.priv);
        if (!s.host && (s.flags & 0x8) && !g_matchTick) g_matchTick = NowMs();
        if (!s.host)
        {   // al unirse: presentacion con el anfitrion
            Peer& p = AddPeer(s.info + 18);
            SetPeerXna(p, s.info + 8); memcpy(p.kid, s.info, 8);
            SendHello(p, T_HELLO);
        }
        return true;
    }
    case 0x000B0011: NETLOG("[net] XSessionDelete obj %08X", obj); g_sess.erase(obj); return true;
    case 0x000B0012:   // XSessionJoinLocal {obj, usuarios, ?, indices*, privadas*}
    {
        // mismo mensaje para jugadores locales (indices en +12) y remotos (XUIDs en +8)
        auto it = g_sess.find(obj);
        bool remote = Load32(buf + 8) != 0;
        if (it != g_sess.end())
        {
            it->second.filledPub += Load32(buf + 4);
            if (remote && (it->second.flags & 0x8) && !g_matchTick) g_matchTick = NowMs();
        }
        NETLOG("[net] XSessionJoin%s obj %08X usuarios %u", remote ? "Remote" : "Local", obj, Load32(buf + 4));
        return true;
    }
    case 0x000B0018:   // XSessionModify {obj, flags, publicas, privadas}
    {
        auto it = g_sess.find(obj);
        if (it != g_sess.end()) { it->second.flags = Load32(buf + 4); it->second.pub = Load32(buf + 8); it->second.priv = Load32(buf + 12); }
        NETLOG("[net] XSessionModify obj %08X flags %X", obj, Load32(buf + 4));
        return true;
    }
    case 0x000B001C:   // XSessionSearchEx {proc, usuario, max resultados, (props << 16) | contextos, props*, contextos*, bytes, resultados*, usuarios}
    {
        uint32_t maxRes = Load32(buf + 8), np = Load32(buf + 12) >> 16, nc = Load32(buf + 12) & 0xFFFF;
        uint32_t sprops = Load32(buf + 16), sctx = Load32(buf + 20), cap = Load32(buf + 24), res = Load32(buf + 28);
        uint64_t now = NowMs();
        if (now >= g_searchUntil || now - g_lastQuery > 1500)
        {   // primera busqueda (o tras una pausa): se pregunta y se esperan las respuestas un momento
            SendQuery();
            g_m.unlock(); Sleep(400); g_m.lock();
            now = NowMs();
        }
        g_searchUntil = now + 8000;
        static int logged = 0;
        if (logged++ < 2)
        {
            for (uint32_t i = 0; i < nc && sctx; i++) NETLOG("[net]   buscar contexto %08X = %u", Load32(sctx + i * 8), Load32(sctx + i * 8 + 4));
            for (uint32_t i = 0; i < np && sprops; i++) NETLOG("[net]   buscar propiedad %08X tipo %u valor %08X%08X", Load32(sprops + i * 24), g_base[sprops + i * 24 + 8], Load32(sprops + i * 24 + 16), Load32(sprops + i * 24 + 20));
        }
        // resultados: cabecera {cuantos, puntero} + XSESSION_SEARCHRESULT[] (0x5C bytes) + propiedades (24) / contextos (8) / datos
        std::vector<const Advert*> ok;
        for (auto& a : g_adverts)
        {
            if (now - a.seen > 6000 || a.filledPub >= a.pub) continue;
            bool match = true;
            for (uint32_t i = 0; i < nc && sctx; i++)
            {
                uint32_t id = Load32(sctx + i * 8), v = Load32(sctx + i * 8 + 4);
                for (auto& c : a.ctx) if (c.first == id && c.second != v) match = false;
            }
            if (!match) { static int w = 0; if (w++ < 10) NETLOG("[net]   partida %s descartada: contextos distintos", Hex(a.info, 8).c_str()); }
            if (match && ok.size() < maxRes) ok.push_back(&a);
        }
        if (res && cap >= 8)
        {
            uint32_t arr = res + 8, extra = arr + 0x5C * (uint32_t)ok.size(), lim = res + cap, count = 0;
            for (auto* a : ok)
            {
                uint32_t need = 0x5C + 24 * (uint32_t)a->props.size() + 8 * (uint32_t)a->ctx.size() + 512;
                if (extra + need > lim) break;
                uint32_t r = arr + count * 0x5C;
                memcpy(g_base + r, a->info, 60);
                Store32(r + 60, a->pub - a->filledPub); Store32(r + 64, a->priv > a->filledPriv ? a->priv - a->filledPriv : 0);
                Store32(r + 68, a->filledPub); Store32(r + 72, a->filledPriv);
                Store32(r + 76, (uint32_t)a->props.size()); Store32(r + 80, (uint32_t)a->ctx.size());
                uint32_t pp = extra; extra += 24 * (uint32_t)a->props.size();
                uint32_t cp = extra; extra += 8 * (uint32_t)a->ctx.size();
                Store32(r + 84, a->props.empty() ? 0 : pp); Store32(r + 88, a->ctx.empty() ? 0 : cp);
                for (auto& [id, v] : a->props)
                {   // XUSER_PROPERTY: id, relleno, XUSER_DATA {tipo, relleno, valor (8) | (tamano, puntero)}
                    memset(g_base + pp, 0, 24);
                    uint8_t t = (uint8_t)((id >> 28) & 0xF);
                    Store32(pp, id); g_base[pp + 8] = t;
                    if (t == 4 || t == 6) { Store32(pp + 16, (uint32_t)v.size()); Store32(pp + 20, extra); memcpy(g_base + extra, v.data(), v.size()); extra += ((uint32_t)v.size() + 3) & ~3u; }
                    else memcpy(g_base + pp + 16, v.data(), std::min<size_t>(8, v.size()));
                    pp += 24;
                }
                for (auto& c : a->ctx) { Store32(cp, c.first); Store32(cp + 4, c.second); cp += 8; }
                count++;
            }
            Store32(res, count); Store32(res + 4, count ? arr : 0);
            if (count) NETLOG("[net] XSessionSearchEx -> %u partidas", count);
        }
        return true;
    }
    default:
    {
        char t[200]; int k = 0;
        for (uint32_t i = 0; i + 4 <= size && i < 40; i += 4) k += snprintf(t + k, sizeof(t) - k, " %08X", Load32(buf + i));
        NETLOG("[net] XGI 0x%08X sin tratar (%u bytes):%s", msg, size, t);
        // XSessionJoinRemote / LeaveRemote (por deducir): {obj, cuantos, xuids*, privadas*}
        return true;
    }
    }
}

// XUID y nombre del jugador 1. Con la red P2P cada copia necesita un XUID distinto (el juego compara jugadores por XUID):
// formato de XUID en linea (0x0009...) a partir del nombre del PC y la carpeta de datos. GH6_NOMBRE = nombre (gamertag).
uint64_t NetUserXuid(uint64_t offlineXuid)
{
    if (!NetOn()) return offlineXuid;
    static const uint64_t x = [] {
        char pc[64] = {}; DWORD n = sizeof(pc); GetComputerNameA(pc, &n);
        char cwd[MAX_PATH] = {}; GetCurrentDirectoryA(MAX_PATH, cwd);
        uint64_t h = 1469598103934665603ull;
        for (const char* s : { (const char*)pc, (const char*)cwd }) for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
    }();
    return x;
}
uint64_t NetMatchTick() { return g_matchTick; }
const char* NetUserName()
{   // GH6_NOMBRE (launcher: online_nombre) o el usuario de Windows; ASCII imprimible, maximo 15 caracteres (como un gamertag)
    static char n[16] = {};
    if (!n[0])
    {
        const char* e = getenv("GH6_NOMBRE");
        char un[64] = {}; DWORD ul = sizeof(un);
        if (!e || !*e) { GetUserNameA(un, &ul); e = un; }
        int k = 0;
        for (const char* q = e; *q && k < 15; q++) if ((unsigned char)*q >= 32 && (unsigned char)*q < 127) n[k++] = *q;
        if (!k) strcpy(n, "Player");
    }
    return n;
}

// ---- voz y amigos (sin implementar) ----
GUEST_IMPL(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
GUEST_IMPL(XamVoiceClose) { ctx.r3.u64 = 0; }
GUEST_IMPL(XamVoiceSubmitPacket) { ctx.r3.u64 = 0; }
GUEST_IMPL(XamUserAreUsersFriends) { NETARGS("XamUserAreUsersFriends"); ctx.r3.u64 = 0; }
GUEST_IMPL(XamShowFriendsUI) { NETARGS("XamShowFriendsUI"); ctx.r3.u64 = 0; }
GUEST_IMPL(XamShowGamerCardUIForXUID) { NETARGS("XamShowGamerCardUIForXUID"); ctx.r3.u64 = 0; }
GUEST_IMPL(XamShowPlayersUI) { NETARGS("XamShowPlayersUI"); ctx.r3.u64 = 0; }
GUEST_IMPL(XamUserGetMembershipTierFromXUID) { NETARGS("XamUserGetMembershipTierFromXUID"); ctx.r3.u64 = NetFake() ? 6 : 0; }   // 6 = Gold

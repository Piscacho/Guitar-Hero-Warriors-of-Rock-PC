// Backend D3D12 (sesiones 7-8): dispositivo, swapchain, render target "EDRAM", draws traducidos y presentacion.
// Todo se usa desde un unico hilo (el del procesador de comandos).
#include "gpu.h"
#include "gpu_draw.h"
#include "gpu_shader.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

using Microsoft::WRL::ComPtr;
void CmdTrace(const char* fmt, ...);
float DecodeDepth24(uint32_t v24, bool isFloat);
// float -> half (redondeo simple, sin desnormales)
static uint16_t FloatToHalf(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xFF) - 127 + 15; uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (((uint32_t)e << 10) + ((m + 0x1000) >> 13)));   // el acarreo del redondeo sube el exponente
}

extern bool g_xlatNoGrad;   // gpu_shader.cpp (sesion 24)
namespace
{
constexpr UINT kFrames = 2;
constexpr UINT kRtW = 1280, kRtH = 720;
constexpr UINT64 kRingSize = 64ull << 20; // por frame
constexpr UINT kDescPerDraw = 48;          // 16 SRV 2D + 16 SRV 3D + 16 SRV cubo
constexpr UINT kDescPerFrame = kDescPerDraw * 8192;  // 8192 draws por frame
ComPtr<ID3D12Device> g_dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGISwapChain3> g_swap;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_srvHeap;
ComPtr<ID3D12Resource> g_backbuffers[kFrames];
ComPtr<ID3D12CommandAllocator> g_alloc[kFrames];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValues[kFrames] = {}, g_nextFence = 1;
HANDLE g_fenceEvent = nullptr;
UINT g_rtvSize = 0, g_srvSize = 0;
ComPtr<ID3D12RootSignature> g_blitRootSig, g_drawRootSig;
ComPtr<ID3D12PipelineState> g_blitPso;
ComPtr<ID3D12Resource> g_fbTexture, g_fbUpload[kFrames];
UINT g_fbW = 0, g_fbH = 0;
ComPtr<ID3D12Resource> g_edram, g_white, g_ring[kFrames];
uint8_t* g_ringMap[kFrames] = {};
UINT64 g_ringOffset = 0;
struct VbCacheEntry { UINT64 off; uint64_t hash; };
std::unordered_map<uint64_t, VbCacheEntry> g_vbCache;   // (direccion, bytes) -> offset en el anillo del frame actual + huella del contenido
std::vector<ComPtr<ID3D12Resource>> g_deferredRelease[kFrames];   // recursos a liberar cuando la GPU termine ese frame
bool g_failed = false, g_frameOpen = false;
UINT g_fi = 0;
uint64_t g_frameDraws = 0;
uint64_t g_curPsHash = 0;   // PS del draw en curso (diagnostico)
UINT g_descCursor = 0;
uint64_t g_frameCounter = 0;
ComPtr<ID3DBlob> g_rectGs;
// ---- EDRAM (sesion 9): un RT por (base, pitch) ----
// Sesion 21: cada superficie tiene el formato de RB_COLOR_INFO (bits 16-19), no siempre BGRA8
struct EdramRt
{
    ComPtr<ID3D12Resource> res; UINT w = 0, h = 0, rtv = 0, base = 0;
    uint32_t xfmt = 0; DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM; UINT bpp = 4; float outScale = 0;
};
// Formato de color de Xenos -> DXGI. Las superficies 16_16 / 16_16_16_16 de Xenos son de punto fijo -32..32: SNORM con la salida escalada 1/32.
void EdramFormat(uint32_t xfmt, DXGI_FORMAT& f, UINT& bpp, float& scale)
{
    scale = 0; bpp = 4;
    switch (xfmt)
    {
    case 2: f = DXGI_FORMAT_R10G10B10A2_UNORM; break;                               // 2_10_10_10
    case 3: case 12: f = DXGI_FORMAT_R16G16B16A16_FLOAT; bpp = 8; break;            // 2_10_10_10_FLOAT (aprox.)
    case 4: f = DXGI_FORMAT_R16G16_SNORM; scale = 1.0f / 32; break;                 // 16_16
    case 5: f = DXGI_FORMAT_R16G16B16A16_SNORM; bpp = 8; scale = 1.0f / 32; break;  // 16_16_16_16
    case 6: f = DXGI_FORMAT_R16G16_FLOAT; break;                                    // 16_16_FLOAT
    case 7: f = DXGI_FORMAT_R16G16B16A16_FLOAT; bpp = 8; break;                     // 16_16_16_16_FLOAT
    case 14: f = DXGI_FORMAT_R32_FLOAT; break;                                      // 32_FLOAT
    case 15: f = DXGI_FORMAT_R32G32_FLOAT; bpp = 8; break;                          // 32_32_FLOAT
    default: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;                                 // 8888 y 8888_GAMMA
    }
}
std::unordered_map<uint32_t, EdramRt> g_edramRts;
EdramRt* g_boundRt = nullptr;
ComPtr<ID3D12Resource> g_readback;
uint64_t g_resolves = 0, g_fbResolves = 0;
constexpr UINT kEdramH = 1024;
// ---- profundidad (sesion 19): una superficie D32+S8 por (base de depth, pitch) ----
struct DepthRt { ComPtr<ID3D12Resource> res; UINT w = 0, h = 0, dsv = 0; };
std::unordered_map<uint32_t, DepthRt> g_depthRts;
ComPtr<ID3D12DescriptorHeap> g_dsvHeap;
UINT g_dsvSize = 0;
// Sesion 24: profundidad D24 (como la EDRAM de la 360) en vez de D32 float. El juego restaura la profundidad del frame anterior
// (palabra D24S8) y vuelve a dibujar personajes y luces con LEQUAL: con D32 float la rejilla de redondeo no coincidia con la de
// 24 bits y la prueba fallaba en columnas periodicas de 1 px (rayado fino en los personajes). GH6_DEPTH32=1 = antes.
static const bool g_depth32 = getenv("GH6_DEPTH32") != nullptr;
static const DXGI_FORMAT kDepthFmt = g_depth32 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
static const DXGI_FORMAT kDepthSrvFmt = g_depth32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

struct ShaderEntry { bool ok = false; ComPtr<ID3DBlob> blob; TranslatedShader info; };
std::unordered_map<uint64_t, ShaderEntry> g_shaders;
std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_psos;

// Copia de profundidad: constantes = origen en muestras (x, y) y paso de muestreo (msx, msy)
const char* kDepthCopyHlsl = R"(
cbuffer C : register(b0) { int4 off; };
Texture2D<float> D : register(t0);
struct O { float z : SV_Target0; float4 p : SV_Target1; };
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
O ps_main(float4 pos : SV_Position)
{
    int2 xy = int2(pos.xy) * off.zw + off.xy;
    float z = D.Load(int3(xy, 0));
    uint v = (uint)(saturate(z) * 16777215.0 + 0.5);
    O o; o.z = z; o.p = float4(0, v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
    return o;
}
)";
ComPtr<ID3D12RootSignature> g_depthCopyRootSig;
ComPtr<ID3D12PipelineState> g_depthCopyPso;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap2;
// Sesion 24 (rendimiento): resolve de color con MSAA por GPU. off.xy = origen en muestras, off.zw = muestras por pixel (msx, msy),
// mode.x = muestra/media (RB_COPY_CONTROL bits 4-6: 4 = media de 0/1, 5 = media de 2/3, 6 = media de las 4; <4 = muestra 0,
// como la ruta de CPU). Muestras en la superficie: (0,0) (1,0) (0,1) (1,1) dentro de cada bloque msx x msy.
const char* kColorResolveHlsl = R"(
cbuffer C : register(b0) { int4 off; int4 mode; };
Texture2D<float4> S : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps_main(float4 pos : SV_Position) : SV_Target0
{
    int2 b = int2(pos.xy) * off.zw + off.xy;
    float4 s0 = S.Load(int3(b, 0));
    if (mode.y == 1) return float4(saturate(s0.xy * 32.0), 0, 0);   // 16_16 (-32..32 en SNORM/32) -> k_16_16 sin signo
    // Sesion 24: sin recortar (textura R16G16_FLOAT): el SSAO guarda en .y la distancia/64, que pasa de 1 en lo lejano; al
    // recortarla a 1 el suavizado bilateral mezclaba fondo y primer plano (bloques y halos en escalera)
    if (mode.y == 2) return float4(s0.xy * 32.0, 0, 0);
    int sel = mode.x;
    if (sel < 4 || sel > 6) return s0;
    int2 dx = int2(off.z > 1 ? 1 : 0, 0), dy = int2(0, off.w > 1 ? 1 : 0);
    float4 s1 = S.Load(int3(b + dx, 0)), s2 = S.Load(int3(b + dy, 0)), s3 = S.Load(int3(b + dx + dy, 0));
    if (sel == 4) { if (off.w > 1 && off.z == 1) s1 = s2; return (s0 + s1) * 0.5; }
    if (sel == 5) return (s2 + s3) * 0.5;
    return (s0 + s1 + s2 + s3) * 0.25;
}
)";
// Sesion 24: aliasing de EDRAM color -> profundidad. En la 360 color y profundidad comparten la EDRAM: el juego restaura la
// profundidad del frame anterior escribiendo COLOR (8888 con la palabra D24S8: G = bits 0-7, B = 8-15, A = 16-23 de la
// profundidad, R = stencil) en la base del buffer de profundidad (PS C316F389, RT0 en la base de RB_DEPTH_INFO). Aqui color
// y profundidad son recursos distintos: tras ese draw se convierte el color escrito en profundidad con este pase.
const char* kColorToDepthHlsl = R"(
Texture2D<float4> S : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float ps_main(float4 pos : SV_Position) : SV_Depth
{
    int2 p = int2(pos.xy);
#ifndef NO_SWAP40
    // En la EDRAM, las baldosas de profundidad tienen las dos columnas de 40 muestras intercambiadas respecto a las de color
    // (como Xenia al copiar entre color y profundidad). Sin esto cada franja de 40 px recibia la profundidad de su vecina.
    uint w, h; S.GetDimensions(w, h);
    int tx = p.x % 80; int sx = p.x + (tx < 40 ? 40 : -40);
    if (sx >= 0 && sx < (int)w) p.x = sx;
#endif
    float4 c = S.Load(int3(p, 0));
    uint v = ((uint)round(c.a * 255.0) << 16) | ((uint)round(c.b * 255.0) << 8) | (uint)round(c.g * 255.0);
    return v / 16777215.0;
}
)";
ComPtr<ID3D12RootSignature> g_colorToDepthRootSig;
ComPtr<ID3D12PipelineState> g_colorToDepthPso;
ComPtr<ID3D12RootSignature> g_colorResolveRootSig;
ComPtr<ID3D12PipelineState> g_colorResolvePso, g_colorResolvePso1616, g_colorResolvePso1616F;
static const bool g_1616Sat = getenv("GH6_1616SAT") != nullptr;   // diagnostico: 16_16 -> k_16_16 recortado a 0..1 (antes)
ComPtr<ID3D12DescriptorHeap> g_rtvHeap3;   // RTV de las texturas resueltas por shader
constexpr UINT kRtvHeap3Size = 512;
UINT g_rtvHeap3Next = 0;
const char* kBlitHlsl = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID)
{
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(V i) : SV_Target { return float4(t0.Sample(s0, i.uv).rgb, 1); }
// Sesion 24: escalado nitido a la ventana (Catmull-Rom con 9 lecturas bilineales). A 1:1 reproduce la imagen exacta.
float4 ps_cubic(V i) : SV_Target
{
    float2 ts; t0.GetDimensions(ts.x, ts.y);
    float2 sp = i.uv * ts, tp1 = floor(sp - 0.5) + 0.5, f = sp - tp1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, tp12 = (tp1 + w2 / w12) / ts, tp0 = (tp1 - 1.0) / ts, tp3 = (tp1 + 2.0) / ts;
    float3 c = 0;
    c += t0.SampleLevel(s0, float2(tp0.x, tp0.y), 0).rgb * w0.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp0.y), 0).rgb * w12.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp0.y), 0).rgb * w3.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp0.x, tp12.y), 0).rgb * w0.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp12.y), 0).rgb * w12.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp12.y), 0).rgb * w3.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp0.x, tp3.y), 0).rgb * w0.x * w3.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp3.y), 0).rgb * w12.x * w3.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp3.y), 0).rgb * w3.x * w3.y;
    return float4(saturate(c), 1);
}
// Sesion 24: suavizado de bordes FXAA (algoritmo 3.11, calidad alta) sobre la imagen 1280x720 antes de escalarla.
// Busca el borde a lo largo de la direccion dominante y mezcla con el texel vecino segun la posicion en el escalon.
static const float kFxaaSubpix = 0.5;        // suavizado de detalles de 1 pixel (0.75 por defecto en FXAA: emborrona textos)
static const float kFxaaEdge = 0.125;        // contraste minimo relativo para tratar un borde
static const float kFxaaEdgeMin = 0.0625;    // contraste minimo absoluto (zonas oscuras)
float FxL(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float FxS(float2 p) { return FxL(t0.SampleLevel(s0, p, 0).rgb); }
float4 ps_fxaa(V i) : SV_Target
{
    float2 ts; t0.GetDimensions(ts.x, ts.y); float2 rcp = 1.0 / ts;
    float2 pos = (floor(i.uv * ts) + 0.5) * rcp;
    float3 rgbM = t0.SampleLevel(s0, pos, 0).rgb; float lumaM = FxL(rgbM);
    float lumaS = FxL(t0.SampleLevel(s0, pos, 0, int2(0, 1)).rgb), lumaN = FxL(t0.SampleLevel(s0, pos, 0, int2(0, -1)).rgb);
    float lumaE = FxL(t0.SampleLevel(s0, pos, 0, int2(1, 0)).rgb), lumaW = FxL(t0.SampleLevel(s0, pos, 0, int2(-1, 0)).rgb);
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(kFxaaEdgeMin, rangeMax * kFxaaEdge)) return float4(rgbM, 1);
    float lumaNW = FxL(t0.SampleLevel(s0, pos, 0, int2(-1, -1)).rgb), lumaSE = FxL(t0.SampleLevel(s0, pos, 0, int2(1, 1)).rgb);
    float lumaNE = FxL(t0.SampleLevel(s0, pos, 0, int2(1, -1)).rgb), lumaSW = FxL(t0.SampleLevel(s0, pos, 0, int2(-1, 1)).rgb);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float edgeHorz = abs(-2.0 * lumaW + lumaNW + lumaSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNE + lumaSE);
    float edgeVert = abs(-2.0 * lumaS + lumaSW + lumaSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNW + lumaNE);
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = (lumaNS + lumaWE) * 2.0 + (lumaNW + lumaSW + lumaNE + lumaSE);
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixA * (1.0 / 12.0) - lumaM) / range);
    float2 posB = pos, offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;
    float lumaNN = (pairN ? lumaN : lumaS) + lumaM;
    float gradientScaled = gradient * 0.25;
    bool lumaMLTZero = lumaM - lumaNN * 0.5 < 0.0;
    float2 posN = posB - offNP, posP = posB + offNP;
    float lumaEndN = FxS(posN) - lumaNN * 0.5, lumaEndP = FxS(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    static const float kSteps[8] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0 };
    [unroll] for (int k = 0; k < 8; k++)
    {
        if (doneN && doneP) break;
        if (!doneN) { posN -= offNP * kSteps[k]; lumaEndN = FxS(posN) - lumaNN * 0.5; doneN = abs(lumaEndN) >= gradientScaled; }
        if (!doneP) { posP += offNP * kSteps[k]; lumaEndP = FxS(posP) - lumaNN * 0.5; doneP = abs(lumaEndP) >= gradientScaled; }
    }
    float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y;
    float dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    bool directionN = dstN < dstP;
    bool goodSpan = directionN ? ((lumaEndN < 0.0) != lumaMLTZero) : ((lumaEndP < 0.0) != lumaMLTZero);
    float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    float off = max(pixelOffset, subpixF * subpixF * kFxaaSubpix);
    if (!horzSpan) pos.x += off * lengthSign; else pos.y += off * lengthSign;
    return float4(t0.SampleLevel(s0, pos, 0).rgb, 1);
}
// Sesion 24: nitidez adaptativa al contraste (algoritmo CAS de AMD FidelityFX, licencia MIT) a la misma resolucion.
// Realza el detalle de las texturas sin crear halos: el refuerzo baja donde ya hay mucho contraste. CAS_SHARP: 0..1.
#ifndef CAS_SHARP
#define CAS_SHARP 0.4
#endif
float3 CasL(int2 p, int2 mx) { return t0.Load(int3(clamp(p, int2(0, 0), mx), 0)).rgb; }
float4 ps_cas(V i) : SV_Target
{
    uint w, h; t0.GetDimensions(w, h); int2 mx = int2(w - 1, h - 1), p = int2(i.pos.xy);
    float3 a = CasL(p + int2(-1, -1), mx), b = CasL(p + int2(0, -1), mx), c = CasL(p + int2(1, -1), mx);
    float3 d = CasL(p + int2(-1, 0), mx), e = CasL(p, mx), f = CasL(p + int2(1, 0), mx);
    float3 g = CasL(p + int2(-1, 1), mx), hh = CasL(p + int2(0, 1), mx), ii = CasL(p + int2(1, 1), mx);
    float3 mn = min(min(min(d, e), min(f, b)), hh); mn += min(mn, min(min(a, c), min(g, ii)));
    float3 mxc = max(max(max(d, e), max(f, b)), hh); mxc += max(mxc, max(max(a, c), max(g, ii)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mxc) / max(mxc, 1e-5)));
    float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, CAS_SHARP));
    float3 col = (b * wgt + d * wgt + f * wgt + hh * wgt + e) / (1.0 + 4.0 * wgt);
    return float4(saturate(col), 1);
}
)";
// Suavizado de bordes en la presentacion (sesion 24): GH6_SUAVIZADO=fxaa|no (lo pone el launcher; por defecto fxaa)
int AaMode() { static int m = [] { const char* v = getenv("GH6_SUAVIZADO"); return (v && (!_stricmp(v, "no") || !_stricmp(v, "0"))) ? 0 : 1; }(); return m; }
// Nitidez CAS (GH6_NITIDEZ=0..1, 0 = desactivada; por defecto 0.4) y filtrado anisotropico de texturas (GH6_ANISO=1/2/4/8/16)
float CasSharp() { static float v = [] { const char* e = getenv("GH6_NITIDEZ"); float f = e ? (float)atof(e) : 0.4f; return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }(); return v; }
UINT AnisoLevel() { static UINT v = [] { const char* e = getenv("GH6_ANISO"); int a = e ? atoi(e) : 16; return (UINT)(a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1); }(); return v; }
ComPtr<ID3D12PipelineState> g_fxaaPso, g_casPso;
// Texturas intermedias del posproceso (0 = tras FXAA, 1 = tras CAS), mismo tamano que la imagen resuelta; SRV fijos 20 y 21
ComPtr<ID3D12Resource> g_ppTex[2];
ComPtr<ID3D12DescriptorHeap> g_ppRtvHeap;
D3D12_RESOURCE_STATES g_ppState[2] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
UINT g_ppW = 0, g_ppH = 0;
constexpr UINT kPpSrvSlot = 20;
// Tamano actual de la cadena de intercambio (sigue al de la ventana) y ventana de presentacion (sesion 24)
UINT g_bbW = kRtW, g_bbH = kRtH;
HWND g_presentHwnd = nullptr;
UINT g_swapFlags = 0;
HANDLE g_frameWait = nullptr;   // objeto de espera de latencia de la cadena de intercambio

// Rect list: 3 esquinas -> quad. La diagonal es la arista mas larga; el 4o vertice = a + b - esquina.
const char* kRectGsHlsl = R"(
[maxvertexcount(4)]
void main(triangle XVSOut v[3], inout TriangleStream<XVSOut> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d02 = dot(p0 - p2, p0 - p2);
    uint c = 0, a = 1, b = 2;
    if (d02 > d12 && d02 >= d01) { c = 1; a = 0; b = 2; }
    else if (d01 > d12 && d01 > d02) { c = 2; a = 0; b = 1; }
    XVSOut d;
    d.pos = v[a].pos + v[b].pos - v[c].pos;
    [unroll] for (int i = 0; i < 16; i++) d.t[i] = v[a].t[i] + v[b].t[i] - v[c].t[i];
    s.Append(v[c]); s.Append(v[a]); s.Append(v[b]); s.Append(d);
}
)";

bool Check(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr)) return true;
    LOG("[d3d12] fallo en %s (0x%08lX); se vuelve a GDI", what, (unsigned long)hr);
    g_failed = true;
    return false;
}

// Sesion 24: medicion de tirones (GH6_HITCHMS=<ms>, por defecto 40) y cache de shaders compilados en disco
double QpcMs()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}
double g_hCompileMs = 0, g_hPsoMs = 0, g_hTexMs = 0, g_hWaitMs = 0;
uint32_t g_hCompiles = 0, g_hCacheHits = 0, g_hPsos = 0, g_hFlushes = 0;
double g_hFlushMs = 0, g_hDrawMs = 0, g_hPresentMs = 0;
thread_local bool t_prewarm = false;   // hilos de precompilacion: no cuentan en la medicion de tirones
std::mutex g_xlatMutex;                // el traductor usa estado global (g_curIsPs, g_xlatNoGrad)
uint64_t Fnv64(const void* d, size_t n, uint64_t h = 1469598103934665603ull);
static uint64_t TexKey(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, uint32_t extra, uint32_t kind)
{ uint32_t v[6] = { base, fmt, w, h, extra, kind }; return Fnv64(v, sizeof(v)); }
uint64_t Fnv64(const void* d, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)d;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// Sesion 24: vertex shaders con IEEE estricto (sin fusionar ni reordenar operaciones). El juego vuelve a dibujar la misma geometria
// con otro VS (p. ej. la luz de los focos 031FA1CF sobre el suelo, con prueba de profundidad LEQUAL); con las mismas operaciones de
// Xenos, la posicion debe salir identica al bit. Optimizado, el driver fusionaba de forma distinta y la prueba fallaba a franjas.
// No arreglo nada visible en las pruebas: queda como opcion (GH6_STRICTVS=1) para no recompilar todos los VS.
static bool StrictVs(const char* target) { static const bool on = getenv("GH6_STRICTVS") != nullptr; return on && target[0] == 'v' && target[1] == 's'; }
ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target, const char* name)
{
    struct CompileTimer { double t0 = QpcMs(); ~CompileTimer() { if (!t_prewarm) g_hCompileMs += QpcMs() - t0; } } ctimer;
    ComPtr<ID3DBlob> code, err;
    if (getenv("GH6_HLSLDUMP"))   // GH6_HLSLDUMP=1: guarda cada shader traducido en hlsl_dump/ (sesion 24)
    {
        CreateDirectoryA("hlsl_dump", nullptr);
        char path[128]; snprintf(path, sizeof(path), "hlsl_dump/%s.hlsl", name);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    // Cache en disco (shadercache\<hash del HLSL + destino>.cso): las siguientes partidas no recompilan. GH6_NOSHADERCACHE=1 la desactiva.
    static const bool noCache = getenv("GH6_NOSHADERCACHE") != nullptr;
    char cpath[96] = {};
    if (!noCache)
    {
        uint64_t key = Fnv64(src.data(), src.size());
        key = Fnv64(target, strlen(target), key); key = Fnv64(entry, strlen(entry), key);
        key = StrictVs(target) ? Fnv64("O1-v1-ieee", 10, key) : Fnv64("O1-v1", 5, key);
        snprintf(cpath, sizeof(cpath), "shadercache/%016llX.cso", (unsigned long long)key);
        HANDLE h = CreateFileA(cpath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
            DWORD got = 0;
            if (sz.QuadPart > 0 && sz.QuadPart < (64 << 20) && SUCCEEDED(D3DCreateBlob((SIZE_T)sz.QuadPart, &code)) &&
                ReadFile(h, code->GetBufferPointer(), (DWORD)sz.QuadPart, &got, nullptr) && got == sz.QuadPart)
            { CloseHandle(h); if (!t_prewarm) g_hCacheHits++; return code; }
            CloseHandle(h); code.Reset();
        }
    }
    if (!t_prewarm) g_hCompiles++;
    if (FAILED(D3DCompile(src.data(), src.size(), name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1 | (StrictVs(target) ? D3DCOMPILE_IEEE_STRICTNESS : 0), 0, &code, &err)))
    {
        LOG("[d3d12] error compilando %s: %.400s", name, err ? (const char*)err->GetBufferPointer() : "?");
        CreateDirectoryA("hlsl_err", nullptr);
        char path[128]; snprintf(path, sizeof(path), "hlsl_err/%s.hlsl", name);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        return nullptr;
    }
    if (cpath[0])
    {
        CreateDirectoryA("shadercache", nullptr);
        char tmp[112]; snprintf(tmp, sizeof(tmp), "%s.%lu", cpath, GetCurrentThreadId());
        HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD put = 0; BOOL ok = WriteFile(h, code->GetBufferPointer(), (DWORD)code->GetBufferSize(), &put, nullptr);
            CloseHandle(h);
            if (!ok || !MoveFileExA(tmp, cpath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return code;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    if (a == b || !r)
    {
        // Transicion invalida (mismo estado antes y despues): invalidaria la lista. Se sustituye por una barrera UAV global (inofensiva).
        static int n = 0;
        if (n++ < 10) LOG("[d3d12] transicion invalida evitada: estado 0x%X -> 0x%X (recurso %p)", (unsigned)a, (unsigned)b, (void*)r);
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; br.UAV.pResource = nullptr;
        return br;
    }
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return br;
}

// Sesion 24 (tirones): biblioteca de PSO en disco (shadercache\pso.lib, propia de cada driver). Crear un PSO cuesta 2-4 ms y al
// empezar cada cancion se creaban decenas; con la biblioteca, desde la segunda partida se cargan casi sin coste.
// El nombre incluye el hash de los bytecodes (si cambia el traductor, el PSO viejo no se reutiliza). GH6_NOPSOCACHE=1 la desactiva.
static ComPtr<ID3D12PipelineLibrary> g_psoLib;
static std::vector<uint8_t> g_psoLibData;   // la biblioteca referencia esta memoria mientras exista
static bool g_psoLibDirty = false; static double g_psoLibLastAdd = 0;
static uint64_t g_psoLibLoaded = 0, g_psoLibStored = 0;
static void InitPsoLibrary()
{
    if (getenv("GH6_NOPSOCACHE") || getenv("GH6_NOSHADERCACHE")) return;
    ComPtr<ID3D12Device1> d1; if (FAILED(g_dev.As(&d1))) return;
    if (FILE* f = fopen("shadercache/pso.lib", "rb"))
    {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (512l << 20)) { g_psoLibData.resize(n); if (fread(g_psoLibData.data(), 1, n, f) != (size_t)n) g_psoLibData.clear(); }
        fclose(f);
    }
    HRESULT hr = E_FAIL;
    if (!g_psoLibData.empty()) hr = d1->CreatePipelineLibrary(g_psoLibData.data(), g_psoLibData.size(), IID_PPV_ARGS(&g_psoLib));
    if (FAILED(hr)) { g_psoLibData.clear(); g_psoLib.Reset(); hr = d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_psoLib)); }
    LOG("[d3d12] biblioteca de PSO: %s (%zu KB)", SUCCEEDED(hr) ? "lista" : "no disponible", g_psoLibData.size() >> 10);
    if (FAILED(hr)) g_psoLib.Reset();
}
static void SavePsoLibrary(bool force)
{
    if (!g_psoLib || !g_psoLibDirty) return;
    if (!force && QpcMs() - g_psoLibLastAdd < 10000) return;   // solo tras 10 s sin PSO nuevos (fuera de las cargas)
    SIZE_T n = g_psoLib->GetSerializedSize();
    std::vector<uint8_t> buf(n);
    if (n && SUCCEEDED(g_psoLib->Serialize(buf.data(), n)))
    {
        CreateDirectoryA("shadercache", nullptr);
        if (FILE* f = fopen("shadercache/pso.lib.tmp", "wb")) { fwrite(buf.data(), 1, n, f); fclose(f); MoveFileExA("shadercache/pso.lib.tmp", "shadercache/pso.lib", MOVEFILE_REPLACE_EXISTING); }
        LOG("[d3d12] biblioteca de PSO guardada: %zu KB (cargados %llu, nuevos %llu)", (size_t)(n >> 10), (unsigned long long)g_psoLibLoaded, (unsigned long long)g_psoLibStored);
    }
    g_psoLibDirty = false;
}
static HRESULT CreatePsoCached(D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, uint64_t key, ComPtr<ID3D12PipelineState>& pso)
{
    if (!g_psoLib) return g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    uint64_t h = Fnv64(&key, sizeof(key));
    { UINT salt = AnisoLevel(); h = Fnv64(&salt, sizeof(salt), h); }   // la firma raiz cambia con el anisotropico
    h = Fnv64(pd.VS.pShaderBytecode, pd.VS.BytecodeLength, h); h = Fnv64(pd.PS.pShaderBytecode, pd.PS.BytecodeLength, h);
    if (pd.GS.pShaderBytecode) h = Fnv64(pd.GS.pShaderBytecode, pd.GS.BytecodeLength, h);
    wchar_t name[24]; swprintf(name, 24, L"%016llX", (unsigned long long)h);
    if (SUCCEEDED(g_psoLib->LoadGraphicsPipeline(name, &pd, IID_PPV_ARGS(&pso)))) { g_psoLibLoaded++; return S_OK; }
    HRESULT hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (SUCCEEDED(hr) && SUCCEEDED(g_psoLib->StorePipeline(name, pso.Get()))) { g_psoLibStored++; g_psoLibDirty = true; g_psoLibLastAdd = QpcMs(); }
    return hr;
}
// Sesion 24: toda CopyTextureRegion pasa por aqui. Un parametro invalido hace fallar Close (E_INVALIDARG) y se pierde la lista
// entera (pantalla negra; luego DEVICE_HUNG). Se comprueban tamanos, alineaciones y formato; si algo no cuadra se registra y se omite.
static bool IsBlockFmt(DXGI_FORMAT f) { return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB); }
static void CheckedCopy(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dx, UINT dy, UINT dz, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
    static const bool noCheck = getenv("GH6_NOCOPYCHECK") != nullptr;
    const char* why = nullptr; char info[256] = {};
    if (!noCheck)
    {
        auto subDims = [](ID3D12Resource* r, UINT sub, UINT& w, UINT& h, DXGI_FORMAT& f) -> bool {
            D3D12_RESOURCE_DESC d = r->GetDesc(); f = d.Format;
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return false;
            UINT mips = d.MipLevels ? d.MipLevels : 1, arr = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
            UINT planes = (d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) ? 2 : 1;
            if (sub >= mips * arr * planes) { w = h = 0; return true; }
            UINT mip = sub % mips; w = (UINT)std::max<UINT64>(d.Width >> mip, 1); h = std::max<UINT>(d.Height >> mip, 1);
            if (IsBlockFmt(d.Format)) { w = (w + 3) & ~3u; h = (h + 3) & ~3u; }
            return true; };
        if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        {
            UINT tw, th; DXGI_FORMAT tf;
            if (subDims(dst->pResource, dst->SubresourceIndex, tw, th, tf))
            {
                UINT cw, ch;
                if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
                {
                    const auto& fp = src->PlacedFootprint.Footprint;
                    cw = box ? box->right - box->left : fp.Width; ch = box ? box->bottom - box->top : fp.Height;
                    if (src->PlacedFootprint.Offset % 512) why = "offset sin alinear a 512";
                    else if (fp.RowPitch % 256) why = "rowpitch sin alinear a 256";
                    else if (IsBlockFmt(fp.Format) && ((fp.Width & 3) || (fp.Height & 3))) why = "huella BC no multiplo de 4";
                    else if (fp.Format != tf && !(IsBlockFmt(tf) && IsBlockFmt(fp.Format))) why = "formato distinto";
                    snprintf(info, sizeof(info), "huella %ux%u fmt %d rp %u off %llu -> sub %u (%ux%u fmt %d) en %u,%u", fp.Width, fp.Height, (int)fp.Format, fp.RowPitch,
                             (unsigned long long)src->PlacedFootprint.Offset, dst->SubresourceIndex, tw, th, (int)tf, dx, dy);
                }
                else
                {
                    UINT sw, sh; DXGI_FORMAT sf; subDims(src->pResource, src->SubresourceIndex, sw, sh, sf);
                    cw = box ? box->right - box->left : sw; ch = box ? box->bottom - box->top : sh;
                    if (box && (box->right > sw || box->bottom > sh || box->right <= box->left || box->bottom <= box->top)) why = "caja fuera del origen";
                    snprintf(info, sizeof(info), "sub %u (%ux%u fmt %d) -> sub %u (%ux%u fmt %d) en %u,%u copia %ux%u", src->SubresourceIndex, sw, sh, (int)sf,
                             dst->SubresourceIndex, tw, th, (int)tf, dx, dy, cw, ch);
                }
                if (!why && (tw == 0 || dx + cw > tw || dy + ch > th)) why = "no cabe en el destino";
            }
        }
        else if (src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        {   // a un buffer (lectura): la caja debe caber en el origen y la huella alineada
            UINT sw, sh; DXGI_FORMAT sf;
            if (subDims(src->pResource, src->SubresourceIndex, sw, sh, sf))
            {
                const auto& fp = dst->PlacedFootprint.Footprint;
                if (dst->PlacedFootprint.Offset % 512) why = "offset sin alinear a 512";
                else if (fp.RowPitch % 256) why = "rowpitch sin alinear a 256";
                else if (box && (box->right > sw || box->bottom > sh || box->right <= box->left || box->bottom <= box->top)) why = "caja fuera del origen";
                else if (!box && (fp.Width < sw || fp.Height < sh)) why = "huella menor que el origen";
                snprintf(info, sizeof(info), "sub %u (%ux%u fmt %d) -> huella %ux%u fmt %d rp %u", src->SubresourceIndex, sw, sh, (int)sf, fp.Width, fp.Height, (int)fp.Format, fp.RowPitch);
            }
        }
    }
    if (why)
    {
        static int n = 0;
        if (n++ < 40) LOG("[d3d12] copia de textura invalida omitida (%s): %s", why, info);
        return;
    }
    g_list->CopyTextureRegion(dst, dx, dy, dz, src, box);
}
// Copia de region comprobada (sesion 23): un rectangulo fuera de los recursos invalida toda la lista de comandos
bool SafeCopyRegion(ID3D12Resource* dst, ID3D12Resource* src, const D3D12_BOX& box, const char* who)
{
    D3D12_RESOURCE_DESC sd = src->GetDesc(), dd = dst->GetDesc();
    UINT w = box.right - box.left, h = box.bottom - box.top;
    bool ok = box.right > box.left && box.bottom > box.top && box.right <= sd.Width && box.bottom <= sd.Height &&
              w <= dd.Width && h <= dd.Height && sd.Format == dd.Format;
    if (!ok)
    {
        static int n = 0;
        if (n++ < 10) LOG("[d3d12] copia invalida evitada (%s): caja (%u,%u)-(%u,%u) origen %llux%u fmt %d -> destino %llux%u fmt %d",
            who, box.left, box.top, box.right, box.bottom, (unsigned long long)sd.Width, sd.Height, (int)sd.Format, (unsigned long long)dd.Width, dd.Height, (int)dd.Format);
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{ dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dl.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sl{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    CheckedCopy(&dl, 0, 0, 0, &sl, &box);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) { auto h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_rtvSize; return h; }
D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT i) { auto h = g_srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_srvSize; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT i) { auto h = g_srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += i * g_srvSize; return h; }

ComPtr<ID3D12RootSignature> MakeRootSig(const D3D12_ROOT_SIGNATURE_DESC& d)
{
    ComPtr<ID3DBlob> blob, err; ComPtr<ID3D12RootSignature> rs;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { LOG("[d3d12] firma raiz: %s", err ? (const char*)err->GetBufferPointer() : "?"); return nullptr; }
    g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    return rs;
}

// Sesion 24: precompilacion en segundo plano. Al arrancar se traducen y compilan (en varios hilos de baja prioridad) todos los
// shaders ya vistos en partidas anteriores (shaders\*.bin, microcodigo en big endian) para llenar shadercache\. Asi, al entrar en
// una cancion, los shaders salen de la cache en vez de compilarse en el hilo de la GPU (eran ~100 ms cada uno: 12 s de pantalla negra).
// GH6_NOPREWARM=1 la desactiva.
// Sustituciones de shaders por hash (sesion 24):
// - Velocidades a cero (desenfoque de movimiento desactivado): los draws de velocidad de personajes/publico (PS DF75D55E,
//   99CD25F5) y de camara (DF757D71) daban desplazamientos de 16+ pixeles con los personajes casi quietos y el compositor
//   C591749F emborronaba toda la imagen. Con velocidad 0 la imagen coincide con la de la 360 (personajes nitidos, fondo
//   desenfocado por profundidad, halo). GH6_MOTIONBLUR=1 lo deja como antes. Pendiente: la causa real de esas velocidades.
// - GH6_PSOUT=<hash>:<expresion>[;...] (diagnostico): sustituye la salida oC0 de esos pixel shaders.
bool ApplyShaderOverrides(uint32_t type, uint64_t hash, std::string& hlsl)
{
    if (type != 1) return false;
    bool changed = false;
    size_t r = hlsl.rfind("    return XVte.w");
    if (r == std::string::npos) return false;
    // Sesion 24: grano de pelicula del ultimo pase (0261F529: lerp(color, ruido, 0.08); el ruido mezcla una textura 64x64 con
    // frac(668.3*u + 270700*v)). Aqui sale como rayas verticales finas sobre los personajes. GH6_NOGRAIN=1 (grano_pelicula=0 en
    // gh6.ini, por defecto) lo quita: el pase devuelve el color sin el ruido.
    {
        static const bool noGrain = getenv("GH6_NOGRAIN") && atoi(getenv("GH6_NOGRAIN")) != 0;
        if (noGrain && hash == 0x0261F5295D681495ull)
        { hlsl.insert(r, "    oC0 = float4(r[0].xyz, oC0.w);\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
    }
    // Sesion 24: con la profundidad restaurada (aliasing color->profundidad) las velocidades ya son correctas: desenfoque de
    // movimiento activo por defecto. GH6_NOMOTIONBLUR=1 (o desenfoque_movimiento=0 en gh6.ini) lo quita.
    static const bool motionBlur = getenv("GH6_NOMOTIONBLUR") == nullptr;
    if (!motionBlur && (hash == 0xDF757D719DE4CB8Full || hash == 0xDF75D55E88917A96ull || hash == 0x99CD25F562821A21ull))
    { hlsl.insert(r, "    oC0 = float4(0, 0, 0, 0);\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
    if (const char* env = getenv("GH6_PSOUT"))
    {
        std::string all = env;
        for (size_t pos = 0; pos <= all.size(); )
        {
            size_t end = all.find(';', pos); if (end == std::string::npos) end = all.size();
            std::string spec = all.substr(pos, end - pos); pos = end + 1;
            size_t c = spec.find(':');
            if (c != std::string::npos && strtoull(spec.substr(0, c).c_str(), nullptr, 16) == hash)
            { hlsl.insert(r, std::string(getenv("GH6_PSOUTAT") ? "    if (XVte.z < 0) " : "    ") + "oC0 = " + spec.substr(c + 1) + ";\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
        }
    }
    return changed;
}

void PrewarmShaders()
{
    if (getenv("GH6_NOPREWARM") || getenv("GH6_NOSHADERCACHE")) return;
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA("shaders/*.bin", &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do { if ((fd.cFileName[0] == 'v' || fd.cFileName[0] == 'p') && fd.cFileName[1] == 's') files.push_back(fd.cFileName); } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    if (files.empty()) return;
    std::thread([files]() {
        double t0 = QpcMs();
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw > 3 ? std::min(hw - 2, 8u) : 1u;
        std::atomic<size_t> next{ 0 }; std::atomic<uint32_t> ok{ 0 };
        auto worker = [&]() {
            t_prewarm = true;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                size_t i = next++;
                if (i >= files.size()) break;
                const std::string& fn = files[i];
                std::string path = "shaders/" + fn;
                FILE* f = fopen(path.c_str(), "rb");
                if (!f) continue;
                std::vector<uint32_t> w;
                uint32_t be;
                while (fread(&be, 4, 1, f) == 1) w.push_back(__builtin_bswap32(be));
                fclose(f);
                if (w.empty()) continue;
                uint32_t type = fn[0] == 'p' ? 1 : 0;
                std::string name = fn.substr(0, fn.size() - 4);
                TranslatedShader t;
                { std::lock_guard<std::mutex> lk(g_xlatMutex); t = TranslateXenosShader(type, w.data(), (uint32_t)w.size()); }
                if (!t.ok) continue;
                uint64_t hash = strtoull(name.c_str() + 3, nullptr, 16);
                ApplyShaderOverrides(type, hash, t.hlsl);
                auto blob = Compile(t.hlsl, "main", type ? "ps_5_0" : "vs_5_0", name.c_str());
                if (!blob && type == 1 && t.hlsl.find("SampleGrad") != std::string::npos)
                {
                    { std::lock_guard<std::mutex> lk(g_xlatMutex); g_xlatNoGrad = true; t = TranslateXenosShader(type, w.data(), (uint32_t)w.size()); g_xlatNoGrad = false; }
                    if (t.ok) { ApplyShaderOverrides(type, hash, t.hlsl); blob = Compile(t.hlsl, "main", "ps_5_0", name.c_str()); }
                }
                if (blob) ok++;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < n; k++) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
        LOG("[d3d12] precompilacion: %u/%zu shaders listos en %.1f s (%u hilos)", ok.load(), files.size(), (QpcMs() - t0) / 1000.0, n);
    }).detach();
}

bool Init(HWND hwnd)
{
    if (getenv("GH6_D3D12_DEBUG")) { ComPtr<ID3D12Debug> dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
    if (!Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)), "D3D12CreateDevice")) return false;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Check(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue")) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = kRtW; sd.Height = kRtH; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kFrames; sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Sesion 24 (retraso de video): latencia maxima de frames configurable (GH6_FRAMELATENCY=1/2; por defecto 0 = sin limite, como antes)
    static const int frameLatency = getenv("GH6_FRAMELATENCY") ? atoi(getenv("GH6_FRAMELATENCY")) : 0;   // 1-2 bajan los fps (medido: 48 y 55 fps)
    if (frameLatency > 0) sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    g_swapFlags = sd.Flags;
    ComPtr<IDXGISwapChain1> sc1;
    if (!Check(factory->CreateSwapChainForHwnd(g_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd")) return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&g_swap);
    g_presentHwnd = hwnd;
    if (frameLatency > 0 && g_swap)
    {
        g_swap->SetMaximumFrameLatency((UINT)frameLatency);
        g_frameWait = g_swap->GetFrameLatencyWaitableObject();
        LOG("[d3d12] latencia maxima de presentacion: %d frame(s)", frameLatency);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kFrames + 1 + 64;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kFrames; i++)
    {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]));
        g_dev->CreateRenderTargetView(g_backbuffers[i].Get(), nullptr, Rtv(i));
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
    }
    D3D12_DESCRIPTOR_HEAP_DESC dh{}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 64;
    g_dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&g_dsvHeap));
    g_dsvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 32 + kFrames * kDescPerFrame; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap));
    g_srvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
    g_list->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_STATIC_SAMPLER_DESC samp{}; samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; samp.MaxLOD = D3D12_FLOAT32_MAX;
    // Blit: tabla con 1 SRV
    D3D12_DESCRIPTOR_RANGE r0{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER bp{}; bp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; bp.DescriptorTable = { 1, &r0 }; bp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    g_blitRootSig = MakeRootSig({ 1, &bp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE });
    // Draws: CBV VS (b0), CBV PS (b0), SRV crudo de vertices (t0, VS), tabla t1..t16 (PS)
    D3D12_DESCRIPTOR_RANGE r1{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 48, 1, 0, 0 };   // t1-t16: 2D, t17-t32: 3D, t33-t48: cubo (sesion 21)
    D3D12_ROOT_PARAMETER dp[4]{};
    dp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; dp[0].Descriptor = { 0, 0 }; dp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    dp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; dp[1].Descriptor = { 0, 0 }; dp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    dp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; dp[2].Descriptor = { 0, 0 }; dp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    dp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; dp[3].DescriptorTable = { 1, &r1 }; dp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Sesion 24: s0 = lineal y s1 = punto, ambos repitiendo; el modo de borde de cada textura (repetir, espejo, recortar,
    // color de borde) se emula en el shader con XTexMode (constante de fetch: clamp_x/y/z, filtros, color de borde)
    D3D12_STATIC_SAMPLER_DESC ds[2] = { samp, samp };
    ds[0].AddressU = ds[0].AddressV = ds[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ds[1] = ds[0]; ds[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; ds[1].ShaderRegister = 1;
    // Sesion 24: filtrado anisotropico (las texturas inclinadas -mastil, suelo- se ven nitidas a distancia). Con SampleGrad usa
    // los gradientes reales; en pasadas a pantalla completa (gradientes iguales) da lo mismo que el trilineal.
    if (AnisoLevel() > 1) { ds[0].Filter = D3D12_FILTER_ANISOTROPIC; ds[0].MaxAnisotropy = AnisoLevel(); }
    LOG("[d3d12] filtrado de texturas: %s (x%u)", AnisoLevel() > 1 ? "anisotropico" : "trilineal", AnisoLevel());
    g_drawRootSig = MakeRootSig({ 4, dp, 2, ds, D3D12_ROOT_SIGNATURE_FLAG_NONE });
    if (!g_blitRootSig || !g_drawRootSig) { g_failed = true; return false; }

    // GH6_UPSCALE=bilineal: escalado bilineal (antes); por defecto Catmull-Rom
    static const bool bilinear = getenv("GH6_UPSCALE") && !strcmp(getenv("GH6_UPSCALE"), "bilineal");
    auto vs = Compile(kBlitHlsl, "vs_main", "vs_5_0", "blit"), ps = Compile(kBlitHlsl, bilinear ? "ps_main" : "ps_cubic", "ps_5_0", "blit");
    g_rectGs = Compile(std::string(kXenosInterfaceHlsl) + kRectGsHlsl, "main", "gs_5_0", "rectlist_gs");
    if (!vs || !ps) { g_failed = true; return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_blitRootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
    if (!Check(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_blitPso)), "CreateGraphicsPipelineState")) return false;
    if (AaMode())
    {
        auto fps = Compile(kBlitHlsl, "ps_fxaa", "ps_5_0", "fxaa");
        if (fps) { pd.PS = { fps->GetBufferPointer(), fps->GetBufferSize() }; g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_fxaaPso)); }
        LOG("[d3d12] suavizado de bordes: %s", g_fxaaPso ? "FXAA" : "no disponible (fallo al compilar)");
    }
    if (CasSharp() > 0.0f)
    {
        char def[64]; snprintf(def, sizeof(def), "#define CAS_SHARP %.3f\n", CasSharp());
        auto cps = Compile(std::string(def) + kBlitHlsl, "ps_cas", "ps_5_0", "cas");
        if (cps) { pd.PS = { cps->GetBufferPointer(), cps->GetBufferSize() }; g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_casPso)); }
        LOG("[d3d12] nitidez CAS %.2f: %s", CasSharp(), g_casPso ? "activa" : "no disponible (fallo al compilar)");
    }
    // Copia de profundidad por GPU (sesion 21): lee el plano de profundidad y escribe R32_FLOAT + RGBA8 con los bits 24_8 empaquetados
    {
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 4 }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &rd }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_depthCopyRootSig = MakeRootSig({ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        auto dvs = Compile(kDepthCopyHlsl, "vs_main", "vs_5_0", "depthcopy"), dps = Compile(kDepthCopyHlsl, "ps_main", "ps_5_0", "depthcopy");
        if (g_depthCopyRootSig && dvs && dps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dpd{};
            dpd.pRootSignature = g_depthCopyRootSig.Get();
            dpd.VS = { dvs->GetBufferPointer(), dvs->GetBufferSize() }; dpd.PS = { dps->GetBufferPointer(), dps->GetBufferSize() };
            dpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            dpd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            dpd.BlendState.IndependentBlendEnable = TRUE;
            dpd.SampleMask = 0xFFFFFFFF;
            dpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; dpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            dpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            dpd.NumRenderTargets = 2; dpd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT; dpd.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM; dpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&dpd, IID_PPV_ARGS(&g_depthCopyPso));
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 64;
        g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap2));
    }
    {   // Resolve de color con MSAA por GPU (sesion 24)
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 8 }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &rd }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_colorResolveRootSig = MakeRootSig({ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        auto cvs = Compile(kColorResolveHlsl, "vs_main", "vs_5_0", "colorresolve"), cps = Compile(kColorResolveHlsl, "ps_main", "ps_5_0", "colorresolve");
        if (g_colorResolveRootSig && cvs && cps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = g_colorResolveRootSig.Get();
            cpd.VS = { cvs->GetBufferPointer(), cvs->GetBufferSize() }; cpd.PS = { cps->GetBufferPointer(), cps->GetBufferSize() };
            cpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            cpd.SampleMask = 0xFFFFFFFF;
            cpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; cpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            cpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            cpd.NumRenderTargets = 1; cpd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; cpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso));
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16_UNORM;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso1616));
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso1616F));
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kRtvHeap3Size;
        g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap3));
    }
    {   // Aliasing color -> profundidad (sesion 24)
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[1]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = { 1, &rd }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_colorToDepthRootSig = MakeRootSig({ 1, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        // GH6_NOSWAP40=1 (diagnostico): sin el intercambio de columnas de 40 muestras (antes)
        const std::string c2dSrc = std::string(getenv("GH6_NOSWAP40") ? "#define NO_SWAP40 1\n" : "") + kColorToDepthHlsl;
        auto zvs = Compile(c2dSrc, "vs_main", "vs_5_0", "color2depth"), zps = Compile(c2dSrc, "ps_main", "ps_5_0", "color2depth");
        if (g_colorToDepthRootSig && zvs && zps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC zpd{};
            zpd.pRootSignature = g_colorToDepthRootSig.Get();
            zpd.VS = { zvs->GetBufferPointer(), zvs->GetBufferSize() }; zpd.PS = { zps->GetBufferPointer(), zps->GetBufferSize() };
            zpd.SampleMask = 0xFFFFFFFF;
            zpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; zpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; zpd.RasterizerState.DepthClipEnable = FALSE;
            zpd.DepthStencilState.DepthEnable = TRUE; zpd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            zpd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; zpd.DepthStencilState.StencilEnable = FALSE;
            zpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            zpd.NumRenderTargets = 0; zpd.DSVFormat = kDepthFmt; zpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&zpd, IID_PPV_ARGS(&g_colorToDepthPso));
        }
    }

    // Render target "EDRAM" (color 0) + textura blanca provisional + buffers de subida
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kRtW; td.Height = kRtH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_B8G8R8A8_UNORM, { 0, 0, 0, 1 } };
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_edram));
    g_dev->CreateRenderTargetView(g_edram.Get(), nullptr, Rtv(kFrames));
    g_dev->CreateShaderResourceView(g_edram.Get(), nullptr, SrvCpu(17));
    td.Width = 1; td.Height = 1; td.Flags = D3D12_RESOURCE_FLAG_NONE;
    g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_white));
    // (Textura 1x1 en heap de subida no es valida para SRV en todos los drivers: usamos una por defecto rellenada por copia)
    g_white.Reset();
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_white));
    for (UINT i = 1; i <= 16; i++) g_dev->CreateShaderResourceView(g_white.Get(), nullptr, SrvCpu(i));
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kRingSize; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kFrames; i++)
    {
        g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_ring[i]));
        D3D12_RANGE none{ 0, 0 }; g_ring[i]->Map(0, &none, (void**)&g_ringMap[i]);
    }
    LOG("[d3d12] backend inicializado (swapchain %ux%u, EDRAM provisional, draws traducidos)", kRtW, kRtH);
    InitPsoLibrary();
    PrewarmShaders();
    return true;
}

// Sesion 24: la cadena de intercambio sigue el tamano de la ventana (antes 1280x720 fijo y Windows la estiraba con un
// filtro bilineal: todo borroso en ventanas grandes). Se llama tras Present, sin frame abierto.
void ResizeSwapIfNeeded()
{
    static const bool fixedSize = getenv("GH6_FIXEDSWAP") != nullptr;
    if (fixedSize || !g_presentHwnd || !g_swap || g_frameOpen) return;
    RECT rc; if (!GetClientRect(g_presentHwnd, &rc)) return;
    UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if (w < 64 || h < 64 || (w == g_bbW && h == g_bbH)) return;   // minimizada o sin cambios
    g_queue->Signal(g_fence.Get(), g_nextFence);
    g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 5000);
    for (UINT i = 0; i < kFrames; i++) g_backbuffers[i].Reset();
    HRESULT hr = g_swap->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN, g_swapFlags);
    for (UINT i = 0; i < kFrames; i++)
    {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]));
        if (g_backbuffers[i]) g_dev->CreateRenderTargetView(g_backbuffers[i].Get(), nullptr, Rtv(i));
    }
    if (SUCCEEDED(hr)) { g_bbW = w; g_bbH = h; }
    LOG("[d3d12] ventana %ux%u: cadena de intercambio redimensionada (0x%08lX)", w, h, (unsigned long)hr);
}
// Rectangulo 16:9 centrado en la cadena de intercambio
void PresentViewport(D3D12_VIEWPORT& vp, D3D12_RECT& sr)
{
    float sc = std::min(g_bbW / (float)kRtW, g_bbH / (float)kRtH);
    float w = kRtW * sc, h = kRtH * sc;
    vp = { std::floor((g_bbW - w) * 0.5f), std::floor((g_bbH - h) * 0.5f), w, h, 0, 1 };
    sr = { 0, 0, (LONG)g_bbW, (LONG)g_bbH };
}

UINT64 RingAlloc(UINT64 size, UINT64 align = 256)
{
    UINT64 o = (g_ringOffset + align - 1) & ~(align - 1);
    if (o + size > kRingSize)
    {
        // Sesion 24: el anillo por frame se llena (los draws siguientes se omiten como "mem"): se registra
        static int logged = 0; static UINT64 lastFrame = ~0ull;
        if (logged < 40 && lastFrame != g_frameCounter) { logged++; lastFrame = g_frameCounter; LOG("[d3d12] anillo lleno en el frame %llu: usado %llu MB, pedido %llu KB", (unsigned long long)g_frameCounter, (unsigned long long)(g_ringOffset >> 20), (unsigned long long)(size >> 10)); }
        return ~0ull;
    }
    g_ringOffset = o + size;
    return o;
}

void OpenFrame()
{
    if (g_frameOpen) return;
    if (g_frameWait) { double tw = QpcMs(); WaitForSingleObjectEx(g_frameWait, 100, TRUE); g_hWaitMs += QpcMs() - tw; }
    g_fi = g_swap->GetCurrentBackBufferIndex();
    if (g_fence->GetCompletedValue() < g_fenceValues[g_fi]) { double tw = QpcMs(); g_fence->SetEventOnCompletion(g_fenceValues[g_fi], g_fenceEvent); WaitForSingleObject(g_fenceEvent, 1000); g_hWaitMs += QpcMs() - tw; }
    g_alloc[g_fi]->Reset();
    g_list->Reset(g_alloc[g_fi].Get(), nullptr);
    g_ringOffset = 0;
    g_vbCache.clear();
    g_deferredRelease[g_fi].clear();   // la GPU ya termino el frame que uso este indice
    static bool whiteDone = false;
    if (!whiteDone)
    {
        whiteDone = true;
        UINT64 o = RingAlloc(256, 512);
        *(uint32_t*)(g_ringMap[g_fi] + o) = 0xFFFFFFFF;
        D3D12_TEXTURE_COPY_LOCATION dst{ g_white.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = o; src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 256 };
        CheckedCopy(&dst, 0, 0, 0, &src, nullptr);
        auto b = Transition(g_white.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b);
    }
    g_boundRt = nullptr;
    g_frameOpen = true;
    g_frameDraws = 0;
    g_descCursor = 0;
    g_frameCounter++;
}

// ---- traduccion de estado ----
inline uint32_t Reg(uint32_t i) { return Load32(0x7FC80000 + i * 4); }
inline float RegF(uint32_t i) { uint32_t v = Reg(i); float f; memcpy(&f, &v, 4); return f; }
// PA_SC_WINDOW_OFFSET (0x2080), aplicado si PA_SU_SC_MODE_CNTL.vtx_window_offset_enable (bit 16): enteros con signo de 15 bits (sesion 23)
inline void WindowOffset(int& x, int& y)
{
    x = y = 0;
    if (!(Reg(0x2205) & (1u << 16))) return;
    uint32_t v = Reg(0x2080);
    x = (int32_t)(v << 17) >> 17; y = (int32_t)((v >> 16) << 17) >> 17;
}

D3D12_BLEND MapBlend(uint32_t b)
{
    switch (b)
    {
    case 0: return D3D12_BLEND_ZERO; case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR; case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA; case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR; case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA; case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR; case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}
D3D12_BLEND MapBlendAlpha(uint32_t b)
{
    D3D12_BLEND x = MapBlend(b);
    if (x == D3D12_BLEND_SRC_COLOR) return D3D12_BLEND_SRC_ALPHA;
    if (x == D3D12_BLEND_INV_SRC_COLOR) return D3D12_BLEND_INV_SRC_ALPHA;
    if (x == D3D12_BLEND_DEST_COLOR) return D3D12_BLEND_DEST_ALPHA;
    if (x == D3D12_BLEND_INV_DEST_COLOR) return D3D12_BLEND_INV_DEST_ALPHA;
    return x;
}
D3D12_BLEND_OP MapOp(uint32_t o)
{
    switch (o) { case 1: return D3D12_BLEND_OP_SUBTRACT; case 2: return D3D12_BLEND_OP_MIN; case 3: return D3D12_BLEND_OP_MAX; case 4: return D3D12_BLEND_OP_REV_SUBTRACT; default: return D3D12_BLEND_OP_ADD; }
}

ShaderEntry& GetShader(uint32_t type, const uint32_t* code, uint32_t count, uint64_t hash)
{
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end()) return it->second;
    ShaderEntry& e = g_shaders[hash];
    { std::lock_guard<std::mutex> lk(g_xlatMutex); e.info = TranslateXenosShader(type, code, count); }
    if (e.info.ok && ApplyShaderOverrides(type, hash, e.info.hlsl)) LOG("[d3d12] sustitucion aplicada a %016llX", (unsigned long long)hash);
    if (!e.info.ok) { LOG("[d3d12] shader %016llX no traducido: %s", (unsigned long long)hash, e.info.error.c_str()); return e; }
    char name[64]; snprintf(name, sizeof(name), "%s_%016llX", type ? "ps" : "vs", (unsigned long long)hash);
    e.blob = Compile(e.info.hlsl, "main", type ? "ps_5_0" : "vs_5_0", name);
    if (!e.blob && type == 1 && e.info.hlsl.find("SampleGrad") != std::string::npos)
    {
        // Sesion 24: los gradientes dentro de flujo de control variable no compilan: se retraduce con nivel 0 fijo
        TranslatedShader t2;
        { std::lock_guard<std::mutex> lk(g_xlatMutex); g_xlatNoGrad = true; t2 = TranslateXenosShader(type, code, count); g_xlatNoGrad = false; }
        if (t2.ok) ApplyShaderOverrides(type, hash, t2.hlsl);
        if (t2.ok) { e.info = t2; e.blob = Compile(e.info.hlsl, "main", "ps_5_0", name); LOG("[d3d12] PS %016llX sin gradientes (nivel 0): %s", (unsigned long long)hash, e.blob ? "ok" : "falla"); }
    }
    e.ok = e.blob != nullptr;
    return e;
}

uint64_t g_skipped[16] = {};

// ---- texturas (sesion 8): solo mip base, formatos 8888 y DXT1/3/5 ----
struct TexEntry { ComPtr<ID3D12Resource> res; DXGI_FORMAT fmt; uint64_t dataHash = 0; uint64_t lastFrame = 0; uint64_t resolveGen = 0; uint32_t levels = 1; };
// Sesion 24: generacion de resolves por CPU por direccion de destino. Una textura se vuelve a subir si hubo un resolve nuevo a su
// direccion desde su ultima subida, aunque el hash de muestras coincida (texturas casi uniformes como la SSAO).
std::unordered_map<uint32_t, uint64_t> g_cpuResolveGen;
uint64_t g_cpuResolveCounter = 0;
uint64_t g_writeBacks = 0, g_gpuResolvesWin = 0, g_cpuResolvesWin = 0;   // volcados de texturas resueltas por GPU a memoria (estadistica, sesion 24)
void FlushPendingResolves();
void FlushResolvesIfOverlap(uint32_t lo, uint32_t hi);
bool g_pendingResolvesEmpty();
// Resolves por GPU (sesion 21): el resultado se queda en una textura de la GPU asociada a la direccion de destino, sin pasar por la CPU.
struct ResolvedTex
{
    ComPtr<ID3D12Resource> res; UINT w = 0, h = 0; bool swapRB = false; uint32_t endian = 0; uint64_t frame = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t destBase = 0, destPitch = 0, destH = 0;   // destino en memoria (para volcarla si se lee con otro formato/tamano)
    bool inMemory = false;                             // ya volcada a la memoria del juego en este estado
    uint64_t memHash = 0;                              // huella de la memoria de destino al resolver: si cambia, la CPU la ha reescrito
    uint64_t gen = 0;                                  // numero de resolve (para rehacer copias derivadas dentro del mismo frame)
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;      // sesion 24: formato del recurso (B8G8R8A8 o R32_FLOAT)
    uint32_t texFmt = 6;                               // formato de textura de Xenos del destino (6 = 8888, 2 = k_8, 36 = k_32_FLOAT)
    UINT rtv = ~0u;                                    // RTV en g_rtvHeap3 (resolves por shader)
};
// Clave de una textura resuelta por GPU con formato distinto de 8888: los bits 32-43 de ResolvedKey estan libres
inline uint64_t ResolvedFmtTag(uint32_t texFmt) { return texFmt == 6 ? 0 : (uint64_t(texFmt & 0x3F) << 32); }
std::unordered_map<uint64_t, ResolvedTex> g_resolved;      // clave: (direccion, ancho, alto)
std::unordered_map<uint32_t, uint64_t> g_resolvedLast;     // direccion -> clave del ultimo resolve por GPU
// Huella rapida de 64 dwords repartidos en los primeros 256 KB de una direccion fisica
inline uint64_t MemHash(uint32_t addr)
{
    const uint32_t* p = (const uint32_t*)(g_base + (0xA0000000u | (addr & 0x1FFFFFFF)));
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { h ^= p[(i * 1031) % 65536]; h *= 1099511628211ull; }
    return h;
}
inline uint64_t ResolvedKey(uint32_t addr, uint32_t w, uint32_t h) { return (uint64_t(addr & 0xFFFFF000) << 32) | (uint64_t(w & 0xFFFF) << 16) | (h & 0xFFFF); }
uint64_t g_gpuResolves = 0;
// Sesion 24 (como Xenia, d3d12_texture_cache.cc): los formatos de 1 o 2 canales replican canales en el host
// (RRRR, RGGG, RGBB). Sin esto, leer .y/.w de una textura k_24_8 daba 0/1 en vez de la profundidad.
static UINT HostRep(uint32_t fmt, uint32_t c)
{
    static bool off = getenv("GH6_NOHOSTREP") != nullptr;
    if (off) return c;
    switch (fmt)
    {
    case 0: case 1: case 2: case 8: case 9: case 22: case 23: case 24: case 27: case 30: case 33: case 36: case 39: case 41: case 43: case 58: case 59:
        return 0;                                   // RRRR
    case 10: case 13: case 25: case 28: case 31: case 34: case 37: case 40: case 42: case 49:
        return c > 1 ? 1 : c;                       // RGGG
    case 16: case 17: case 54: case 55: case 57:
        return c > 2 ? 2 : c;                       // RGBB
    default: return c;
    }
}

// Resolves de profundidad por GPU (sesion 21): rz = R32_FLOAT (lectura como k_24_8), rp = RGBA8 con los bits 24_8 (lectura como 8888)
struct ResolvedDepth
{
    ComPtr<ID3D12Resource> rz, rp; UINT w = 0, h = 0, rtv = 0; uint32_t endian = 0; uint64_t frame = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
std::unordered_map<uint64_t, ResolvedDepth> g_resolvedDepth;
std::unordered_map<uint32_t, uint64_t> g_resolvedDepthLast;
uint64_t g_texUploads = 0, g_texUploadBytes = 0, g_vbCopyBytes = 0;   // estadisticas por presentacion
std::unordered_map<uint64_t, TexEntry> g_textures;

uint32_t TiledOuter(uint32_t y, uint32_t pitch, uint32_t log2Bpp)
{
    uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2Bpp + 7);
    uint32_t micro = ((y & 6) << 2) << log2Bpp;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
}
uint32_t TiledInner(uint32_t x, uint32_t y, uint32_t log2Bpp, uint32_t base)
{
    uint32_t macro = (x >> 5) << (log2Bpp + 7);
    uint32_t micro = (x & 7) << log2Bpp;
    uint32_t off = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}
inline uint32_t Swap32(uint32_t v, uint32_t e)
{
    switch (e) { case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); case 2: return __builtin_bswap32(v); case 3: return (v << 16) | (v >> 16); default: return v; }
}

// Crea (o reutiliza) la textura de la constante de fetch tf y escribe su SRV en 'slot'. false -> usar blanca.
bool BindTexture(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2) return false;                    // no es constante de textura
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    if (!g_pendingResolvesEmpty())
    {   // resolves por CPU pendientes que escriben en esta textura (o sus mips): completarlos antes de leer la memoria
        uint32_t span = std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * 16u, 64u << 20);
        FlushResolvesIfOverlap(base, base + span);
        if (uint32_t ma = fc[5] & 0xFFFFF000) FlushResolvesIfOverlap(ma, ma + span);
    }
    bool tiled = fc[0] >> 31; uint32_t pitchTexels = ((fc[0] >> 22) & 0x1FF) * 32;
    uint32_t dim = (fc[5] >> 9) & 3;
    if (dim != 1 || !base) return false;                   // solo 2D
    DXGI_FORMAT df; uint32_t blockBytes, blockDim;
    int conv = 0;   // 0 copia directa, 1 = profundidad 24_8 -> float, 2 = 24_8_FLOAT -> float, 3 = componentes de 16 bits con signo/exp -> half
    // Constante de fetch: dword0 bits 2-9 = signo por componente (1 = con signo), dword3 bits 13-18 = exp_adjust (con signo)
    uint32_t signBits = (fc[0] >> 2) & 0xFF;
    int expAdjust = (int)((fc[3] >> 13) & 0x3F); if (expAdjust & 0x20) expAdjust -= 64;
    bool signedOrExp = (signBits & 0x55) != 0 || expAdjust != 0;
    switch (fmt)
    {
    case 6: case 14: df = DXGI_FORMAT_R8G8B8A8_UNORM; blockBytes = 4; blockDim = 1; break;
    case 2: df = DXGI_FORMAT_R8_UNORM; blockBytes = 1; blockDim = 1; break;
    case 18: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;
    case 19: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;
    case 20: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;
    // Sesion 19: mas formatos (tabla de formatos de Xenia)
    case 3: df = DXGI_FORMAT_B5G5R5A1_UNORM; blockBytes = 2; blockDim = 1; break;      // k_1_5_5_5
    case 4: df = DXGI_FORMAT_B5G6R5_UNORM; blockBytes = 2; blockDim = 1; break;        // k_5_6_5
    case 15: df = DXGI_FORMAT_B4G4R4A4_UNORM; blockBytes = 2; blockDim = 1; break;     // k_4_4_4_4
    case 10: df = DXGI_FORMAT_R8G8_UNORM; blockBytes = 2; blockDim = 1; break;         // k_8_8
    case 24: df = DXGI_FORMAT_R16_UNORM; blockBytes = 2; blockDim = 1; break;          // k_16
    case 30: df = DXGI_FORMAT_R16_FLOAT; blockBytes = 2; blockDim = 1; break;          // k_16_FLOAT
    case 7: df = DXGI_FORMAT_R10G10B10A2_UNORM; blockBytes = 4; blockDim = 1; break;   // k_2_10_10_10
    case 25: df = DXGI_FORMAT_R16G16_UNORM; blockBytes = 4; blockDim = 1; if (signedOrExp) { df = DXGI_FORMAT_R16G16_FLOAT; conv = 3; } break; // k_16_16
    case 31: df = DXGI_FORMAT_R16G16_FLOAT; blockBytes = 4; blockDim = 1; break;       // k_16_16_FLOAT
    case 36: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; break;          // k_32_FLOAT
    case 22: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; conv = 1; break; // k_24_8 (profundidad)
    case 23: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; conv = 2; break; // k_24_8_FLOAT
    case 26: df = DXGI_FORMAT_R16G16B16A16_UNORM; blockBytes = 8; blockDim = 1; if (signedOrExp) { df = DXGI_FORMAT_R16G16B16A16_FLOAT; conv = 3; } break; // k_16_16_16_16
    case 32: df = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; blockDim = 1; break; // k_16_16_16_16_FLOAT
    case 37: df = DXGI_FORMAT_R32G32_FLOAT; blockBytes = 8; blockDim = 1; break;       // k_32_32_FLOAT
    case 38: df = DXGI_FORMAT_R32G32B32A32_FLOAT; blockBytes = 16; blockDim = 1; break; // k_32_32_32_32_FLOAT
    case 49: df = DXGI_FORMAT_BC5_UNORM; blockBytes = 16; blockDim = 4; break;         // k_DXN
    case 59: df = DXGI_FORMAT_BC4_UNORM; blockBytes = 8; blockDim = 4; break;          // k_DXT5A
    default:
    {
        static std::unordered_map<uint64_t, int> seen;
        if (seen.size() < 64 && seen[(uint64_t(fmt) << 40) | (uint64_t(w) << 20) | h]++ == 0)
            LOG("[d3d12] formato de textura %u sin soporte (%ux%u, base %08X, frame %llu)", fmt, w, h, base, (unsigned long long)g_frameCounter);
        return false;
    }
    }
    // Profundidad resuelta por GPU leida como k_24_8 (R32_FLOAT) o como 8888 (bits empaquetados)
    if (fmt == 22 || fmt == 6)
    {
        auto last = g_resolvedDepthLast.find(base);
        auto it = (last != g_resolvedDepthLast.end() && last->second == ResolvedKey(base, w, h)) ? g_resolvedDepth.find(last->second) : g_resolvedDepth.end();
        if (it != g_resolvedDepth.end() && it->second.rz && g_frameCounter - it->second.frame <= 60)
        {
            // Diferencia de endian entre resolve y lectura = permutacion de bytes (XOR del indice): 8in16 = 1, 8in32 = 3, 16in32 = 2
            static const uint32_t emask[4] = { 0, 1, 3, 2 };
            uint32_t xm = emask[it->second.endian & 3] ^ emask[endian & 3];
            ResolvedDepth& r = it->second;
            if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                D3D12_RESOURCE_BARRIER bs[2] = { Transition(r.rz.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), Transition(r.rp.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
                g_list->ResourceBarrier(2, bs); r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            uint32_t swz = (fc[3] >> 1) & 0xFFF;
            uint32_t xmUse = fmt == 22 ? 0 : xm;
            auto map = [xmUse, fmt](uint32_t c) -> UINT { return c < 4 ? (fmt == 22 ? 0u : (c ^ xmUse)) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = fmt == 22 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
            g_dev->CreateShaderResourceView(fmt == 22 ? r.rz.Get() : r.rp.Get(), &sd, slot);
            return true;
        }
    }
    // Sesion 24: texturas resueltas por GPU en k_8 (copia 8888, canal rojo) o k_32_FLOAT (copia R32_FLOAT); mismo tamano exacto
    if (fmt == 2 || fmt == 36 || (fmt == 25 && !signedOrExp))
    {
        uint64_t k = ResolvedKey(base, w, h) ^ ResolvedFmtTag(fmt);
        auto last = g_resolvedLast.find(base);
        if (last != g_resolvedLast.end() && last->second != k)
        {
            // Sesion 24: el resolve tiene otro tamano (p. ej. 112x207 leido como 110x207): copia de la region a una textura derivada
            // en la GPU (antes se volcaba a memoria esperando a la GPU: tirones de ~50 ms)
            auto srcIt = g_resolved.find(last->second);
            if (srcIt != g_resolved.end() && srcIt->second.res && srcIt->second.texFmt == fmt && w <= srcIt->second.w + 8 && h <= srcIt->second.h + 8 &&
                (srcIt->second.inMemory || MemHash(base) == srcIt->second.memHash))
            {
                ResolvedTex& src = srcIt->second;
                ResolvedTex& dsub = g_resolved[k ^ 1];
                if (!dsub.res || dsub.gen != src.gen || dsub.w != w || dsub.h != h || dsub.fmt != src.fmt)
                {
                    if (!dsub.res || dsub.w != w || dsub.h != h || dsub.fmt != src.fmt)
                    {
                        if (dsub.res) g_deferredRelease[g_fi].push_back(dsub.res);
                        dsub = ResolvedTex{};
                        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
                        td.Format = src.fmt; td.SampleDesc.Count = 1;
                        g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dsub.res));
                        dsub.w = w; dsub.h = h; dsub.fmt = src.fmt; dsub.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    }
                    if (dsub.res)
                    {
                        D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                        if (src.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_COPY_SOURCE); src.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                        if (dsub.state != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_COPY_DEST); dsub.state = D3D12_RESOURCE_STATE_COPY_DEST; }
                        if (nb) g_list->ResourceBarrier(nb, bs);
                        D3D12_BOX box{ 0, 0, 0, std::min(w, src.w), std::min(h, src.h), 1 };
                        SafeCopyRegion(dsub.res.Get(), src.res.Get(), box, "region derivada");
                        dsub.texFmt = src.texFmt; dsub.gen = src.gen; dsub.memHash = src.memHash; dsub.inMemory = src.inMemory; dsub.frame = src.frame;
                    }
                }
                if (dsub.res)
                {
                    if (dsub.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                    {
                        auto b = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b);
                        dsub.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    }
                    uint32_t swz = (fc[3] >> 1) & 0xFFF;
                    auto map = [&](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = dsub.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                    g_dev->CreateShaderResourceView(dsub.res.Get(), &sd, slot);
                    return true;
                }
            }
        }
        if (last != g_resolvedLast.end() && last->second == k)
        {
            auto it = g_resolved.find(k);
            if (it != g_resolved.end() && it->second.res && it->second.texFmt == fmt && (it->second.inMemory || MemHash(base) == it->second.memHash))
            {
                ResolvedTex& r = it->second;
                if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                {
                    auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b);
                    r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                }
                // Un solo canal (R del render target): todos los canales de la textura lo replican (RRRR, como HostRep)
                uint32_t swz = (fc[3] >> 1) & 0xFFF;
                auto map = [&](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = r.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                g_dev->CreateShaderResourceView(r.res.Get(), &sd, slot);
                return true;
            }
        }
    }
    // Textura producida por un resolve por GPU: se usa directamente (canales: si el resolve intercambio R/B, se deshace en el SRV)
    if (fmt == 6)
    {
        auto last = g_resolvedLast.find(base);
        auto it = (last != g_resolvedLast.end() && last->second == ResolvedKey(base, w, h)) ? g_resolved.find(last->second) : g_resolved.end();
        if (last != g_resolvedLast.end() && it == g_resolved.end())
        {
            // El ultimo resolve a esta direccion tiene otro tamano (p. ej. 224x416 leido como 220x415):
            // se copia la region que se pide a una textura de ese tamano, en la GPU
            auto srcIt = g_resolved.find(last->second);
            static bool noDsub = getenv("GH6_NODSUB") != nullptr;
            if (!noDsub && srcIt != g_resolved.end() && srcIt->second.res && srcIt->second.texFmt == 6 && w <= srcIt->second.w + 8 && h <= srcIt->second.h + 8)
            {
                ResolvedTex& s = srcIt->second;
                ResolvedTex& dsub = g_resolved[ResolvedKey(base, w, h) ^ 1];   // clave derivada (bit 0 distinto)
                static bool genMode = getenv("GH6_DSUBFRAME") == nullptr;   // GH6_DSUBFRAME=1: criterio anterior (por frame)
                if (!dsub.res || (genMode ? dsub.gen != s.gen : dsub.frame != s.frame) || dsub.w != w || dsub.h != h)
                {
                    if (!dsub.res || dsub.w != w || dsub.h != h)
                    {
                        if (dsub.res) g_deferredRelease[g_fi].push_back(dsub.res);
                        dsub = ResolvedTex{};
                        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
                        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                        g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dsub.res));
                        dsub.w = w; dsub.h = h; dsub.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    }
                    if (dsub.res)
                    {
                        D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                        if (s.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(s.res.Get(), s.state, D3D12_RESOURCE_STATE_COPY_SOURCE); s.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                        if (dsub.state != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_COPY_DEST); dsub.state = D3D12_RESOURCE_STATE_COPY_DEST; }
                        if (nb) g_list->ResourceBarrier(nb, bs);
                        D3D12_BOX box{ 0, 0, 0, std::min(w, s.w), std::min(h, s.h), 1 };
                        SafeCopyRegion(dsub.res.Get(), s.res.Get(), box, "region derivada");
                        dsub.swapRB = s.swapRB; dsub.endian = s.endian; dsub.frame = s.frame; dsub.memHash = s.memHash; dsub.inMemory = s.inMemory; dsub.gen = s.gen;
                    }
                }
                if (dsub.res) it = g_resolved.find(ResolvedKey(base, w, h) ^ 1);
            }
        }
        // Sesion 23: sin limite de antiguedad (el juego reutiliza copias de hace muchos frames, p. ej. los retratos del lobby);
        // solo se descarta si la memoria de destino ha cambiado desde el resolve (la CPU la ha reescrito).
        {   // GH6_TEXAGE=<direccion hex>: antiguedad del resolve que se usa al leer esa direccion (diagnostico, sesion 24)
            static uint32_t ageAddr = getenv("GH6_TEXAGE") ? (uint32_t)strtoul(getenv("GH6_TEXAGE"), nullptr, 16) : 0;
            static uint32_t ageLogs = 0;
            if (ageAddr && base == ageAddr && ageLogs++ < 400)
                LOG("[texage] %08X %ux%u: frame %llu, resolve del frame %llu (gen %llu), encontrado %d, memoria igual %d, ultimo resolve clave igual %d",
                    base, w, h, (unsigned long long)g_frameCounter, it != g_resolved.end() ? (unsigned long long)it->second.frame : 0ull,
                    it != g_resolved.end() ? (unsigned long long)it->second.gen : 0ull, it != g_resolved.end(),
                    it != g_resolved.end() ? (int)(it->second.inMemory || MemHash(base) == it->second.memHash) : -1,
                    last != g_resolvedLast.end() ? (int)(last->second == ResolvedKey(base, w, h)) : -1);
        }
        if (it != g_resolved.end() && it->second.res && (it->second.inMemory || MemHash(base) == it->second.memHash))
        {
            static const uint32_t emask[4] = { 0, 1, 3, 2 };
            uint32_t xm = emask[it->second.endian & 3] ^ emask[endian & 3];
            ResolvedTex& r = it->second;
            if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b);
                r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            // Canal de la textura -> byte de la palabra (permutacion de endian) -> canal del RT (vista BGRA: x=R, y=G, z=B, w=A)
            const UINT chan[4] = { r.swapRB ? 2u : 0u, 1u, r.swapRB ? 0u : 2u, 3u };
            uint32_t swz = (fc[3] >> 1) & 0xFFF;
            auto map = [&](uint32_t c) -> UINT { return c < 4 ? chan[c ^ xm] : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
            g_dev->CreateShaderResourceView(r.res.Get(), &sd, slot);
            return true;
        }
        if (last != g_resolvedLast.end() && it == g_resolved.end())
        {
            static std::unordered_map<uint32_t, int> seen;
            if (seen[base]++ == 0) LOG("[d3d12] textura resuelta por GPU en %08X leida con otro tamano: %ux%u endian %u", base, w, h, endian);
        }
    }
    uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
    uint32_t pitchBlocks = std::max<uint32_t>((pitchTexels ? pitchTexels : w) / blockDim, bw);
    uint32_t log2b = blockBytes == 1 ? 0 : blockBytes == 2 ? 1 : blockBytes == 4 ? 2 : blockBytes == 8 ? 3 : 4;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    // hash rapido de los datos para detectar cambios
    // Sesion 24 (rendimiento): la huella se calcula una vez por frame y textura (era ~20 % del hilo de la GPU: hasta 64 KB por
    // bind en cada draw). Se invalida en cada resolve por CPU. GH6_TEXHASHEVERY=1 la recalcula siempre.
    static bool hashEvery = getenv("GH6_TEXHASHEVERY") != nullptr;
    struct HashMemo { uint64_t frame, rcount, dh; };
    static std::unordered_map<uint64_t, HashMemo> hashMemo;
    uint64_t memoKey = TexKey(base, fmt, w, h, pitchBlocks * 2 + (tiled ? 1 : 0), 9);
    auto memoIt = hashEvery ? hashMemo.end() : hashMemo.find(memoKey);
    bool memoHit = memoIt != hashMemo.end() && memoIt->second.frame == g_frameCounter && memoIt->second.rcount == g_cpuResolveCounter;
    uint64_t dh = memoHit ? memoIt->second.dh : 1469598103934665603ull;
    if (!memoHit) {
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    // Sesion 24: ademas, muestras repartidas por toda la textura. Con solo los primeros 16 KB, una copia cuya parte de arriba
    // no cambia (la SSAO de 640x360 en k_8, con el fondo constante) no se volvia a subir.
    static bool oldTexHash = getenv("GH6_TEXHASHOLD") != nullptr;
    if (!oldTexHash)
    {
        uint64_t bytes = uint64_t((pitchBlocks + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes;
        uint32_t dwords = (uint32_t)std::min<uint64_t>(bytes / 4, 1u << 22);
        if (dwords <= 16384)   // texturas pequenas (<= 64 KB, p. ej. k_8 de 64x64 que escribe la CPU): todos los dwords
            for (uint32_t i = 0; i < dwords; i++) { dh ^= ((const uint32_t*)src)[i]; dh *= 1099511628211ull; }
        else
            for (uint32_t i = 0; i < 256; i++) { dh ^= ((const uint32_t*)src)[(uint64_t(i) * dwords / 256 + (i * 977) % 61) % dwords]; dh *= 1099511628211ull; }
    }
    if (!hashEvery) { if (hashMemo.size() > 65536) hashMemo.clear(); hashMemo[memoKey] = { g_frameCounter, g_cpuResolveCounter, dh }; }
    }
    // Sesion 24: mipmaps. dword 5 bits 12-31 = direccion de los mips, dword 4 bits 6-9 = nivel maximo (como Xenia)
    auto l2c = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
    auto l2f = [](uint32_t v) { uint32_t l = 0; while ((2u << l) <= v) l++; return l; };
    uint32_t mipAddr = fc[5] & 0xFFFFF000, mipMaxLvl = (fc[4] >> 6) & 0xF;
    bool packedMips = ((fc[5] >> 11) & 1) != 0;
    static bool noMips = getenv("GH6_NOMIPS") != nullptr;
    uint32_t levels = 1;
    if (!noMips && mipAddr && mipMaxLvl > 0 && conv != 1 && conv != 2)
        levels = std::min(mipMaxLvl, l2f(std::max(w, h))) + 1;
    // Desplazamiento (en bloques) de un nivel dentro del tile de mips empaquetados (GetPackedMipOffset de Xenia, 2D)
    auto packedOff = [&](uint32_t L, uint32_t& xb, uint32_t& yb) -> bool {
        uint32_t lw = l2c(w), lh = l2c(h), ls = std::min(lw, lh);
        xb = yb = 0;
        if (ls > 4 + L) return false;
        uint32_t pbase = ls > 4 ? ls - 4 : 0, pm = L - pbase, x = 0, y = 0;
        if (pm < 3) { if (lw > lh) y = 16u >> pm; else x = 16u >> pm; }
        else if (lw > lh) x = (1u << (lw - pbase)) >> (pm - 2);
        else y = (1u << (lh - pbase)) >> (pm - 2);
        xb = x / blockDim; yb = y / blockDim;
        return true;
    };
    // El formato DXGI va en la clave: la misma textura puede subirse como UNORM o como half segun el signo de la constante (sesion 23)
    // Sesion 24: llave sin solapes (antes (base << 20) ^ (w << 32)... se pisaban: dos texturas distintas compartian recurso y la
    // copia de tamano equivocado invalidaba la lista entera -> pantalla negra en el modo Quest)
    uint64_t key = TexKey(base, fmt, w, h, (tiled ? 1u : 0u) | (uint32_t(df) << 8) | (levels << 20), 0);
    TexEntry& e = g_textures[key];
    static bool texNoCacheAll = getenv("GH6_TEXNOCACHE") != nullptr;   // diagnostico: resube siempre (GH6_TEXNOCACHE=<fmt> solo ese formato)
    static int texNoCacheFmt = texNoCacheAll ? atoi(getenv("GH6_TEXNOCACHE")) : -1;
    bool texNoCache = texNoCacheAll && (texNoCacheFmt <= 1 || (int)fmt == texNoCacheFmt);
    static bool noResolveGen = getenv("GH6_NORESOLVEGEN") != nullptr;
    uint64_t rgen = 0;
    if (!noResolveGen) { auto gi = g_cpuResolveGen.find(base & 0xFFFFF000); if (gi != g_cpuResolveGen.end()) rgen = gi->second; }
    if (!e.res || e.dataHash != dh || texNoCache || e.resolveGen != rgen)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = bw * blockDim; td.Height = bh * blockDim;
            td.DepthOrArraySize = 1; td.MipLevels = (UINT16)levels; td.Format = df; td.SampleDesc.Count = 1;
            if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&e.res)))) return false;
            e.fmt = df; e.levels = levels;
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); g_list->ResourceBarrier(1, &b); }
        UINT rowPitch = (bw * blockBytes + 255) & ~255u;
        UINT64 o = RingAlloc(UINT64(rowPitch) * bh, 512);
        if (o == ~0ull) return false;
        uint8_t* dst = g_ringMap[g_fi] + o;
        // Sesion 24: mips empaquetados (dword 5 bit 11): si el lado mas corto es <= 16 texeles, el nivel base esta desplazado 16 texeles
        // dentro del tile de 32x32 bloques (en Y si es mas ancha que alta, si no en X), como GetPackedMipOffset de Xenia.
        // Sin esto las texturas finas de la interfaz (barra del medidor 256x16, marcadores 32x16) salian como rectangulos.
        uint32_t pkX = 0, pkY = 0;
        {
            static bool noPacked = getenv("GH6_NOPACKEDMIPS") != nullptr;
            auto l2c = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
            uint32_t lw = l2c(w), lh = l2c(h);
            if (!noPacked && ((fc[5] >> 11) & 1) && std::min(lw, lh) <= 4)
            { if (lw > lh) pkY = 16 / blockDim; else pkX = 16 / blockDim; }
        }
        // Destilea un nivel: lsrc = inicio del nivel, lpitch = pitch en bloques (alineado a 32 si es tiled), (lox, loy) = desplazamiento
        auto fillRows = [&](const uint8_t* src, uint32_t bw, uint32_t y0, uint32_t y1, uint32_t lpitch, uint32_t pkX, uint32_t pkY, uint8_t* dst, UINT rowPitch)
        {
        for (uint32_t by = y0; by < y1; by++)
        {
            uint32_t outer = tiled ? TiledOuter(by + pkY, lpitch, log2b) : 0;
            for (uint32_t bx = 0; bx < bw; bx++)
            {
                uint32_t so = tiled ? TiledInner(bx + pkX, by + pkY, log2b, outer) : ((by + pkY) * lpitch + bx + pkX) * blockBytes;
                if (blockBytes == 1) { dst[by * rowPitch + bx] = src[so]; continue; }
                if (blockBytes == 2)
                {
                    // Texel de 16 bits: se aplica el swap a la palabra de 32 bits que lo contiene y se extrae su mitad
                    uint32_t wd = Swap32(*(const uint32_t*)(src + (so & ~3u)), endian);
                    *(uint16_t*)(dst + by * rowPitch + bx * 2) = (uint16_t)(wd >> ((so & 2) * 8));
                    continue;
                }
                const uint32_t* sp = (const uint32_t*)(src + so);
                uint32_t* dp = (uint32_t*)(dst + by * rowPitch + bx * blockBytes);
                for (uint32_t k = 0; k < blockBytes / 4; k++) dp[k] = Swap32(sp[k], endian);
                if (conv == 3)
                {
                    // Cada componente de 16 bits -> half, con su signo y el exp_adjust de la constante
                    uint16_t* c16 = (uint16_t*)dp;
                    float mul = ldexpf(1.0f, expAdjust);
                    for (uint32_t k = 0; k < blockBytes / 2; k++)
                    {
                        bool sgn = ((signBits >> (k * 2)) & 3) == 1;
                        float f = sgn ? std::max(-1.0f, (int16_t)c16[k] / 32767.0f) : c16[k] / 65535.0f;
                        c16[k] = FloatToHalf(f * mul);
                    }
                }
                else if (conv)
                {
                    // 24 bits de profundidad en la parte alta, stencil en la baja
                    float z = DecodeDepth24(dp[0] >> 8, conv == 2);
                    memcpy(dp, &z, 4);
                }
            }
        }
        };
        // Sesion 24 (tirones): las texturas grandes (>= 128x128 bloques) se destilean en varios hilos; antes 30-85 ms por carga
        auto fillLevel = [&](const uint8_t* src, uint32_t bw, uint32_t bh, uint32_t lpitch, uint32_t pkX, uint32_t pkY, uint8_t* dst, UINT rowPitch)
        {
            static const unsigned nthr = getenv("GH6_TEXTHREADS") ? (unsigned)std::max(1, atoi(getenv("GH6_TEXTHREADS")))
                                                                  : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
            if (nthr <= 1 || uint64_t(bw) * bh < 16384) { fillRows(src, bw, 0, bh, lpitch, pkX, pkY, dst, rowPitch); return; }
            unsigned n = std::min<unsigned>(nthr, (bh + 31) / 32);
            std::vector<std::thread> th;
            uint32_t step = ((bh + n - 1) / n + 31) & ~31u;   // franjas de 32 filas de bloques (un tile)
            for (uint32_t y0 = step; y0 < bh; y0 += step)
                th.emplace_back([&, y0] { fillRows(src, bw, y0, std::min(bh, y0 + step), lpitch, pkX, pkY, dst, rowPitch); });
            fillRows(src, bw, 0, std::min(bh, step), lpitch, pkX, pkY, dst, rowPitch);
            for (auto& t : th) t.join();
        };
        fillLevel(src, bw, bh, tiled ? ((pitchBlocks + 31) & ~31u) : pitchBlocks, pkX, pkY, dst, rowPitch);
        // GH6_TEXDUMP=<base hex>: guarda los datos ya destileados de esa textura (formato sin bloques) en texdump_*.raw (sesion 24)
        if (static const char* td = getenv("GH6_TEXDUMP"); td && (base & 0xFFFFF000) == ((uint32_t)strtoul(td, nullptr, 16) & 0xFFFFF000))
        {
            static int dumps = 0;
            static int onlyFmt = getenv("GH6_TEXDUMPFMT") ? atoi(getenv("GH6_TEXDUMPFMT")) : -1;            // filtro de formato
            // desde ese frame, o t<segundos> desde el arranque
            static const char* tm = getenv("GH6_TEXDUMPMIN");
            static long long minFrame = (tm && tm[0] != 't') ? atoll(tm) : 0;
            static DWORD t0 = GetTickCount();
            bool timeOk = !(tm && tm[0] == 't') || GetTickCount() - t0 >= (DWORD)(atof(tm + 1) * 1000);
            if (dumps < 12 && (onlyFmt < 0 || (int)fmt == onlyFmt) && (long long)g_frameCounter >= minFrame && timeOk)
            {
                dumps++;
                char pn[128]; snprintf(pn, sizeof(pn), "texdump_%08X_f%u_%ux%u_bpp%u_%llu.raw", base, fmt, bw, bh, blockBytes, (unsigned long long)g_frameCounter);
                if (FILE* f = fopen(pn, "wb")) { for (uint32_t y = 0; y < bh; y++) fwrite(dst + y * rowPitch, 1, bw * blockBytes, f); fclose(f); }
            }
        }
        D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        sl.PlacedFootprint.Offset = o; sl.PlacedFootprint.Footprint = { df, bw * blockDim, bh * blockDim, 1, rowPitch };
        CheckedCopy(&d, 0, 0, 0, &sl, nullptr);
        // Sesion 24: niveles 1..n desde mip_address (disposicion de GetGuestTextureLayout de Xenia): cada nivel ocupa el tamano de la
        // potencia de dos desplazada, alineado a 32x32 bloques y a 4 KB; desde el nivel empaquetado (lado corto <= 16) todos van en
        // un mismo tile con los desplazamientos de packedOff.
        if (levels > 1)
        {
            const uint8_t* msrc = g_base + (0xA0000000u | (mipAddr & 0x1FFFFFFF));
            uint32_t P = packedMips ? (l2c(std::min(w, h)) > 4 ? l2c(std::min(w, h)) - 4 : 0) : 99;
            uint32_t npw = 1u << l2c(w), nph = 1u << l2c(h);
            auto lvlPitch = [&](uint32_t l) { uint32_t t = std::max(npw >> l, 1u); uint32_t pb = (((t + blockDim - 1) / blockDim) + 31) & ~31u;
                if (!tiled) pb = ((pb * blockBytes + 255) & ~255u) / blockBytes; return pb; };
            auto lvlRows = [&](uint32_t l) { uint32_t t = std::max(nph >> l, 1u); return (((t + blockDim - 1) / blockDim) + 31) & ~31u; };
            auto lvlStride = [&](uint32_t l) { return (uint64_t(lvlPitch(l)) * blockBytes * lvlRows(l) + 4095) & ~4095ull; };
            for (uint32_t L = 1; L < levels; L++)
            {
                uint32_t stored = std::min(L, P);       // nivel cuyo almacenamiento contiene a L
                uint64_t off = 0;
                if (P != 0) for (uint32_t l = 1; l < stored; l++) off += lvlStride(l);
                uint32_t storeLvl = (P == 0) ? 0 : stored;
                uint32_t ox = 0, oy = 0;
                if (L >= P) packedOff(L, ox, oy);
                uint32_t lw = std::max(w >> L, 1u), lh = std::max(h >> L, 1u);
                uint32_t lbw = (lw + blockDim - 1) / blockDim, lbh = (lh + blockDim - 1) / blockDim;
                UINT lrp = (lbw * blockBytes + 255) & ~255u;
                UINT64 lo = RingAlloc(UINT64(lrp) * lbh, 512);
                if (lo == ~0ull) break;
                fillLevel(msrc + off, lbw, lbh, lvlPitch(storeLvl), ox, oy, g_ringMap[g_fi] + lo, lrp);
                D3D12_TEXTURE_COPY_LOCATION md{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; md.SubresourceIndex = L;
                D3D12_TEXTURE_COPY_LOCATION ms{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                ms.PlacedFootprint.Offset = lo; ms.PlacedFootprint.Footprint = { df, lbw * blockDim, lbh * blockDim, 1, lrp };
                CheckedCopy(&md, 0, 0, 0, &ms, nullptr);
            }
        }
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b);
        e.dataHash = dh; e.resolveGen = rgen;
        // Estadistica (sesion 21): texturas que se vuelven a subir mucho
        g_texUploads++; g_texUploadBytes += uint64_t(rowPitch) * bh;
        CmdTrace("tex2D %08X fmt %u %ux%u df %d bloques %ux%u rp %u off %llu", base, fmt, w, h, (int)df, bw, bh, rowPitch, (unsigned long long)o);
        static std::unordered_map<uint64_t, uint32_t> perTex;
        uint32_t c = ++perTex[key];
        if (c == 100) LOG("[d3d12] textura %08X (%ux%u formato %u) subida 100 veces (contenido cambiante)", base, w, h, fmt);
    }
    e.lastFrame = g_frameCounter;
    // swizzle de la constante (3 bits por componente: 0-3 = xyzw, 4 = 0, 5 = 1)
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = e.levels ? e.levels : 1;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}
} // namespace

// MSAA de RB_SURFACE_INFO (bits 16-17: 0 = 1x, 1 = 2x, 2 = 4x). En la EDRAM las muestras ocupan sitio:
// 2x = 1x2 (alto doble), 4x = 2x2. Se emula con una superficie de muestras (sesion 21), igual que las celdas de la EDRAM real,
// de modo que un clear a 640 con 4x cae en las mismas celdas que la escena a 1280 sin MSAA.
void MsaaScale(uint32_t& sx, uint32_t& sy)
{
    uint32_t m = (Reg(0x2000) >> 16) & 3;
    static bool noMsaa = getenv("GH6_NOMSAA") != nullptr;
    sx = (!noMsaa && m >= 2) ? 2 : 1; sy = (!noMsaa && m >= 1) ? 2 : 1;
}
// ---- texturas 3D (sesion 21): tablas de correccion de color y similares ----
// Direccion tiled 3D de Xenos (texture_address.h de Xenia): macro-tiles de 32x16x4, bancos alternos por (y/8) ^ (z/4).
static int64_t Tiled3DOffset(int32_t x, int32_t y, int32_t z, uint32_t pitchAligned, uint32_t heightAligned, uint32_t log2b)
{
    int32_t outer = ((((z >> 2) * int32_t(heightAligned >> 4) + (y >> 4)) * int32_t(pitchAligned >> 5)) + (x >> 5)) << 7;
    int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    int64_t oi = int64_t(outer | inner) << log2b;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1, pipe = ((x >> 3) & 3) ^ (bank << 1);
    return int64_t((uint32_t(y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oi & 0xF) | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}
std::unordered_map<uint64_t, TexEntry> g_textures3D;
bool BindTexture3D(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2) return false;
    uint32_t dim = (fc[5] >> 9) & 3;
    if (dim != 2) return false;
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    // dword2 de una textura 3D: ancho 11 bits, alto 11 bits, profundidad 10 bits (menos uno)
    uint32_t w = (fc[2] & 0x7FF) + 1, h = ((fc[2] >> 11) & 0x7FF) + 1, dpt = ((fc[2] >> 22) & 0x3FF) + 1;
    if (!g_pendingResolvesEmpty()) FlushResolvesIfOverlap(base, base + std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * std::max(dpt, 4u) * 16u, 64u << 20));
    bool tiled = fc[0] >> 31; uint32_t pitch = std::max<uint32_t>(((fc[0] >> 22) & 0x1FF) * 32, w);
    DXGI_FORMAT df; uint32_t bpp;
    switch (fmt)
    {
    case 6: case 14: df = DXGI_FORMAT_R8G8B8A8_UNORM; bpp = 4; break;
    case 7: df = DXGI_FORMAT_R10G10B10A2_UNORM; bpp = 4; break;
    case 2: df = DXGI_FORMAT_R8_UNORM; bpp = 1; break;
    default:
    {
        static std::unordered_map<uint32_t, int> seen;
        if (seen[fmt]++ == 0) LOG("[d3d12] textura 3D con formato %u sin soporte (%ux%ux%u)", fmt, w, h, dpt);
        return false;
    }
    }
    if (!base || w > 2048 || h > 2048 || dpt > 1024) return false;
    uint32_t log2b = bpp == 1 ? 0 : 2;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    uint64_t dh = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    uint64_t key = TexKey(base, fmt, w, h, dpt, 3);
    TexEntry& e = g_textures3D[key];
    if (!e.res || e.dataHash != dh)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; td.Width = w; td.Height = h; td.DepthOrArraySize = (UINT16)dpt;
            td.MipLevels = 1; td.Format = df; td.SampleDesc.Count = 1;
            if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&e.res)))) return false;
            e.fmt = df;
            LOG("[d3d12] textura 3D %ux%ux%u formato %u %s en %08X", w, h, dpt, fmt, tiled ? "tiled" : "lineal", base);
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); g_list->ResourceBarrier(1, &b); }
        UINT rowPitch = (w * bpp + 255) & ~255u;
        UINT64 o = RingAlloc(UINT64(rowPitch) * h * dpt, 512);
        if (o == ~0ull) return false;
        uint8_t* dst = g_ringMap[g_fi] + o;
        uint32_t pitchAligned = (pitch + 31) & ~31u, heightAligned = (h + 31) & ~31u;
        for (uint32_t z = 0; z < dpt; z++)
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    uint64_t so = tiled ? (uint64_t)Tiled3DOffset(x, y, z, pitchAligned, heightAligned, log2b)
                                        : ((uint64_t(z) * heightAligned + y) * pitchAligned + x) * bpp;
                    uint8_t* dp = dst + (uint64_t(z) * h + y) * rowPitch + x * bpp;
                    if (bpp == 1) *dp = src[so];
                    else *(uint32_t*)dp = Swap32(*(const uint32_t*)(src + so), endian);
                }
        D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        sl.PlacedFootprint.Offset = o; sl.PlacedFootprint.Footprint = { df, w, h, dpt, rowPitch };
        if (bpp == 4 && w >= 32 && h >= 32 && dpt >= 16)
        {
            // Diagnostico (sesion 23): esquinas y centro de la tabla 3D (una tabla neutra va de 000000 a FFFFFF)
            static int logged = 0;
            if (logged++ < 6)
            {
                auto at = [&](uint32_t x, uint32_t y, uint32_t z) { return *(uint32_t*)(dst + (uint64_t(z) * h + y) * rowPitch + x * 4); };
                uint32_t m = w - 1, n = h - 1, q = dpt - 1;
                LOG("[d3d12] tabla 3D %08X: (0,0,0)=%08X (m,0,0)=%08X (0,m,0)=%08X (0,0,m)=%08X (m,m,m)=%08X centro=%08X", base,
                    at(0, 0, 0), at(m, 0, 0), at(0, n, 0), at(0, 0, q), at(m, n, q), at(w / 2, h / 2, dpt / 2));
            }
        }
        CmdTrace("tex3D %08X fmt %u %ux%ux%u rp %u off %llu", base, fmt, w, h, dpt, rowPitch, (unsigned long long)o);
        CheckedCopy(&d, 0, 0, 0, &sl, nullptr);
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b);
        e.dataHash = dh;
    }
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}

// ---- texturas cubo (sesion 21): 6 caras 2D consecutivas, cada una alineada a 32 bloques de alto y a 4 KB ----
std::unordered_map<uint64_t, TexEntry> g_texturesCube;
bool BindTextureCube(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    static bool noCube = getenv("GH6_NOCUBE") != nullptr;   // diagnostico
    if (noCube) return false;
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2 || ((fc[5] >> 9) & 3) != 3) return false;
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    if (!g_pendingResolvesEmpty())
    {   // resolves por CPU pendientes que escriben en esta textura (o sus mips): completarlos antes de leer la memoria
        uint32_t span = std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * 16u, 64u << 20);
        FlushResolvesIfOverlap(base, base + span);
        if (uint32_t ma = fc[5] & 0xFFFFF000) FlushResolvesIfOverlap(ma, ma + span);
    }
    bool tiled = fc[0] >> 31; uint32_t pitchTexels = ((fc[0] >> 22) & 0x1FF) * 32;
    DXGI_FORMAT df; uint32_t blockBytes, blockDim;
    switch (fmt)
    {
    case 6: case 14: df = DXGI_FORMAT_R8G8B8A8_UNORM; blockBytes = 4; blockDim = 1; break;
    case 18: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;
    case 19: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;
    case 20: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;
    case 32: df = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; blockDim = 1; break;
    default:
    {
        static std::unordered_map<uint32_t, int> seen;
        if (seen[fmt]++ == 0) LOG("[d3d12] textura cubo con formato %u sin soporte (%ux%u)", fmt, w, h);
        return false;
    }
    }
    if (!base || w > 4096 || h > 4096) return false;
    uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
    uint32_t pitchBlocks = std::max<uint32_t>((pitchTexels ? pitchTexels : w) / blockDim, bw);
    uint32_t log2b = blockBytes == 4 ? 2 : blockBytes == 8 ? 3 : 4;
    uint32_t pitchAligned = (pitchBlocks + 31) & ~31u, heightAligned = (bh + 31) & ~31u;
    uint64_t faceStride = (uint64_t(pitchAligned) * heightAligned * blockBytes + 0xFFF) & ~0xFFFull;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    uint64_t dh = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    uint64_t key = TexKey(base, fmt, w, h, 0, 6);
    TexEntry& e = g_texturesCube[key];
    if (!e.res || e.dataHash != dh)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = bw * blockDim; td.Height = bh * blockDim; td.DepthOrArraySize = 6;
            td.MipLevels = 1; td.Format = df; td.SampleDesc.Count = 1;
            if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&e.res)))) return false;
            e.fmt = df;
            LOG("[d3d12] textura cubo %ux%u formato %u %s en %08X", w, h, fmt, tiled ? "tiled" : "lineal", base);
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); g_list->ResourceBarrier(1, &b); }
        UINT rowPitch = (bw * blockBytes + 255) & ~255u;
        UINT64 faceBytes = (UINT64(rowPitch) * bh + 511) & ~511ull;
        UINT64 o = RingAlloc(faceBytes * 6, 512);
        if (o == ~0ull) return false;
        for (uint32_t face = 0; face < 6; face++)
        {
            const uint8_t* fsrc = src + face * faceStride;
            uint8_t* dst = g_ringMap[g_fi] + o + face * faceBytes;
            for (uint32_t by = 0; by < bh; by++)
            {
                uint32_t outer = tiled ? TiledOuter(by, pitchAligned, log2b) : 0;
                for (uint32_t bx = 0; bx < bw; bx++)
                {
                    uint32_t so = tiled ? TiledInner(bx, by, log2b, outer) : (by * pitchBlocks + bx) * blockBytes;
                    const uint32_t* sp = (const uint32_t*)(fsrc + so);
                    uint32_t* dp = (uint32_t*)(dst + by * rowPitch + bx * blockBytes);
                    for (uint32_t k = 0; k < blockBytes / 4; k++) dp[k] = Swap32(sp[k], endian);
                }
            }
            D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = face;
            D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            if (face == 0) CmdTrace("texCubo %08X fmt %u %ux%u bloques %ux%u rp %u off %llu", base, fmt, w, h, bw, bh, rowPitch, (unsigned long long)o);
            sl.PlacedFootprint.Offset = o + face * faceBytes; sl.PlacedFootprint.Footprint = { df, bw * blockDim, bh * blockDim, 1, rowPitch };
            CheckedCopy(&d, 0, 0, 0, &sl, nullptr);
        }
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b);
        e.dataHash = dh;
    }
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}

EdramRt* GetEdramRt(uint32_t infoReg = 0x2001)   // RB_COLOR_INFO; MRT: RB_COLOR1_INFO 0x2003, 2 = 0x2004, 3 = 0x2005 (sesion 24)
{
    uint32_t base = Reg(infoReg) & 0xFFF, pitch = Reg(0x2000) & 0x3FFF, xfmt = (Reg(infoReg) >> 16) & 0xF;
    if (!pitch) pitch = kRtW;
    {   // diagnostico (sesion 24): sesgo de exponente del color (bits 20-25)
        static std::unordered_map<uint32_t, int> seenBias;
        uint32_t ci = Reg(infoReg);
        if ((ci >> 20) & 0x3F && seenBias.size() < 32 && seenBias[ci]++ == 0) LOG("[edram] RB_COLOR_INFO %08X con sesgo de exponente %d (pitch %u)", ci, (int)(((ci >> 20) & 0x3F) ^ 0x20) - 0x20, pitch);
    }
    { uint32_t sx, sy; MsaaScale(sx, sy); pitch *= sx; }
    static bool only8888 = getenv("GH6_RT8888") != nullptr;   // diagnostico: comportamiento anterior
    if (only8888) xfmt = 0;
    DXGI_FORMAT f; UINT bpp; float scale;
    EdramFormat(xfmt, f, bpp, scale);
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM) xfmt = 0;   // 8888 y 8888_GAMMA comparten superficie
    uint32_t key = (xfmt << 28) | (base << 14) | pitch;
    auto found = g_edramRts.find(key);
    if (found != g_edramRts.end()) return &found->second;
    if (g_edramRts.size() >= 60) return nullptr;
    EdramRt& rt = g_edramRts[key];
    rt.w = pitch; rt.h = 2 * kEdramH; rt.rtv = kFrames + 1 + (UINT)g_edramRts.size(); rt.base = base;
    rt.xfmt = xfmt; rt.fmt = f; rt.bpp = bpp; rt.outScale = scale;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = rt.w; td.Height = rt.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ f, { 0, 0, 0, 1 } };
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt.res)))) { g_edramRts.erase(key); return nullptr; }
    g_dev->CreateRenderTargetView(rt.res.Get(), nullptr, Rtv(rt.rtv));
    const float black[4] = { 0, 0, 0, 1 };
    g_list->ClearRenderTargetView(Rtv(rt.rtv), black, 0, nullptr);
    LOG("[edram] nueva superficie: base %u tiles, pitch %u, formato %u (DXGI %d)", base, pitch, xfmt, (int)f);
    return &rt;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT i) { auto h = g_dsvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_dsvSize; return h; }

// RB_DEPTH_INFO (0x2002): bits 0-11 base en tiles, bit 16 formato (0 = D24S8, 1 = D24FS8). El pitch es el de RB_SURFACE_INFO.
DepthRt* GetDepthRt()
{
    uint32_t base = Reg(0x2002) & 0xFFF, pitch = Reg(0x2000) & 0x3FFF;
    if (!pitch) pitch = kRtW;
    { uint32_t sx, sy; MsaaScale(sx, sy); pitch *= sx; }
    uint32_t key = (base << 16) | pitch;
    auto found = g_depthRts.find(key);
    if (found != g_depthRts.end()) return &found->second;
    if (g_depthRts.size() >= 60) return nullptr;
    DepthRt& d = g_depthRts[key];
    d.w = pitch; d.h = 2 * kEdramH; d.dsv = (UINT)g_depthRts.size() - 1;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = d.w; td.Height = d.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kDepthFmt; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFmt; cv.DepthStencil = { 1.0f, 0 };
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&d.res)))) { g_depthRts.erase(key); return nullptr; }
    g_dev->CreateDepthStencilView(d.res.Get(), nullptr, Dsv(d.dsv));
    g_list->ClearDepthStencilView(Dsv(d.dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    LOG("[edram] nueva superficie de profundidad: base %u tiles, pitch %u, formato %s", base, pitch, (Reg(0x2002) >> 16) & 1 ? "D24FS8" : "D24S8");
    return &d;
}

// Valor de RB_DEPTH_CLEAR (depth en los 24 bits altos, stencil en los 8 bajos) -> float
float DecodeDepth24(uint32_t v24, bool isFloat)
{
    if (!isFloat) return v24 / 16777215.0f;
    // 20e4: 4 bits de exponente (sesgo 15), 20 de mantisa
    uint32_t e = v24 >> 20, m = v24 & 0xFFFFF;
    if (!e) return ldexpf((float)m, -20 - 14);
    return ldexpf(1.0f + m / 1048576.0f, (int)e - 15);
}

D3D12_COMPARISON_FUNC MapCmp(uint32_t f)
{
    static const D3D12_COMPARISON_FUNC t[8] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS };
    return t[f & 7];
}
D3D12_STENCIL_OP MapStencilOp(uint32_t o)
{
    static const D3D12_STENCIL_OP t[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
        D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
    return t[o & 7];
}

// Con GH6_D3D12_DEBUG=1: vuelca al log los mensajes de la capa de depuracion (errores y avisos)
void DrainDebugMessages()
{
    static ComPtr<ID3D12InfoQueue> q;
    static bool tried = false;
    if (!tried) { tried = true; if (getenv("GH6_D3D12_DEBUG")) g_dev.As(&q); }
    if (!q) return;
    static int total = 0, warns = 0;
    UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && total < 200; i++)
    {
        SIZE_T len = 0; q->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = (D3D12_MESSAGE*)buf.data();
        if (!SUCCEEDED(q->GetMessage(i, m, &len))) continue;
        bool err = m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
        if (err || (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING && warns++ < 20)) { total++; LOG("[d3d12 debug] %.600s", m->pDescription); }
    }
    q->ClearStoredMessages();
}

// Traza de operaciones que graban comandos especiales (sesion 23): si Close falla, se vuelca para saber cual fue
std::vector<std::string> g_cmdTrace;
std::string g_pendingOp;
std::string g_swapResolves;   // destinos de resolve desde el ultimo swap (GH6_SWAPLOG)
int g_closeFailures = 0;   // con GH6_FLUSHEACH: operacion que se iba a registrar cuando fallo el Close (las subidas se graban antes de su traza)
void FlushAndWait();
void CmdTrace(const char* fmt, ...)
{
    // GH6_FLUSHEACH=1: cierra y ejecuta la lista antes de cada operacion, para que un fallo de Close senale la anterior
    static bool flushEnv = getenv("GH6_FLUSHEACH") != nullptr;
    bool flushEach = flushEnv;
    char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (flushEach && g_frameOpen) { g_pendingOp = b; FlushAndWait(); g_pendingOp.clear(); }
    if (g_cmdTrace.size() >= 400) return;
    g_cmdTrace.emplace_back(b);
}
void DumpCmdTrace(HRESULT hr)
{
    static int dumps = 0;
    g_closeFailures++;
    // Tras un Close fallido la lista queda en error para siempre (los siguientes Close devuelven E_FAIL aunque se reinicie):
    // se crea una lista nueva y se cierra, para que el codigo siguiente la reinicie con normalidad (sesion 23).
    {
        ComPtr<ID3D12GraphicsCommandList> fresh;
        if (SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[g_fi].Get(), nullptr, IID_PPV_ARGS(&fresh))))
        {
            fresh->Close();
            g_list = fresh;
        }
    }
    if (dumps++ < 8)
    {
        LOG("[d3d12] Close fallo (0x%08lX): ultimas operaciones de la lista (%zu):", (unsigned long)hr, g_cmdTrace.size());
        size_t from = (dumps == 1 || g_cmdTrace.size() <= 40) ? 0 : g_cmdTrace.size() - 40;   // el primer fallo, entero
        for (size_t i = from; i < g_cmdTrace.size(); i++) LOG("[d3d12]   %s", g_cmdTrace[i].c_str());
        if (!g_pendingOp.empty()) LOG("[d3d12]   (siguiente: %s)", g_pendingOp.c_str());
    }
}

void FlushAndWait()
{
    struct FlushTimer { double t0 = QpcMs(); ~FlushTimer() { g_hFlushMs += QpcMs() - t0; g_hFlushes++; } } ftimer;
    {   // GH6_FLUSHSTATS=1: de donde se llama (direccion de retorno, simbolizable con llvm-symbolizer) (sesion 24)
        static const bool on = getenv("GH6_FLUSHSTATS") != nullptr;
        if (on)
        {
            static std::unordered_map<uintptr_t, uint64_t> sites; static uint64_t total = 0;
            uintptr_t ra = (uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleW(nullptr) + 0x140000000ull;
            sites[ra]++;
            if (++total % 500 == 0) { std::string l; for (auto& kv : sites) { char b[64]; snprintf(b, sizeof(b), " %llX=%llu", (unsigned long long)kv.first, (unsigned long long)kv.second); l += b; } LOG("[flushstats]%s", l.c_str()); }
        }
    }
    DrainDebugMessages();
    HRESULT chr = g_list->Close();
    if (FAILED(chr))
    {
        // Lista invalida: se descarta (ejecutarla haria perder el dispositivo para siempre)
        static int n = 0; if (n++ < 10) LOG("[d3d12] Close de la lista fallo (0x%08lX, draws en el frame %llu): se descarta", (unsigned long)chr, (unsigned long long)g_frameDraws);
        DumpCmdTrace(chr);
    }
    else
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
    }
    g_cmdTrace.clear();
    g_queue->Signal(g_fence.Get(), g_nextFence);
    g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 5000);
    g_alloc[g_fi]->Reset();
    g_list->Reset(g_alloc[g_fi].Get(), nullptr);
    g_boundRt = nullptr;
}

// ---- conversion de pixeles en los resolves de superficies que no son 8888 (sesion 23, segun resolve.xesli de Xenia) ----
static float HalfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -f : f;
}
// Pixel de nuestro render target -> 4 floats con el rango del formato de Xenos
static void UnpackRtPixel(DXGI_FORMAT f, const uint8_t* px, float v[4])
{
    v[0] = v[1] = v[2] = 0; v[3] = 1;
    switch (f)
    {
    case DXGI_FORMAT_R16G16_SNORM:            // 16_16 de Xenos: -32..32 guardado como SNORM x 1/32
        for (int k = 0; k < 2; k++) v[k] = std::max(-1.0f, ((const int16_t*)px)[k] / 32767.0f) * 32.0f; v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R16G16B16A16_SNORM:      // 16_16_16_16
        for (int k = 0; k < 4; k++) v[k] = std::max(-1.0f, ((const int16_t*)px)[k] / 32767.0f) * 32.0f; break;
    case DXGI_FORMAT_R16G16_FLOAT:
        v[0] = HalfToFloat(((const uint16_t*)px)[0]); v[1] = HalfToFloat(((const uint16_t*)px)[1]); v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int k = 0; k < 4; k++) v[k] = HalfToFloat(((const uint16_t*)px)[k]); break;
    case DXGI_FORMAT_R32_FLOAT: memcpy(&v[0], px, 4); v[1] = v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R32G32_FLOAT: memcpy(v, px, 8); v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        uint32_t w = *(const uint32_t*)px;
        v[0] = (w & 1023) / 1023.0f; v[1] = ((w >> 10) & 1023) / 1023.0f; v[2] = ((w >> 20) & 1023) / 1023.0f; v[3] = (w >> 30) / 3.0f; break;
    }
    default: break;
    }
}
static bool PackResolveSupported(uint32_t fmt)
{
    switch (fmt) { case 6: case 7: case 25: case 26: case 31: case 32: case 36: case 37: return true; default: return false; }
}
// 4 floats -> formato de destino del resolve (palabras de 32 bits en orden de memoria antes del endian)
static void PackResolvePixel(uint32_t fmt, const float v[4], uint32_t w[2])
{
    auto un = [](float x, float mx) { return (uint32_t)(std::clamp(std::isnan(x) ? 0.0f : x, 0.0f, 1.0f) * mx + 0.5f); };
    auto fb = [](float x) { uint32_t u; memcpy(&u, &x, 4); return u; };
    switch (fmt)
    {
    case 6:  w[0] = un(v[0], 255) | (un(v[1], 255) << 8) | (un(v[2], 255) << 16) | (un(v[3], 255) << 24); break;            // 8_8_8_8
    case 7:  w[0] = un(v[0], 1023) | (un(v[1], 1023) << 10) | (un(v[2], 1023) << 20) | (un(v[3], 3) << 30); break;        // 2_10_10_10
    case 25: w[0] = un(v[0], 65535) | (un(v[1], 65535) << 16); break;                                                    // 16_16 (sin signo)
    case 26: w[0] = un(v[0], 65535) | (un(v[1], 65535) << 16); w[1] = un(v[2], 65535) | (un(v[3], 65535) << 16); break;  // 16_16_16_16
    case 31: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); break;                                     // 16_16_FLOAT
    case 32: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); w[1] = FloatToHalf(v[2]) | (uint32_t(FloatToHalf(v[3])) << 16); break;
    case 36: w[0] = fb(v[0]); break;                                                                                     // 32_FLOAT
    case 37: w[0] = fb(v[0]); w[1] = fb(v[1]); break;                                                                    // 32_32_FLOAT
    default: break;
    }
}

// Resolve: copia un rectangulo de la superficie EDRAM actual a memoria fisica (tiled), y clear opcional.
static long long DrawLogFrame();
// Sesion 24 (rendimiento): resolves por CPU diferidos. Antes cada uno hacia FlushAndWait (esperar a que la GPU acabe todo)
// y convertia los pixeles al momento: ~33 % del hilo de comandos en canciones. Ahora la copia a memoria de lectura se graba
// en la lista y la conversion se aplaza hasta que alguien necesita esa memoria: una textura o vertices que la solapan, un
// paquete que la CPU del juego puede observar (escrituras a memoria, eventos, interrupciones, swap) o el final del lote.
// Se espera una sola vez por lote. Experimental y desactivado: GH6_DEFERRESOLVE=1 (se cerraba al cargar escenarios).
namespace {
struct PendingResolve { UINT64 off; uint32_t lo, hi; std::function<void(const uint8_t*)> fn; uint64_t sig = 0; uint64_t frame = 0; };
uint64_t DeferSig(uint32_t lo, uint32_t hi) { uint64_t h = 1469598103934665603ull; for (uint32_t a = lo; a + 4 <= hi; a += 4093 & ~3u) { h ^= *(const uint32_t*)(g_base + (0xA0000000u | a)); h *= 1099511628211ull; } return h; }
std::vector<PendingResolve> g_pendingResolves;
uint64_t g_flushTotal = 0, g_flushOverlap = 0, g_deferredCount = 0;
ComPtr<ID3D12Resource> g_readbackDefer;
UINT64 g_readbackDeferOff = 0;
constexpr UINT64 kReadbackDeferSize = 64ull << 20;
void FlushPendingResolves()
{
    if (g_pendingResolves.empty()) return;
    g_flushTotal++; g_deferredCount += g_pendingResolves.size();
    FlushAndWait();
    uint8_t* p = nullptr; g_readbackDefer->Map(0, nullptr, (void**)&p);
    for (auto& pr : g_pendingResolves)
    {
        static int logged = 0;
        if (logged < 40 && DeferSig(pr.lo, pr.hi) != pr.sig) { logged++; LOG("[defer] la memoria %08X-%08X cambio antes de completar el resolve (frame %llu -> %llu)", pr.lo, pr.hi, (unsigned long long)pr.frame, (unsigned long long)g_frameCounter); }
    }
    if (p) for (auto& pr : g_pendingResolves) pr.fn(p + pr.off);
    if (p) g_readbackDefer->Unmap(0, nullptr);
    g_pendingResolves.clear();
    g_readbackDeferOff = 0;
    g_cpuResolveCounter++;
}
bool g_pendingResolvesEmpty() { return g_pendingResolves.empty(); }
void FlushResolvesIfOverlap(uint32_t lo, uint32_t hi)
{
    uint32_t len = hi - lo; lo &= 0x1FFFFFFF; hi = lo + len;
    for (auto& pr : g_pendingResolves) if (pr.lo < hi && lo < pr.hi) { g_flushOverlap++; FlushPendingResolves(); return; }
}

} // namespace

void Resolve()
{
    uint32_t ctrl = Reg(0x2318), destBase = Reg(0x2319), destPitchReg = Reg(0x231A), destInfo = Reg(0x231B);
    uint32_t src = ctrl & 7, cmd = (ctrl >> 20) & 3;
    bool colorClear = (ctrl >> 8) & 1;
    // Rectangulo: 3 vertices float2 del stream vf0 del propio draw
    uint32_t vdw0 = Reg(0x4800), vdw1 = Reg(0x4801);
    const uint8_t* vb = g_base + (0xA0000000u | (vdw0 & 0x1FFFFFFC));
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 3; i++)
    {
        uint32_t ux = ((const uint32_t*)vb)[i * 2], uy = ((const uint32_t*)vb)[i * 2 + 1];
        if ((vdw1 & 3) == 2) { ux = __builtin_bswap32(ux); uy = __builtin_bswap32(uy); }
        float fx, fy; memcpy(&fx, &ux, 4); memcpy(&fy, &uy, 4);
        minx = std::min(minx, fx); maxx = std::max(maxx, fx); miny = std::min(miny, fy); maxy = std::max(maxy, fy);
    }
    {
        int wox, woy; WindowOffset(wox, woy);   // el rectangulo del resolve tambien se desplaza (como Xenia)
        minx += wox; maxx += wox; miny += woy; maxy += woy;
    }
    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    // Rectangulo en pixeles logicos; en la superficie de muestras se escala por el MSAA (msx, msy)
    uint32_t msx, msy; MsaaScale(msx, msy);
    int x0 = std::max(0, (int)minx), y0 = std::max(0, (int)miny);
    uint32_t destPitch = destPitchReg & 0x3FFF, destH = (destPitchReg >> 16) & 0x3FFF;
    // Los vertices del rectangulo son inclusivos (0..1279 para 1280 de ancho): se suma 1 y se recorta al destino (sesion 21)
    int x1 = std::min((int)(rt->w / msx), (int)maxx + 1), y1 = std::min((int)(rt->h / msy), (int)maxy + 1);
    if (destPitch) x1 = std::min(x1, x0 + (int)destPitch);
    if (destH) y1 = std::min(y1, y0 + (int)destH);
    uint32_t fmt = (destInfo >> 7) & 0x3F, endian = destInfo & 7; bool swapRB = (destInfo >> 24) & 1;
    // RB_COPY_DEST_INFO bit 3 = destino 3D (array), bits 4-6 = rebanada: se escribe con el direccionamiento tiled 3D (sesion 23)
    static bool no3dResolve = getenv("GH6_NO3DRESOLVE") != nullptr;
    bool destArray = !no3dResolve && ((destInfo >> 3) & 1); uint32_t destSlice = (destInfo >> 4) & 7;
    int w = x1 - x0, h = y1 - y0;
    { char rb[48]; snprintf(rb, sizeof(rb), "%08X(s%u f%u e%u) ", destBase, src, fmt, endian); if (g_swapResolves.size() < 400) g_swapResolves += rb; }
    CmdTrace("resolve src %u cmd %u fmt %u rect (%d,%d)-(%d,%d) dest %08X pitch %u rt %ux%u fmt %d msaa %ux%u ctrl %08X", src, cmd, fmt, x0, y0, x1, y1, destBase, destPitch, rt->w, rt->h, (int)rt->fmt, msx, msy, ctrl);
    if ((long long)g_frameCounter == DrawLogFrame())   // sesion 24: resolves del frame registrado
        LOG("[drawresolve] src=%u cmd=%u rect=(%d,%d)-(%d,%d) -> %08X pitch %u fmt %u surf=%08X color0=%08X depth=%08X clear=%d", src, cmd, x0, y0, x1, y1, destBase, destPitch, fmt, Reg(0x2000), Reg(0x2001), Reg(0x2002), (int)colorClear);
    static std::unordered_map<uint64_t, int> seenResolve;
    uint64_t rkey = (uint64_t(src) << 56) ^ (uint64_t(fmt) << 48) ^ (uint64_t(destPitch) << 24) ^ (uint64_t(cmd) << 20) ^ (uint64_t(Reg(0x2000) >> 16) << 8);
    if (seenResolve.size() < 64 && seenResolve[rkey]++ == 0) { LOG("[edram] (msaa %u, frame %llu, formato color %u, base %u) resolve src=%u", (Reg(0x2000) >> 16) & 3, (unsigned long long)g_frameCounter, (Reg(0x2001) >> 16) & 0xF, Reg(0x2001) & 0xFFF, src); LOG("[edram] resolve src=%u cmd=%u rect=(%d,%d)-(%d,%d) -> %08X pitch %u alto %u fmt %u endian %u swap %d clear %d",
                                     src, cmd, x0, y0, x1, y1, destBase, destPitch, destH, fmt, endian, (int)swapRB, (int)colorClear); }
    // Sesion 19: resolve de profundidad (src 4) -> memoria en formato 24_8 (D24S8) tiled de 32 bits
    DepthRt* depthSrc = (src == 4 && w > 0 && h > 0 && destPitch && !getenv("GH6_NODEPTH")) ? GetDepthRt() : nullptr;
    {   // GH6_RESOLVELOG=1 (diagnostico): todos los resolves con frame y destino
        static const bool rlog = getenv("GH6_RESOLVELOG") != nullptr; static int rn = 0;
        static const uint32_t rlogAddr = rlog ? (uint32_t)strtoul(getenv("GH6_RESOLVELOG"), nullptr, 16) : 0;
        if (rlog && (rlogAddr > 1 ? destBase == rlogAddr : src == 4) && rn++ < 100000) LOG("[resolvelog] f%llu src=%u %dx%d -> %08X pitch %u fmt %u depthRt=%p surf=%08X color0=%08X depth=%08X prevPS=%016llX", (unsigned long long)g_frameCounter, src, w, h, destBase, destPitch, fmt, (void*)depthSrc,
            Reg(0x2000), Reg(0x2001), Reg(0x2002), (unsigned long long)g_curPsHash);
    }
    static bool cpuResolveDepth = getenv("GH6_CPURESOLVE") != nullptr || getenv("GH6_CPUDEPTH") != nullptr;
    if (depthSrc && g_depthCopyPso && !cpuResolveDepth)
    {
        int dw = std::min(w, (int)(depthSrc->w / msx) - x0), dhh = std::min(h, (int)(depthSrc->h / msy) - y0);
        if (dw > 0 && dhh > 0)
        {
            uint64_t k = ResolvedKey(destBase, dw, dhh);
            ResolvedDepth& r = g_resolvedDepth[k];
            if (!r.rz)
            {
                static UINT nextRtv = 0;
                if (nextRtv + 2 > 64) { g_resolvedDepth.erase(k); goto depthCpu; }
                D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = dw; td.Height = dhh; td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                td.Format = DXGI_FORMAT_R32_FLOAT;
                g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r.rz));
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r.rp));
                if (!r.rz || !r.rp) { g_resolvedDepth.erase(k); goto depthCpu; }
                r.w = dw; r.h = dhh; r.rtv = nextRtv; nextRtv += 2; r.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                auto h0 = g_rtvHeap2->GetCPUDescriptorHandleForHeapStart(); h0.ptr += r.rtv * inc;
                g_dev->CreateRenderTargetView(r.rz.Get(), nullptr, h0); h0.ptr += inc;
                g_dev->CreateRenderTargetView(r.rp.Get(), nullptr, h0);
            }
            {
                D3D12_RESOURCE_BARRIER bs[3]; UINT nb = 0;
                bs[nb++] = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                if (r.state != D3D12_RESOURCE_STATE_RENDER_TARGET)
                {
                    bs[nb++] = Transition(r.rz.Get(), r.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    bs[nb++] = Transition(r.rp.Get(), r.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    r.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                g_list->ResourceBarrier(nb, bs);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = kDepthSrvFmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                // Descriptor propio del area del frame: varias copias por frame no pueden compartir hueco (la GPU los lee al ejecutar)
                UINT dslot = 6 + g_fi;
                if (g_descCursor + 1 <= kDescPerFrame) { dslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
                g_dev->CreateShaderResourceView(depthSrc->res.Get(), &sd, SrvCpu(dslot));
                UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                auto h0 = g_rtvHeap2->GetCPUDescriptorHandleForHeapStart(); h0.ptr += r.rtv * inc;
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { h0, h0 }; rtvs[1].ptr += inc;
                g_list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
                g_boundRt = nullptr;
                D3D12_VIEWPORT vp{ 0, 0, (float)dw, (float)dhh, 0, 1 }; D3D12_RECT sr{ 0, 0, dw, dhh };
                g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
                g_list->SetPipelineState(g_depthCopyPso.Get());
                g_list->SetGraphicsRootSignature(g_depthCopyRootSig.Get());
                ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
                g_list->SetDescriptorHeaps(1, heaps);
                int consts[4] = { x0 * (int)msx, y0 * (int)msy, (int)msx, (int)msy };
                g_list->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
                g_list->SetGraphicsRootDescriptorTable(1, SrvGpu(dslot));
                g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_list->DrawInstanced(3, 1, 0, 0);
                auto back = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                g_list->ResourceBarrier(1, &back);
            }
            r.endian = endian; r.frame = g_frameCounter;
            g_resolvedDepthLast[destBase & 0xFFFFF000] = k;
            g_resolvedLast.erase(destBase & 0xFFFFF000);
            g_resolves++; g_gpuResolves++;
            depthSrc = nullptr;   // hecho: no pasar por la CPU
        }
    }
depthCpu:
    if (depthSrc && g_readback)
    {
        FlushPendingResolves();
        g_resolvedDepthLast.erase(destBase & 0xFFFFF000);
        int dw = std::min(w, (int)(depthSrc->w / msx) - x0), dhh = std::min(h, (int)(depthSrc->h / msy) - y0);
        UINT rowPitch = ((UINT)(dw * msx) * 4 + 255) & ~255u;
        auto a = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &a);
        DXGI_FORMAT planeFmt = DXGI_FORMAT_R32_TYPELESS;
        if (!g_depth32)
        {   // D24: formato copiable del plano de profundidad segun el driver (palabra de 32 bits con la profundidad en los 24 bajos)
            D3D12_RESOURCE_DESC rd = depthSrc->res->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            g_dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
            planeFmt = fp.Footprint.Format;
        }
        D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { planeFmt, (UINT)(dw * msx), (UINT)(dhh * msy), 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION sl{ depthSrc->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;   // plano de profundidad
        D3D12_BOX box{ (UINT)(x0 * msx), (UINT)(y0 * msy), 0, (UINT)((x0 + dw) * msx), (UINT)((y0 + dhh) * msy), 1 };
        CheckedCopy(&dst, 0, 0, 0, &sl, &box);
        auto b = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); g_list->ResourceBarrier(1, &b);
        FlushAndWait();
        uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
        if (!p) { LOG("[d3d12] Map del readback (profundidad) fallo; dispositivo: 0x%08lX", (unsigned long)g_dev->GetDeviceRemovedReason()); return; }
        uint8_t* out = g_base + (0xA0000000u | (destBase & 0x1FFFFFFF));
        uint32_t pitchAligned = (destPitch + 31) & ~31u, maxH = destH ? destH : 0x3FFF;
        for (int y = 0; y < dhh && (uint32_t)y < maxH; y++)
        {
            uint32_t outer = TiledOuter((uint32_t)y, pitchAligned, 2);
            for (int x = 0; x < dw && (uint32_t)x < destPitch; x++)
            {
                uint32_t v;
                if (g_depth32) { float z; memcpy(&z, p + (y * msy) * rowPitch + (x * msx) * 4, 4); v = (uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8; }
                else { uint32_t raw; memcpy(&raw, p + (y * msy) * rowPitch + (x * msx) * 4, 4); v = (raw & 0xFFFFFF) << 8; }
                *(uint32_t*)(out + TiledInner((uint32_t)x, (uint32_t)y, 2, outer)) = endian <= 3 ? Swap32(v, endian) : v;
            }
        }
        g_readback->Unmap(0, nullptr);
        g_resolves++;
    }
    // Bytes por texel del formato de destino del resolve (0 = sin soporte)
    uint32_t destBpp = 0;
    switch (fmt)
    {
    case 2: destBpp = 1; break;                                             // k_8
    case 3: case 4: case 10: case 15: case 24: case 30: destBpp = 2; break; // 16 bits
    case 6: case 7: case 25: case 31: case 36: destBpp = 4; break;
    case 26: case 32: case 37: destBpp = 8; break;
    }
    bool rt8888 = rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    // Sesion 21: 8888 -> 8888 / k_8; otras superficies -> copia directa si el tamano del texel coincide
    // Superficies que no son 8888: se convierten a float y se empaquetan en el formato de destino (destinos de 32 y 64 bits)
    bool supported = destBpp && (rt8888 ? (fmt == 6 || fmt == 2 || fmt == 3 || fmt == 4 || fmt == 15)
                                        : (destBpp == 4 || destBpp == 8) && PackResolveSupported(fmt));
    // RB_COPY_DEST_INFO bits 16-21: sesgo de exponente con signo (el valor se multiplica por 2^sesgo)
    int expBias = (int)((destInfo >> 16) & 0x3F); if (expBias & 0x20) expBias -= 64;
    float expScale = ldexpf(1.0f, expBias);
    { static std::unordered_map<uint64_t, int> seenRb; if (expBias && seenRb.size() < 32 && seenRb[(uint64_t(destBase) << 8) | uint8_t(expBias)]++ == 0)
        LOG("[edram] resolve con sesgo de exponente %d -> %08X fmt %u (superficie formato %u, %ux%u)", expBias, destBase, fmt, rt->xfmt, w, h); }
    if (!(src == 4 && depthSrc) && !(w > 0 && h > 0 && src < 4 && cmd != 3 && supported && destPitch))
    {
        static std::unordered_map<uint32_t, int> seenSkip;
        if (seenSkip.size() < 64 && seenSkip[destBase]++ == 0)
            LOG("[edram] resolve OMITIDO -> %08X: src=%u cmd=%u rect=(%d,%d)-(%d,%d) fmt %u, superficie formato %u (bpp %u), pitch destino %u, msaa %u",
                destBase, src, cmd, x0, y0, x1, y1, fmt, rt->xfmt, rt->bpp, destPitch, (Reg(0x2000) >> 16) & 3);
    }
    static bool cpuResolve = getenv("GH6_CPURESOLVE") != nullptr;
    static bool fbMem = getenv("GH6_FBMEM") != nullptr;   // frontbuffer (endian 0) por CPU si se presenta desde memoria
    // Las copias pequenas (<= 64x64, p. ej. las rebanadas de la tabla 3D de color que el juego dibuja) van por la CPU directas a memoria:
    // asi sirven tambien a texturas 3D/cubo que leen varias copias a la vez (sesion 23: retratos negros y escenas oscuras)
    // Sesion 24 (rendimiento): ademas de 8888 -> 8888 sin MSAA, van por GPU (sin esperar a la GPU ni convertir en la CPU):
    // 8888 con MSAA (media de muestras con shader), 8888 -> k_8 y k_32_FLOAT -> k_32_FLOAT. Se leen como textura resuelta.
    // GH6_OLDGPURESOLVE=1 = solo el caso anterior.
    static const bool oldGpuResolve = getenv("GH6_OLDGPURESOLVE") != nullptr;
    uint32_t gpuTexFmt = 0; bool gpuShader = false;
    if (!cpuResolve && !destArray && !(fbMem && endian == 0) && w > 0 && h > 0 && src < 4 && cmd != 3 && destPitch)
    {
        bool single = msx == 1 && msy == 1;
        if (rt8888 && fmt == 6 && single) gpuTexFmt = 6;
        else if (!oldGpuResolve && rt8888 && fmt == 6 && g_colorResolvePso && g_rtvHeap3) { gpuTexFmt = 6; gpuShader = true; }
        else if (!oldGpuResolve && rt8888 && fmt == 2 && single) gpuTexFmt = 2;
        else if (!oldGpuResolve && rt->fmt == DXGI_FORMAT_R32_FLOAT && fmt == 36 && single && expBias == 0) gpuTexFmt = 36;
        else if (!oldGpuResolve && rt->fmt == DXGI_FORMAT_R16G16_SNORM && fmt == 25 && single && expBias == 0 && g_colorResolvePso1616 && g_rtvHeap3)
        { gpuTexFmt = 25; gpuShader = true; }
    }
    bool gpuPath = gpuTexFmt != 0;
    if (gpuPath)
    {
        uint64_t rkey2 = ResolvedKey(destBase, w, h) ^ ResolvedFmtTag(gpuTexFmt);
        ResolvedTex& r = g_resolved[rkey2];
        g_resolvedLast[destBase & 0xFFFFF000] = rkey2;
        {
            static uint32_t ageAddr = getenv("GH6_TEXAGE") ? (uint32_t)strtoul(getenv("GH6_TEXAGE"), nullptr, 16) : 0;
            static uint32_t rlogs = 0;
            if (ageAddr && (destBase & 0xFFFFF000) == ageAddr && rlogs++ < 400)
                LOG("[texage] resolve -> %08X en el frame %llu: %ux%u desde base %u (rect %d,%d-%d,%d), gpu %d", destBase, (unsigned long long)g_frameCounter, w, h, rt->base, x0, y0, x1, y1, (int)gpuPath);
        }
        DXGI_FORMAT resFmt = gpuTexFmt == 36 ? DXGI_FORMAT_R32_FLOAT : gpuTexFmt == 25 ? (g_1616Sat ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_FLOAT) : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (r.res && (r.fmt != resFmt || (gpuShader && r.rtv == ~0u) || r.w != (UINT)w || r.h != (UINT)h))
        {   // otro formato o hace falta poder dibujar en ella: se recrea (el recurso viejo se libera cuando la GPU acabe el frame)
            g_deferredRelease[g_fi].push_back(r.res); r.res.Reset();
        }
        if (!r.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = resFmt; td.SampleDesc.Count = 1;
            bool wantRt = gpuShader;
            if (wantRt && r.rtv == ~0u && g_rtvHeap3Next >= kRtvHeap3Size) wantRt = false;   // sin RTV libres
            if (gpuShader && !wantRt) gpuPath = false;                                         // MSAA por la CPU
            else
            {
                if (wantRt) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.res)))) gpuPath = false;
                else
                {
                    r.fmt = resFmt;
                    if (wantRt)
                    {
                        if (r.rtv == ~0u) r.rtv = g_rtvHeap3Next++;
                        UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                        auto h0 = g_rtvHeap3->GetCPUDescriptorHandleForHeapStart(); h0.ptr += r.rtv * inc;
                        g_dev->CreateRenderTargetView(r.res.Get(), nullptr, h0);
                    }
                }
            }
            r.w = w; r.h = h; r.state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (!gpuPath) { g_resolvedLast.erase(destBase & 0xFFFFF000); g_resolved.erase(rkey2); }
        }
        if (gpuPath && gpuShader)
        {
            ResolvedTex& rr = g_resolved[rkey2];
            // Media de muestras con un triangulo a pantalla completa sobre la textura resuelta
            D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
            bs[nb++] = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (rr.state != D3D12_RESOURCE_STATE_RENDER_TARGET) { bs[nb++] = Transition(rr.res.Get(), rr.state, D3D12_RESOURCE_STATE_RENDER_TARGET); rr.state = D3D12_RESOURCE_STATE_RENDER_TARGET; }
            g_list->ResourceBarrier(nb, bs);
            UINT dslot = 6 + g_fi;
            if (g_descCursor + 1 <= kDescPerFrame) { dslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = gpuTexFmt == 25 ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            g_dev->CreateShaderResourceView(rt->res.Get(), &sd, SrvCpu(dslot));
            UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto h0 = g_rtvHeap3->GetCPUDescriptorHandleForHeapStart(); h0.ptr += rr.rtv * inc;
            g_list->OMSetRenderTargets(1, &h0, FALSE, nullptr);
            g_boundRt = nullptr;
            D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 }; D3D12_RECT sr{ 0, 0, w, h };
            g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
            g_list->SetPipelineState(gpuTexFmt == 25 ? (g_1616Sat ? g_colorResolvePso1616.Get() : g_colorResolvePso1616F.Get()) : g_colorResolvePso.Get());
            g_list->SetGraphicsRootSignature(g_colorResolveRootSig.Get());
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
            g_list->SetDescriptorHeaps(1, heaps);
            static bool noAvgS = getenv("GH6_NOSAMPLEAVG") != nullptr;
            int consts[8] = { x0 * (int)msx, y0 * (int)msy, (int)msx, (int)msy, noAvgS ? 0 : (int)((ctrl >> 4) & 7), gpuTexFmt == 25 ? (g_1616Sat ? 1 : 2) : 0, 0, 0 };
            g_list->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
            g_list->SetGraphicsRootDescriptorTable(1, SrvGpu(dslot));
            g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_list->DrawInstanced(3, 1, 0, 0);
            auto back = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_list->ResourceBarrier(1, &back);
        }
        else if (gpuPath)
        {
            ResolvedTex& rr = g_resolved[rkey2];
            if (rr.state != D3D12_RESOURCE_STATE_COPY_DEST) { auto b0 = Transition(rr.res.Get(), rr.state, D3D12_RESOURCE_STATE_COPY_DEST); g_list->ResourceBarrier(1, &b0); rr.state = D3D12_RESOURCE_STATE_COPY_DEST; }
            auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &a);
            D3D12_BOX box{ (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
            SafeCopyRegion(rr.res.Get(), rt->res.Get(), box, "resolve");
            auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); g_list->ResourceBarrier(1, &b);
        }
        if (gpuPath)
        {
            ResolvedTex& r = g_resolved[rkey2];
            r.texFmt = gpuTexFmt;
            r.swapRB = swapRB; r.endian = endian; r.frame = g_frameCounter;
            r.destBase = destBase; r.destPitch = destPitch; r.destH = destH; r.inMemory = false;
            r.memHash = MemHash(destBase); static uint64_t genCounter = 0; r.gen = ++genCounter;
            g_gpuResolvesWin++;
            g_resolvedDepthLast.erase(destBase & 0xFFFFF000);
            g_resolves++; g_gpuResolves++;
        }
    }
    if (!gpuPath && w > 0 && h > 0 && src < 4 && cmd != 3 && supported && destPitch)
    {
        // Un resolve por CPU a esta direccion invalida la copia en la GPU (sin liberar el recurso: la GPU puede estar usandolo)
        g_resolvedLast.erase(destBase & 0xFFFFF000);
        g_cpuResolveGen[destBase & 0xFFFFF000] = ++g_cpuResolveCounter;
        g_cpuResolvesWin++;
        static const bool cpuStats = getenv("GH6_CPURESOLVESTATS") != nullptr;
        if (cpuStats)
        {   // estadistica (sesion 24): por que va por la CPU
            static std::unordered_map<std::string, uint64_t> why; static uint64_t total = 0;
            char k[160]; snprintf(k, sizeof(k), "%ux%u fmt%u sup%u arr%d msaa%ux%u dest%08X", w, h, fmt, rt->xfmt, (int)destArray, msx, msy, destBase);
            why[k]++;
            if (++total % 160 == 0)
            {
                std::vector<std::pair<uint64_t, std::string>> v; for (auto& kv : why) v.push_back({ kv.second, kv.first });
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 12; i++) LOG("[cpuresolve] %llu x %s", (unsigned long long)v[i].first, v[i].second.c_str());
                why.clear();
            }
        }
        if (!g_readback)
        {
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048ull * 16 * kEdramH; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
        }
        UINT rowPitch = ((UINT)(w * msx) * rt->bpp + 255) & ~255u;
        // Desactivado por defecto: con el diferido el juego se cerraba al cargar un escenario (las copias tardias llegan
        // despues de que el juego reutiliza esa memoria). GH6_DEFERRESOLVE=1 lo activa (experimental).
        static const bool noDefer = getenv("GH6_DEFERRESOLVE") == nullptr;
        UINT64 need = UINT64(rowPitch) * (UINT64)(h * msy);
        bool defer = !noDefer && need + 512 <= kReadbackDeferSize;
        if (defer && !g_readbackDefer)
        {
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kReadbackDeferSize; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rbh{ D3D12_HEAP_TYPE_READBACK };
            if (FAILED(g_dev->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readbackDefer)))) defer = false;
        }
        UINT64 rbOff = 0;
        if (defer)
        {
            if (g_readbackDeferOff + need + 512 > kReadbackDeferSize) FlushPendingResolves();
            rbOff = (g_readbackDeferOff + 511) & ~511ull;
            g_readbackDeferOff = rbOff + need;
        }
        else FlushPendingResolves();
        auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &a);
        D3D12_TEXTURE_COPY_LOCATION dst{ defer ? g_readbackDefer.Get() : g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { rt->fmt, (UINT)(w * msx), (UINT)(h * msy), 1, rowPitch };
        dst.PlacedFootprint.Offset = rbOff;
        D3D12_TEXTURE_COPY_LOCATION sl{ rt->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
        D3D12_BOX box{ (UINT)(x0 * msx), (UINT)(y0 * msy), 0, (UINT)(x1 * msx), (UINT)(y1 * msy), 1 };
        CheckedCopy(&dst, 0, 0, 0, &sl, &box);
        auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); g_list->ResourceBarrier(1, &b);
        const UINT rtBpp = rt->bpp; const DXGI_FORMAT rtFmt = rt->fmt;
        auto convert = [=](const uint8_t* p) {
        uint8_t* out = g_base + (0xA0000000u | (destBase & 0x1FFFFFFF));
        uint32_t pitchAligned = (destPitch + 31) & ~31u;
        uint32_t maxH = destH ? destH : 0x3FFF;
        uint32_t log2d = destBpp == 1 ? 0 : destBpp == 2 ? 1 : destBpp == 4 ? 2 : 3;
        for (int y = 0; y < h && (uint32_t)y < maxH; y++)
        {
            uint32_t outer = TiledOuter((uint32_t)y, pitchAligned, log2d);
            for (int x = 0; x < w && (uint32_t)x < destPitch; x++)
            {
                const uint8_t* px = p + (y * msy) * rowPitch + (x * msx) * rtBpp;   // una muestra por pixel
                // Sesion 24 (como Xenia): RB_COPY_CONTROL bits 4-6 = muestra a copiar; 4 = media de 0/1, 5 = media de 2/3,
                // 6 = media de las 4. El juego reduce la escena a la mitad reinterpretandola como MSAA 4x con media.
                uint8_t avgPx[16];
                static bool noAvg = getenv("GH6_NOSAMPLEAVG") != nullptr;
                uint32_t sampleSel = (ctrl >> 4) & 7;
                if (!noAvg && rt8888 && sampleSel >= 4 && sampleSel <= 6 && (msx > 1 || msy > 1))
                {
                    // Muestras en la superficie: (0,0) (1,0) (0,1) (1,1) dentro del bloque msx x msy
                    const uint8_t* s[4] = { px, px + (msx > 1 ? 4 : 0), px + (msy > 1 ? rowPitch : 0), px + (msy > 1 ? rowPitch : 0) + (msx > 1 ? 4 : 0) };
                    int first = sampleSel == 5 ? 2 : 0, count = sampleSel == 6 ? 4 : 2;
                    if (sampleSel == 4 && msy > 1 && msx == 1) { s[1] = s[2]; }   // 2x: muestras arriba/abajo
                    for (int c = 0; c < 4; c++)
                    {
                        uint32_t sum = 0;
                        for (int k = 0; k < count; k++) sum += s[first + k][c];
                        avgPx[c] = (uint8_t)((sum + count / 2) / count);
                    }
                    px = avgPx;
                }
                uint8_t* o = out + (destArray ? (uint64_t)Tiled3DOffset(x, y, (int32_t)destSlice, pitchAligned, (destH + 31) & ~31u, log2d)
                                              : (uint64_t)TiledInner((uint32_t)x, (uint32_t)y, log2d, outer));
                if (rt8888 && fmt == 2) { *o = px[2]; continue; }   // k_8: canal rojo
                if (rt8888 && destBpp == 2)
                {
                    // 8888 -> 16 bits (sesion 21). px = B G R A. Con swapRB se intercambian R y B.
                    uint32_t r = swapRB ? px[0] : px[2], g = px[1], b = swapRB ? px[2] : px[0], a = px[3];
                    uint16_t v = fmt == 3 ? (uint16_t)(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))      // 1_5_5_5
                               : fmt == 4 ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))                       // 5_6_5
                               : (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));                // 4_4_4_4
                    // El endian del destino se aplica por palabras de 16 bits (8in16 = intercambio de bytes)
                    if (endian == 1 || endian == 2) v = __builtin_bswap16(v);
                    *(uint16_t*)o = v;
                    continue;
                }
                if (rt8888)
                {
                    uint32_t v = swapRB ? (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24))   // bytes B G R A
                                        : (px[2] | (px[1] << 8) | (px[0] << 16) | (px[3] << 24));  // bytes R G B A
                    *(uint32_t*)o = endian <= 3 ? Swap32(v, endian) : v;
                    continue;
                }
                // Sesion 23 (como resolve.xesli de Xenia): se desempaqueta el valor del render target a float y se empaqueta en el
                // formato de destino con su sesgo de exponente. 16_16 y 16_16_16_16 son de punto fijo -32..32 (nuestro SNORM x32)
                // y el resolve escribe datos SIN signo: no es una copia bit a bit.
                static bool raw16 = getenv("GH6_RAWRESOLVE") != nullptr;   // diagnostico: copia bit a bit (comportamiento anterior)
                if (raw16 && rtBpp == destBpp)
                {
                    for (uint32_t k = 0; k < destBpp / 4; k++) ((uint32_t*)o)[k] = Swap32(((const uint32_t*)px)[k], endian);
                    continue;
                }
                float v[4];
                UnpackRtPixel(rtFmt, px, v);
                for (int k = 0; k < 4; k++) v[k] *= expScale;
                uint32_t words[2] = {};
                PackResolvePixel(fmt, v, words);
                for (uint32_t k = 0; k < destBpp / 4; k++) ((uint32_t*)o)[k] = Swap32(words[k], endian);
            }
        }
        };
        if (defer)
        {
            uint32_t span = ((destPitch + 31) & ~31u) * (((destH ? destH : (uint32_t)h) + 31) & ~31u) * std::max<uint32_t>(destBpp, 1);
            if (destArray) span = 0x01000000;   // texturas 3D: rango amplio (conservador)
            uint32_t lo = destBase & 0x1FFFFFFF;
            g_pendingResolves.push_back({ rbOff, lo, lo + std::max<uint32_t>(span, 4096), convert, DeferSig(lo, lo + std::max<uint32_t>(span, 4096)), g_frameCounter });
            static const bool deferNow = getenv("GH6_DEFERNOW") != nullptr;   // diagnostico: completar al momento
            if (deferNow) FlushPendingResolves();
        }
        else
        {
            FlushAndWait();
            uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
            if (!p)
            {
                static int n = 0;
                if (n++ < 3) LOG("[d3d12] Map del readback fallo; dispositivo: 0x%08lX", (unsigned long)g_dev->GetDeviceRemovedReason());
                return;
            }
            convert(p);
            g_readback->Unmap(0, nullptr);
        }
        g_resolves++;
        if ((destBase & 0x1FFFFFFF) == (GpuGetStats().frontbuffer.load() & 0x1FFFFFFF) || destBase == 0x17DF0000) g_fbResolves++;
    }
    if (colorClear && w > 0 && h > 0)
    {
        uint32_t c = Reg(0x231E);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        D3D12_RECT r{ (LONG)(x0 * msx), (LONG)(y0 * msy), (LONG)(x1 * msx), (LONG)std::min<uint32_t>(y1 * msy, rt->h) };
        g_list->ClearRenderTargetView(Rtv(rt->rtv), col, 1, &r);
    }
    // Clear de profundidad/stencil (RB_COPY_CONTROL bit 9, valor en RB_DEPTH_CLEAR 0x231D)
    if (((ctrl >> 9) & 1) && w > 0 && h > 0 && !getenv("GH6_NODEPTH"))
    {
        if (DepthRt* dr = GetDepthRt())
        {
            uint32_t v = Reg(0x231D);
            float z = DecodeDepth24(v >> 8, (Reg(0x2002) >> 16) & 1);
            D3D12_RECT r{ (LONG)(x0 * msx), (LONG)(y0 * msy), std::min((LONG)(x1 * msx), (LONG)dr->w), std::min((LONG)(y1 * msy), (LONG)dr->h) };
            g_list->ClearDepthStencilView(Dsv(dr->dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z, (UINT8)(v & 0xFF), 1, &r);
            static int logged = 0;
            if (logged < 6) { logged++; LOG("[edram] clear de profundidad %08X -> z=%g stencil=%u rect=(%d,%d)-(%d,%d)", v, z, v & 0xFF, x0, y0, x1, y1); }
        }
    }
}

// Vuelca a la memoria del juego una textura resuelta por GPU (como el resolve por CPU: tiled 8888 con swapRB y endian del destino)
static void WriteBackResolved(ResolvedTex& r)
{
    if (!r.res || r.inMemory || !g_readback || !r.destPitch) return;
    FlushPendingResolves();
    if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &b); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
    UINT rowPitch = (r.w * 4 + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { r.fmt, r.w, r.h, 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION sl{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    CheckedCopy(&dst, 0, 0, 0, &sl, nullptr);
    FlushAndWait();
    uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
    if (!p) return;
    uint8_t* out = g_base + (0xA0000000u | (r.destBase & 0x1FFFFFFF));
    uint32_t pitchAligned = (r.destPitch + 31) & ~31u, maxH = r.destH ? r.destH : 0x3FFF;
    if (r.texFmt == 2 || r.texFmt == 36 || r.texFmt == 25)
    {   // sesion 24: k_8 = canal rojo (byte 2 de BGRA) con tiling de 1 byte; k_32_FLOAT = palabra con el endian del destino
        for (uint32_t y = 0; y < r.h && y < maxH; y++)
        {
            uint32_t outer = TiledOuter(y, pitchAligned, r.texFmt == 2 ? 0 : 2);
            for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
            {
                const uint8_t* px = p + y * rowPitch + x * 4;
                if (r.texFmt == 2) *(out + TiledInner(x, y, 0, outer)) = px[2];
                else if (r.fmt == DXGI_FORMAT_R16G16_FLOAT)
                {   // 16_16 sin recortar (sesion 24): en memoria, entero de 16 bits con 0x7FFF = 32 (como Xenia)
                    uint32_t wv = 0;
                    for (int k = 0; k < 2; k++)
                    {
                        float fv = HalfToFloat(((const uint16_t*)px)[k]);
                        int iv = (int)std::lround(std::clamp(fv / 32.0f, -1.0f, 1.0f) * 32767.0f);
                        wv |= uint32_t(uint16_t(int16_t(iv))) << (16 * k);
                    }
                    *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = Swap32(wv, r.endian);
                }
                else *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = Swap32(*(const uint32_t*)px, r.endian);   // 32_FLOAT y 16_16 (R | G << 16)
            }
        }
        g_readback->Unmap(0, nullptr);
        r.inMemory = true;
        g_cpuResolveCounter++;
        g_writeBacks++;
        return;
    }
    for (uint32_t y = 0; y < r.h && y < maxH; y++)
    {
        uint32_t outer = TiledOuter(y, pitchAligned, 2);
        for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
        {
            const uint8_t* px = p + y * rowPitch + x * 4;
            uint32_t v = r.swapRB ? (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24)) : (px[2] | (px[1] << 8) | (px[0] << 16) | (px[3] << 24));
            *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = r.endian <= 3 ? Swap32(v, r.endian) : v;
        }
    }
    g_readback->Unmap(0, nullptr);
    r.inMemory = true;
    g_writeBacks++;
    g_cpuResolveCounter++;   // la memoria cambio: invalida las huellas de textura memorizadas en este frame
    static int n = 0;
    if (n++ < 20) LOG("[d3d12] textura resuelta %08X (%ux%u) volcada a memoria para una lectura con otro formato/tamano", r.destBase, r.w, r.h);
}
static long long DrawLogFrame();
static long long DrawLogFrameExt() { return DrawLogFrame(); }
// Diagnostico (sesion 23): guarda una textura resuelta por GPU como BMP (BGRA tal cual, con canal alfa aparte en otro BMP)
static void DumpResolvedTex(ResolvedTex& r, const char* tag)
{
    if (!r.res || !g_readback) return;
    if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &b); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
    UINT rowPitch = (r.w * 4 + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { r.fmt, r.w, r.h, 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION sl{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    CheckedCopy(&dst, 0, 0, 0, &sl, nullptr);
    FlushAndWait();
    uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
    if (!p) return;
    for (int pass = 0; pass < 2; pass++)
    {
        char name[96]; snprintf(name, sizeof(name), "res_%s_%08X%s.bmp", tag, r.destBase, pass ? "_alfa" : "");
        if (FILE* f = fopen(name, "wb"))
        {
            uint32_t size = r.w * r.h * 4;
            uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = (int32_t)r.w; *(int32_t*)(hdr + 22) = -(int32_t)r.h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
            fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> row(r.w * 4);
            for (UINT y = 0; y < r.h; y++)
            {
                const uint8_t* s = p + y * rowPitch;
                for (UINT x = 0; x < r.w; x++) { uint8_t a = s[x * 4 + 3]; if (pass) { row[x*4] = row[x*4+1] = row[x*4+2] = a; } else { row[x*4] = s[x*4]; row[x*4+1] = s[x*4+1]; row[x*4+2] = s[x*4+2]; } row[x*4+3] = 255; }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
    }
    g_readback->Unmap(0, nullptr);
}

// Antes de preparar un draw: las texturas que el camino rapido no puede servir desde la GPU se vuelcan a memoria
static void WriteBackResolvedForDraw(uint32_t mask)
{
    for (uint32_t t = 0; t < 16; t++)
    {
        if (!(mask & (1u << t))) continue;
        uint32_t fc0 = Reg(0x4800 + t * 6), fc1 = Reg(0x4800 + t * 6 + 1), fc2 = Reg(0x4800 + t * 6 + 2), fc5 = Reg(0x4800 + t * 6 + 5);
        if ((fc0 & 3) != 2) continue;
        uint32_t base = fc1 & 0xFFFFF000, fmt = fc1 & 0x3F, dim = (fc5 >> 9) & 3;
        auto last = g_resolvedLast.find(base);
        if (last == g_resolvedLast.end()) continue;
        auto it = g_resolved.find(last->second);
        if (it == g_resolved.end() || it->second.inMemory) continue;
        ResolvedTex& r = it->second;
        uint32_t w = (fc2 & 0x1FFF) + 1, h = ((fc2 >> 13) & 0x1FFF) + 1;
        {
            // GH6_DUMPRES=<direccion hex>: en el frame de GH6_DRAWLOG, vuelca la copia por GPU que leera este draw
            static uint32_t dumpAddr = getenv("GH6_DUMPRES") ? (uint32_t)strtoul(getenv("GH6_DUMPRES"), nullptr, 16) : 0;
            static int dumps = 0;
            if (dumpAddr == base && dumps < 12 && (long long)g_frameCounter == DrawLogFrameExt())
            {
                char tag[32]; snprintf(tag, sizeof(tag), "n%02d_g%llu", dumps++, (unsigned long long)r.gen);
                DumpResolvedTex(r, tag);
            }
        }
        bool gpuOk = dim == 1 && ((fmt == 6 && r.texFmt == 6 && w <= r.w + 8 && h <= r.h + 8) ||     // directo o region derivada
                                  ((fmt == 2 || fmt == 36 || (fmt == 25 && ((fc0 >> 2) & 0x55) == 0 && ((Reg(0x4800 + t * 6 + 3) >> 13) & 0x3F) == 0)) &&
                                   r.texFmt == fmt && w <= r.w + 8 && h <= r.h + 8)); // sesion 24 (directo o region derivada)
        if (!gpuOk)
        {
            static std::unordered_map<uint64_t, int> seenWb;
            uint64_t wk = (uint64_t(base) << 16) ^ (uint64_t(fmt) << 8) ^ dim ^ (uint64_t(w) << 40) ^ (uint64_t(r.texFmt) << 2);
            if (!r.inMemory && seenWb.size() < 40 && seenWb[wk]++ == 0)
                LOG("[writeback] %08X leida como fmt %u dim %u %ux%u; resuelta como texFmt %u %ux%u (PS %016llX)", base, fmt, dim, w, h, r.texFmt, r.w, r.h, (unsigned long long)g_curPsHash);
            WriteBackResolved(r);
        }
    }
}

// GH6_DRAWLOG=<frame> o t<segundos> (primer frame tras ese tiempo desde el arranque)
static long long g_forceLogFrame = -1;   // sesion 24: frame forzado por GH6_DUMPVS
static long long DrawLogFrame()
{
    if (g_forceLogFrame >= 0 && (long long)g_frameCounter == g_forceLogFrame) return g_forceLogFrame;
    static const char* s = getenv("GH6_DRAWLOG");
    static long long frame = (s && s[0] != 't' && s[0] != 's') ? atoll(s) : -1;
    static DWORD t0 = GetTickCount();
    if (s && s[0] == 't' && frame < 0 && GetTickCount() - t0 >= (DWORD)(atof(s + 1) * 1000)) frame = (long long)g_frameCounter + 1;
    // GH6_DRAWLOG=s<RB_SURFACE_INFO hex>: el frame siguiente al primero que dibuja en esa superficie (sesion 23)
    if (s && s[0] == 's' && frame < 0 && Reg(0x2000) == (uint32_t)strtoul(s + 1, nullptr, 16)) frame = (long long)g_frameCounter + 1;
    // GH6_DRAWLOGN=n: registra n frames seguidos (el juego reparte escena y postproceso en dos swaps) (sesion 24)
    static long long n = getenv("GH6_DRAWLOGN") ? atoll(getenv("GH6_DRAWLOGN")) : 1;
    // Sesion 24: F9 (una comprobacion por frame) registra los 2 frames siguientes (los dos tipos de frame del juego)
    {
        static uint64_t lastCheck = ~0ull; static bool wasDown = false;
        if (lastCheck != g_frameCounter)
        {
            lastCheck = g_frameCounter;
            bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (down && !wasDown && (frame < 0 || (long long)g_frameCounter >= frame + n))
            { frame = (long long)g_frameCounter + 1; n = 2; LOG("[drawlog] F9: registrando los frames %lld y %lld", frame, frame + 1); }
            wasDown = down;
            // GH6_DRAWLOGEVERY=<desde s>:<cada s>: registra 2 frames y guarda shot_<s>.bmp periodicamente (diagnostico, sesion 24)
            static const char* ev = getenv("GH6_DRAWLOGEVERY");
            if (ev)
            {
                static double from = atof(ev), every = strchr(ev, ':') ? atof(strchr(ev, ':') + 1) : 5.0;
                static DWORD start = GetTickCount(); static double next = from;
                double el = (GetTickCount() - start) / 1000.0;
                if (el >= next && (frame < 0 || (long long)g_frameCounter >= frame + n))
                {
                    next += every; frame = (long long)g_frameCounter + 1; n = 2;
                    extern std::atomic<int> g_dumpRequest; g_dumpRequest = (int)el;
                    LOG("[drawlog] periodico t=%.1f: frames %lld y %lld (shot_%03d)", el, frame, frame + 1, (int)el);
                }
            }
        }
    }
    if (frame >= 0 && (long long)g_frameCounter > frame && (long long)g_frameCounter < frame + n) return (long long)g_frameCounter;
    return frame;
}
static void DumpEdram(UINT64 frame);static void D3D12DrawImpl(const XenosDraw& d);
// Envoltorio (sesion 21): en el frame de GH6_DRAWLOG indica para cada draw si se dibujo o por que se omitio
void D3D12Draw(const XenosDraw& d)
{
    struct DrawTimer { double t0 = QpcMs(); ~DrawTimer() { g_hDrawMs += QpcMs() - t0; } } dtimer;
    g_curPsHash = d.psHash;
    {   // GH6_PSCONSTLOG=<hash PS>[,<hash>...]: constantes PS c0-c15, mezcla y mascara de color de los draws con ese PS (diagnostico)
        static std::vector<uint64_t> hs = [] { std::vector<uint64_t> v; const char* s = getenv("GH6_PSCONSTLOG"); while (s && *s) { v.push_back(strtoull(s, nullptr, 16)); s = strchr(s, ','); if (s) s++; } return v; }();
        static int logged = 0;
        if (!hs.empty() && logged < 60 && std::find(hs.begin(), hs.end(), d.psHash) != hs.end())
        {
            logged++;
            std::string cs;
            static const int extra[] = { 20, 110, 111, 152, 153 };
            for (int ci = 0; ci < 16 + 5; ci++)
            {
                int c = ci < 16 ? ci : extra[ci - 16];
                char b[96]; float f[4];
                for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4400 + c * 4 + k); memcpy(&f[k], &u, 4); }
                snprintf(b, sizeof(b), " c%d=(%.4g %.4g %.4g %.4g)", c, f[0], f[1], f[2], f[3]); cs += b;
            }
            for (int c = 0; c < 4; c++)
            {
                char b[96]; float f[4];
                for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + c * 4 + k); memcpy(&f[k], &u, 4); }
                snprintf(b, sizeof(b), " vc%d=(%.4g %.4g %.4g %.4g)", c, f[0], f[1], f[2], f[3]); cs += b;
            }
            LOG("[psconst] PS %016llX VS %016llX frame %llu blend0=%08X mask=%08X color0=%08X depthctl=%08X |%s", (unsigned long long)d.psHash, (unsigned long long)d.vsHash,
                (unsigned long long)g_frameCounter, Reg(0x2201), Reg(0x2104), Reg(0x2001), Reg(0x2200), cs.c_str());
        }
    }
    {   // GH6_DUMPVS: la primera vez que aparece ese VS se registra el frame en curso desde ese draw (sesion 24)
        static uint64_t vsTrig = getenv("GH6_DUMPVS") ? strtoull(getenv("GH6_DUMPVS"), nullptr, 16) : 0;
        static bool trig = false;
        static DWORD vsT0 = GetTickCount(); static double vsMin = getenv("GH6_DUMPVSMIN") ? atof(getenv("GH6_DUMPVSMIN")) : 0;
        if (vsTrig && !trig && d.vsHash == vsTrig && (GetTickCount() - vsT0) / 1000.0 >= vsMin) { trig = true; g_forceLogFrame = (long long)g_frameCounter; LOG("[drawlog] GH6_DUMPVS: frame %lld", g_forceLogFrame); }
    }
    long long logFrame = DrawLogFrame();
    if (logFrame < 0 || g_failed || !g_dev) { D3D12DrawImpl(d); return; }
    OpenFrame();
    if ((long long)g_frameCounter != logFrame) { D3D12DrawImpl(d); return; }
    uint64_t before[8]; memcpy(before, g_skipped, sizeof(before));
    uint64_t drawsBefore = g_frameDraws;
    D3D12DrawImpl(d);
    static const char* names[8] = { "prim", "shader", "pso", "mem", "mascara0", "descriptores", "?", "?" };
    std::string why;
    for (int i = 0; i < 8; i++) if (g_skipped[i] != before[i]) why += names[i];
    LOG("[drawres] %s", g_frameDraws != drawsBefore ? "dibujado" : why.empty() ? "omitido (otro motivo)" : ("omitido: " + why).c_str());
}
static void D3D12DrawImpl(const XenosDraw& d)
{
    if (g_failed || !g_dev) return;
    OpenFrame();
    long long logFrame = DrawLogFrame();
    if ((long long)g_frameCounter == logFrame)
    {
        uint32_t tf0[6]; for (int i = 0; i < 6; i++) tf0[i] = Reg(0x4800 + i);
        LOG("[drawst] WSC=%08X/%08X SSC=%08X/%08X CC=%08X AREF=%g WOFF=%08X VPX=%g/%g VPY=%g/%g DC=%08X CMASK=%X VTE=%08X SC=%08X ZSCALE=%g ZOFF=%g tf1=%u@%08X tf2=%u@%08X tf3=%u@%08X", Reg(0x2081), Reg(0x2082), Reg(0x200E), Reg(0x200F), Reg(0x2202), RegF(0x210E), Reg(0x2080), RegF(0x210F), RegF(0x2110), RegF(0x2111), RegF(0x2112), Reg(0x2200), Reg(0x2104) & 0xF, Reg(0x2206), Reg(0x2205), RegF(0x2113), RegF(0x2114),
            Reg(0x4800 + 6 + 1) & 0x3F, Reg(0x4800 + 6 + 1) & 0xFFFFF000, Reg(0x4800 + 12 + 1) & 0x3F, Reg(0x4800 + 12 + 1) & 0xFFFFF000, Reg(0x4800 + 18 + 1) & 0x3F, Reg(0x4800 + 18 + 1) & 0xFFFFF000);
        LOG("[draw] f%llu prim=%u n=%u src=%u VS=%016llX PS=%016llX SURFACE=%08X COLOR0=%08X DEPTH=%08X MODE=%08X COPY_CTRL=%08X COPY_DEST=%08X/%08X/%08X BLEND=%08X tf0fmt=%u tf0base=%08X",
            (unsigned long long)g_frameCounter, d.primType, d.numIndices, d.source, (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
            Reg(0x2000), Reg(0x2001), Reg(0x2002), Reg(0x2208), Reg(0x2318), Reg(0x2319), Reg(0x231A), Reg(0x231B), Reg(0x2201), tf0[1] & 0x3F, tf0[1] & 0xFFFFF000);
    }
    // GH6_DUMPDRAWS=a,b,c (con GH6_DRAWLOG=<frame>): vuelca las superficies EDRAM antes del draw numero a, b, c de ese frame
    {
        static std::vector<int> dumpDraws = [] {
            std::vector<int> v; const char* s = getenv("GH6_DUMPDRAWS");
            while (s && *s) { v.push_back(atoi(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        static int drawIndex = 0;
        // GH6_DUMPTEXDRAW=<hex>: vuelca antes del primer draw que lee esa textura en tf0 y antes del siguiente (sesion 24)
        static uint32_t dumpTex = getenv("GH6_DUMPTEXDRAW") ? (uint32_t)strtoul(getenv("GH6_DUMPTEXDRAW"), nullptr, 16) : 0;
        static int dumpNext = 0, texHit = 0;
        // GH6_DUMPREL=a,b: ademas, vuelca a, b draws despues del de GH6_DUMPTEXDRAW
        static std::vector<int> dumpRel = [] {
            std::vector<int> v; const char* s = getenv("GH6_DUMPREL");
            while (s && *s) { v.push_back(atoi(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        if ((long long)g_frameCounter == logFrame)
        {
            drawIndex++;
            bool hit = std::find(dumpDraws.begin(), dumpDraws.end(), drawIndex) != dumpDraws.end();
            if (texHit && std::find(dumpRel.begin(), dumpRel.end(), drawIndex - texHit) != dumpRel.end()) hit = true;
            if (dumpNext == 1) { hit = true; dumpNext = 2; }
            static uint64_t dumpPs = getenv("GH6_DUMPPS") ? strtoull(getenv("GH6_DUMPPS"), nullptr, 16) : 0;   // filtro opcional por PS
            if (dumpTex && !dumpNext && (Reg(0x4800 + 1) & 0xFFFFF000) == dumpTex && (!dumpPs || d.psHash == dumpPs) && (Reg(0x2208) & 7) == 4) { hit = true; dumpNext = 1; texHit = drawIndex; }
            // GH6_DUMPVS=<hash>: igual, disparado por el primer draw con ese VS (GH6_DUMPREL = draws siguientes a volcar) (sesion 24)
            static uint64_t dumpVs = getenv("GH6_DUMPVS") ? strtoull(getenv("GH6_DUMPVS"), nullptr, 16) : 0;
            if (dumpVs && !texHit && d.vsHash == dumpVs) { hit = true; dumpNext = 2; texHit = drawIndex; }
            if (hit) { FlushAndWait(); DumpEdram(100000 + drawIndex); }
        }
    }
    // RB_MODECONTROL (0x2208) & 7 == 6: "copy" = resolve de EDRAM a memoria (pendiente de emular), no es un draw visible
    if ((Reg(0x2208) & 7) == 6) { Resolve(); return; }
    // Topologia
    D3D12_PRIMITIVE_TOPOLOGY topo; D3D12_PRIMITIVE_TOPOLOGY_TYPE ttype; bool rect = false, expand = false;
    switch (d.primType)
    {
    case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 3: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 8: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; rect = true; break;
    case 5: case 13: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; expand = true; break;
    default:
    {
        static int logged = 0;
        if (logged < 8) { logged++; LOG("[d3d12] draw omitido: prim %u, %u indices, origen %u, VS %016llX, PS %016llX, PA_SU_POINT_SIZE=%08X MINMAX=%08X",
            d.primType, d.numIndices, d.source, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, Reg(0x2280), Reg(0x2281)); }
        g_skipped[0]++; return; // puntos y otros: pendiente
    }
    }
    if (rect && !g_rectGs) return;
    // Diagnostico (sesion 24): GH6_SKIPFSQUAD=<tex hex> omite los quads de la interfaz a pantalla completa que leen esa textura en tf0
    if (static const char* sq = getenv("GH6_SKIPFSQUAD"); sq && d.vsHash == 0x56A07B413E52351Aull && (Reg(0x4800 + 1) & 0xFFFFF000) == (uint32_t)strtoul(sq, nullptr, 16))
    {
        uint32_t a = 0xA0000000u | (Reg(0x4800 + 95 * 2) & 0x1FFFFFFC);
        float x2, y2; uint32_t ux = __builtin_bswap32(*(uint32_t*)(g_base + a + 2 * 28)), uy = __builtin_bswap32(*(uint32_t*)(g_base + a + 2 * 28 + 4));
        memcpy(&x2, &ux, 4); memcpy(&y2, &uy, 4);
        if (x2 >= 1279 && y2 >= 719) return;
    }
    ShaderEntry& vs = GetShader(0, d.vsCode, d.vsCount, d.vsHash);
    ShaderEntry& ps = GetShader(1, d.psCode, d.psCount, d.psHash);
    if (!vs.ok || !ps.ok) { g_skipped[1]++; return; }
    // Texturas resueltas por GPU que este draw leera con otro formato o tamano: se vuelcan antes a la memoria del juego (sesion 23)
    WriteBackResolvedForDraw(ps.info.textureMask | ps.info.texture3DMask | ps.info.textureCubeMask);

    // Blend (RB_BLENDCONTROL0 = 0x2201)
    uint32_t bc = Reg(0x2201);
    // RB_COLOR_MASK (0x2104), bits 0-3 = R,G,B,A del RT0 (sesion 16). Mascara 0 = pasada solo de profundidad/stencil:
    // como aun no hay depth buffer, se omite (antes pintaba la pantalla de blanco).
    uint32_t colorMask = Reg(0x2104) & 0xF;
    // RB_MODECONTROL = 5 (kDepth): solo profundidad, sin escribir color (sesion 23). Antes pintaba de negro encima de la imagen
    // (retratos del lobby y desenfoque de profundidad de las canciones).
    if ((Reg(0x2208) & 7) == 5) colorMask = 0;
    // Sesion 19: profundidad. RB_DEPTHCONTROL (0x2200): bit0 stencil, bit1 z, bit2 escritura z, bits 4-6 funcion z,
    // bits 8-19 stencil cara frontal (func, fail, zpass, zfail), bit7 + bits 20-31 cara trasera.
    static bool noDepth = getenv("GH6_NODEPTH") != nullptr;
    uint32_t dc = noDepth ? 0 : Reg(0x2200);
    // PS que exporta profundidad con z desactivado (p. ej. la reduccion de profundidad de la profundidad de campo): el juego cuenta con
    // que se escriba en su superficie de profundidad, que copia justo despues. Se escribe con ALWAYS (sesion 23).
    // Sesion 24: activado por defecto (GH6_NOPSDEPTH=1 lo quita). Sin esto la profundidad reducida (320x180) que usan los draws de
    // velocidad (desenfoque de movimiento) quedaba vacia: no se escribian velocidades y el compositor final mezclaba los personajes
    // con una version borrosa (personajes y publico "transparentes", estelas, lavado verde en las canciones).
    static bool psDepthWrite = getenv("GH6_NOPSDEPTH") == nullptr;
    if (psDepthWrite && !noDepth && ps.info.writesDepth && !(dc & 2)) dc = (dc & ~0x7Eu) | 0x76u;
    {   // GH6_NOZFORPS=<hash PS> (diagnostico): ese PS sin prueba ni escritura de profundidad
        static const uint64_t noZPs = getenv("GH6_NOZFORPS") ? strtoull(getenv("GH6_NOZFORPS"), nullptr, 16) : 0;
        if (noZPs && d.psHash == noZPs) dc &= ~0x6u;
    }
    bool useDepth = (dc & 3) != 0;
    if (!colorMask && !useDepth) { g_skipped[4]++; return; }
    // PA_SU_SC_MODE_CNTL (0x2205): bit0 cull frontal, bit1 cull trasero, bit2 cara frontal = horaria
    static bool noCull = getenv("GH6_NOCULL") != nullptr;
    uint32_t sc = (noCull || rect || expand) ? 0 : Reg(0x2205) & 7;
    EdramRt* curRt = GetEdramRt();
    if (!curRt) return;
    // Sesion 24: MRT. Si el PS escribe oC1 y RB_COLOR_MASK (bits 4-7) lo permite, se enlaza la superficie de RB_COLOR1_INFO como RT1
    // (el juego restaura asi el color de la escena del frame anterior a la vez que la profundidad, antes de sumar las luces).
    static bool noMrt = getenv("GH6_NOMRT") != nullptr;
    EdramRt* rt1 = nullptr; uint32_t mask1 = 0, bc1 = 0;
    if (!noMrt && (ps.info.mrtMask & 2) && (Reg(0x2208) & 7) != 5)
    {
        mask1 = (Reg(0x2104) >> 4) & 0xF;
        if (mask1) { rt1 = GetEdramRt(0x2003); bc1 = Reg(0x2209); }   // RB_BLENDCONTROL1
        if (rt1 == curRt || (rt1 && rt1->w != curRt->w)) rt1 = nullptr;
        static int mrtLogged = 0;
        if (mrtLogged < 8) { mrtLogged++; LOG("[d3d12] MRT: PS %016llX RT1 info %08X mascara %X blend %08X -> %s", (unsigned long long)d.psHash, Reg(0x2003), mask1, bc1, rt1 ? "enlazado" : "no"); }
    }
    uint64_t key = d.vsHash * 31 ^ d.psHash ^ (uint64_t(d.primType) << 56) ^ (uint64_t(bc) << 8) ^ (uint64_t(colorMask) << 48)
                 ^ ((uint64_t(useDepth ? dc : 0) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(sc) << 60)) ^ (uint64_t(curRt->fmt) * 0xC2B2AE3D27D4EB4Full)
                 ^ (rt1 ? ((uint64_t(rt1->fmt) + 1) * 0x165667B19E3779F9ull ^ (uint64_t(mask1) << 36) ^ (uint64_t(bc1) * 0x27D4EB2F165667C5ull)) : 0);
    auto& pso = g_psos[key];
    if (!pso)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_drawRootSig.Get();
        pd.VS = { vs.blob->GetBufferPointer(), vs.blob->GetBufferSize() };
        pd.PS = { ps.blob->GetBufferPointer(), ps.blob->GetBufferSize() };
        if (rect) pd.GS = { g_rectGs->GetBufferPointer(), g_rectGs->GetBufferSize() };
        auto& rt = pd.BlendState.RenderTarget[0];
        uint32_t cs = bc & 0x1F, cop = (bc >> 5) & 7, cd = (bc >> 8) & 0x1F, as = (bc >> 16) & 0x1F, aop = (bc >> 21) & 7, ad = (bc >> 24) & 0x1F;
        rt.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
        rt.SrcBlend = MapBlend(cs); rt.DestBlend = MapBlend(cd); rt.BlendOp = MapOp(cop);
        rt.SrcBlendAlpha = MapBlendAlpha(as); rt.DestBlendAlpha = MapBlendAlpha(ad); rt.BlendOpAlpha = MapOp(aop);
        rt.RenderTargetWriteMask = (UINT8)colorMask;
        if (rt1)
        {
            pd.BlendState.IndependentBlendEnable = TRUE;
            auto& r1 = pd.BlendState.RenderTarget[1];
            uint32_t cs1 = bc1 & 0x1F, cop1 = (bc1 >> 5) & 7, cd1 = (bc1 >> 8) & 0x1F, as1 = (bc1 >> 16) & 0x1F, aop1 = (bc1 >> 21) & 7, ad1 = (bc1 >> 24) & 0x1F;
            r1.BlendEnable = !(cs1 == 1 && cd1 == 0 && cop1 == 0 && as1 == 1 && ad1 == 0 && aop1 == 0);
            r1.SrcBlend = MapBlend(cs1); r1.DestBlend = MapBlend(cd1); r1.BlendOp = MapOp(cop1);
            r1.SrcBlendAlpha = MapBlendAlpha(as1); r1.DestBlendAlpha = MapBlendAlpha(ad1); r1.BlendOpAlpha = MapOp(aop1);
            r1.RenderTargetWriteMask = (UINT8)mask1;
        }
        pd.SampleMask = 0xFFFFFFFF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.DepthClipEnable = FALSE;
        pd.RasterizerState.CullMode = (sc & 3) == 1 ? D3D12_CULL_MODE_FRONT : (sc & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        pd.RasterizerState.FrontCounterClockwise = (sc & 4) == 0;
        if (useDepth)
        {
            auto& ds = pd.DepthStencilState;
            ds.DepthEnable = (dc >> 1) & 1;
            ds.DepthWriteMask = ((dc >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            ds.DepthFunc = MapCmp(dc >> 4);
            {   // GH6_EQBIAS=<n> (diagnostico): segundas pasadas (z sin escritura, LEQUAL/EQUAL) adelantadas n unidades de profundidad
                static const int eqBias = getenv("GH6_EQBIAS") ? atoi(getenv("GH6_EQBIAS")) : 0;
                uint32_t zf = (dc >> 4) & 7;
                if (eqBias && ((dc >> 1) & 1) && !((dc >> 2) & 1) && (zf == 2 || zf == 3))
                {
                    static const float eqSlope = getenv("GH6_EQSLOPE") ? (float)atof(getenv("GH6_EQSLOPE")) : 0.0f;
                    pd.RasterizerState.DepthBias = -eqBias; pd.RasterizerState.SlopeScaledDepthBias = -eqSlope;
                    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                }
            }
            ds.StencilEnable = dc & 1;
            uint32_t srm = Reg(0x210C);
            ds.StencilReadMask = (UINT8)(srm >> 8); ds.StencilWriteMask = (UINT8)(srm >> 16);
            ds.FrontFace = { MapStencilOp(dc >> 11), MapStencilOp(dc >> 17), MapStencilOp(dc >> 14), MapCmp(dc >> 8) };
            ds.BackFace = ((dc >> 7) & 1) ? D3D12_DEPTH_STENCILOP_DESC{ MapStencilOp(dc >> 23), MapStencilOp(dc >> 29), MapStencilOp(dc >> 26), MapCmp(dc >> 20) } : ds.FrontFace;
            pd.DSVFormat = kDepthFmt;
        }
        pd.PrimitiveTopologyType = ttype;
        pd.NumRenderTargets = rt1 ? 2 : 1; pd.RTVFormats[0] = curRt->fmt; pd.SampleDesc.Count = 1;
        if (rt1) pd.RTVFormats[1] = rt1->fmt;
        double tPso = QpcMs(); HRESULT hrPso = CreatePsoCached(pd, key, pso); g_hPsoMs += QpcMs() - tPso; g_hPsos++;
        if (FAILED(hrPso)) { LOG("[d3d12] PSO fallido (prim %u, formato RT %d)", d.primType, (int)curRt->fmt); g_skipped[2]++; return; }
    }

    // Constantes VS (C en 0x4000) y PS (C en 0x4400) + bools + fetch + viewport
    auto buildCb = [&](uint32_t cbase, const TranslatedShader* fetchInfo, uint64_t& outOffset) -> uint8_t* {
        constexpr UINT64 kCbSize = 4864;   // sesion 24: + XLoop (32 constantes de bucle en 4320) + XTexMode[16] en 4448
        outOffset = RingAlloc(kCbSize);
        if (outOffset == ~0ull) return nullptr;
        uint8_t* p = g_ringMap[g_fi] + outOffset;
        uint32_t* f = (uint32_t*)p;
        for (uint32_t i = 0; i < 1024; i++) f[i] = Reg(cbase + i);
        uint32_t* b = (uint32_t*)(p + 4096);
        for (uint32_t i = 0; i < 8; i++) b[i] = Reg(0x4900 + i);
        uint32_t* fe = (uint32_t*)(p + 4128);
        memset(fe, 0, 128);
        float* v = (float*)(p + 4256);
        uint32_t rtw = Reg(0x2000) & 0x3FFF; if (!rtw) rtw = kRtW;
        v[0] = (float)Reg(0x2206); v[1] = (float)rtw; v[2] = (float)kEdramH; v[3] = curRt->outScale;
        {   // GH6_PSOUTAT=<s> (diagnostico): las sustituciones de GH6_PSOUT solo actuan desde ese segundo (XVte.z < 0 en el PS)
            static const double psoutAt = getenv("GH6_PSOUTAT") ? atof(getenv("GH6_PSOUTAT")) : -1;
            static const double tStart = QpcMs();
            if (psoutAt >= 0 && cbase == 0x4400 && (QpcMs() - tStart) >= psoutAt * 1000) v[2] = -1.0f;
        }
        v[4] = RegF(0x210F); v[5] = RegF(0x2110); v[6] = RegF(0x2111); v[7] = RegF(0x2112);
        v[8] = RegF(0x2113); v[9] = RegF(0x2114);
        { int wox, woy; WindowOffset(wox, woy); v[10] = (float)wox; v[11] = (float)woy; }   // PA_SC_WINDOW_OFFSET (sesion 23)
        {   // Sesion 24: PA_SU_VTX_CNTL.pix_center = 0 (convencion D3D9, centro del pixel en el entero): +0.5 en pantalla, como Xenia.
            // Sin esto cada pase a pantalla completa se corria medio pixel; en la cadena de desenfoque a 320x180 el error se
            // acumulaba y la capa borrosa quedaba desplazada de la nitida (personajes duplicados y bordes en escalera).
            static const bool noHalfPix = getenv("GH6_NOHALFPIXEL") != nullptr;
            static const double halfAt = getenv("GH6_HALFPIXAT") ? atof(getenv("GH6_HALFPIXAT")) : -1;   // diagnostico: activar desde el segundo N
            static const double hStart = QpcMs();
            bool on = !noHalfPix && (halfAt < 0 || QpcMs() - hStart >= halfAt * 1000);
            if (on && (Reg(0x2302) & 1) == 0) { v[10] += 0.5f; v[11] += 0.5f; }
        }
        // Prueba de alfa: RB_COLORCONTROL (0x2202) bits 0-2 funcion, bit 3 activada; RB_ALPHA_REF (0x210E) (sesion 24)
        { static bool noAt = getenv("GH6_NOALPHATEST") != nullptr; uint32_t cc = Reg(0x2202); v[12] = (!noAt && (cc & 8)) ? (float)(cc & 7) : 8.0f; v[13] = RegF(0x210E); v[14] = v[15] = 0; }
        {   // PsParamGen (sesion 24): SQ_PROGRAM_CNTL (0x2180) bit 18, SQ_CONTEXT_MISC (0x2181) bits 8-15 = registro destino
            static bool noPgen = getenv("GH6_NOPARAMGEN") != nullptr;
            uint32_t pg = (Reg(0x2181) >> 8) & 0xFF;
            v[14] = (!noPgen && ((Reg(0x2180) >> 18) & 1) && pg < 16) ? (float)pg : -1.0f;
            uint32_t pmx, pmy; MsaaScale(pmx, pmy); v[15] = (float)(pmx * 8 + pmy);
        }
        { uint32_t* lp = (uint32_t*)(p + 4320); for (uint32_t i = 0; i < 32; i++) lp[i] = Reg(0x4908 + i); }
        {   // XTexMode[16] (sesion 24): x = clamp_x | clamp_y << 3 | clamp_z << 6, y = filtro de ampliacion, z = de reduccion, w = color de borde
            static bool noTexMode = getenv("GH6_NOTEXMODE") != nullptr;
            uint32_t* tm = (uint32_t*)(p + 4448);
            for (uint32_t t = 0; t < 16; t++)
            {
                uint32_t f0 = Reg(0x4800 + t * 6), f3 = Reg(0x4800 + t * 6 + 3), f5 = Reg(0x4800 + t * 6 + 5);
                tm[t * 4 + 0] = noTexMode ? (2u | (2u << 3) | (2u << 6)) : (f0 >> 10) & 0x1FF;
                tm[t * 4 + 1] = noTexMode ? 1u : (f3 >> 19) & 3;
                tm[t * 4 + 2] = noTexMode ? 1u : (((f3 >> 21) & 3) | (((f3 >> 23) & 3) << 2));
                tm[t * 4 + 3] = f5 & 3;
            }
        }   // constantes de bucle (sesion 24)
        return p;
    };
    uint64_t vsCbOff, psCbOff;
    uint8_t* vsCb = buildCb(0x4000, &vs.info, vsCbOff);
    if (!vsCb || !buildCb(0x4400, nullptr, psCbOff)) { g_skipped[3]++; return; }
    // Streams de vertices: copiar desde la memoria fisica de invitado al buffer de subida
    uint32_t* fe = (uint32_t*)(vsCb + 4128);
    for (size_t k = 0; k < vs.info.vertexFetchSlots.size(); k++)
    {
        uint32_t vf = vs.info.vertexFetchSlots[k];
        uint32_t dw0 = Reg(0x4800 + vf * 2), dw1 = Reg(0x4800 + vf * 2 + 1);
        uint32_t addr = dw0 & 0x1FFFFFFC, bytes = ((dw1 >> 2) & 0xFFFFFF) * 4;
        if (bytes > (8u << 20)) bytes = 8u << 20;
        // Sesion 16: el mismo vertex buffer se usa en muchos draws del frame; se copia una sola vez por frame
        // (antes se agotaban los 64 MB del anillo y se omitian miles de draws).
        uint64_t key = (uint64_t(addr) << 32) | bytes;
        UINT64 o;
        // Sesion 24: el juego reutiliza las mismas direcciones (anillo de vertices de la interfaz) con otro contenido
        // dentro del frame: la copia solo vale si la huella del contenido coincide (antes el draw recibia los vertices
        // de un quad anterior a pantalla completa y tapaba los retratos del lobby).
        if (!g_pendingResolvesEmpty()) FlushResolvesIfOverlap(addr, addr + std::max(bytes, 4u));
        const uint8_t* src = g_base + (0xA0000000u | addr);
        uint64_t h = 1469598103934665603ull;
        {
            const uint32_t* w = (const uint32_t*)src; uint32_t nw = bytes / 4;
            uint32_t step = nw <= 1024 ? 1 : nw / 1024;
            for (uint32_t i = 0; i < nw; i += step) h = (h ^ w[i]) * 1099511628211ull;
            if (nw) h = (h ^ w[nw - 1]) * 1099511628211ull;
        }
        static bool noCheck = getenv("GH6_VBNOCHECK") != nullptr;
        auto it = g_vbCache.find(key);
        if (it != g_vbCache.end() && (noCheck || it->second.hash == h)) o = it->second.off;
        else
        {
            o = RingAlloc(bytes + 16, 16);
            if (o == ~0ull) { g_skipped[3]++; return; }
            memcpy(g_ringMap[g_fi] + o, src, bytes);
            g_vbCache[key] = { o, h };
            g_vbCopyBytes += bytes;
        }
        fe[k * 4 + 0] = (uint32_t)o; fe[k * 4 + 1] = dw1 & 3;
    }
    // Sesion 24: en el frame de GH6_DRAWLOG, vertices crudos del primer stream y constantes C[111]/C[154] (shader de la interfaz)
    if ((long long)g_frameCounter == DrawLogFrame() && !vs.info.vertexFetchSlots.empty())
    {
        uint32_t vf = vs.info.vertexFetchSlots[0];
        uint32_t a = 0xA0000000u | (Reg(0x4800 + vf * 2) & 0x1FFFFFFC);
        for (uint32_t v = 0; v < std::min<uint32_t>(d.numIndices, 4); v++)
        {
            uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = __builtin_bswap32(*(uint32_t*)(g_base + a + v * 28 + i * 4));
            float f[7]; memcpy(f, raw, sizeof(f));
            LOG("[drawvtx] @%08X v%u pos (%g, %g, %g, %g) col %08X uv (%g, %g)", a, v, f[0], f[1], f[2], f[3], raw[4], f[5], f[6]);
        }
        auto pc = [](int n, int k) { return RegF(0x4400 + n * 4 + k); };
        {
            char buf[1200]; int k = 0;
            static const int idx[] = { 0, 1, 2, 3, 4, 5, 6, 9, 10, 20, 111, 152 };
            for (int n : idx) k += snprintf(buf + k, sizeof(buf) - k, " C%d(%g %g %g %g)", n, pc(n,0), pc(n,1), pc(n,2), pc(n,3));
            LOG("[drawps]%s", buf);
        }
        LOG("[drawvtx] C111 (%g %g %g %g) C154 (%g %g %g %g)", RegF(0x4000 + 111 * 4), RegF(0x4000 + 111 * 4 + 1), RegF(0x4000 + 111 * 4 + 2), RegF(0x4000 + 111 * 4 + 3),
            RegF(0x4000 + 154 * 4), RegF(0x4000 + 154 * 4 + 1), RegF(0x4000 + 154 * 4 + 2), RegF(0x4000 + 154 * 4 + 3));
    }
    if (d.vsHash == 0x56A07B413E52351Aull && getenv("GH6_UIDEBUG"))
    {
        static int n = 0;
        static DWORD lastT = 0;
        if (d.numIndices >= 40 && GetTickCount() - lastT > 3000)
        {
            lastT = GetTickCount();
            uint32_t a0 = 0xA0000000u | (Reg(0x4800 + 95 * 2) & 0x1FFFFFFC);
            uint32_t rx = __builtin_bswap32(*(uint32_t*)(g_base + a0)), ry = __builtin_bswap32(*(uint32_t*)(g_base + a0 + 4)); float fx, fy; memcpy(&fx, &rx, 4); memcpy(&fy, &ry, 4);
            LOG("[ui] t=%lu frame %llu: quads n=%u primer vertice (%g, %g)", GetTickCount(), (unsigned long long)g_frameCounter, d.numIndices, fx, fy);
        }
        static int big = 0;
        if (d.numIndices >= 40 && big++ < 0) {
            uint32_t dw0 = Reg(0x4800 + 95 * 2), dw1 = Reg(0x4800 + 95 * 2 + 1);
            uint32_t a = 0xA0000000u | (dw0 & 0x1FFFFFFC);
            for (uint32_t v = 0; v < std::min<uint32_t>(d.numIndices, 12); v++)
            {
                uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = __builtin_bswap32(*(uint32_t*)(g_base + a + v * 28 + i * 4));
                float px, py; memcpy(&px, &raw[0], 4); memcpy(&py, &raw[1], 4);
                LOG("[ui]   v%u pos (%g, %g) color %08X  (raw %08X %08X)", v, px, py, raw[4], *(uint32_t*)(g_base + a + v * 28), *(uint32_t*)(g_base + a + v * 28 + 16));
            }
            LOG("[ui]   PS=%016llX blend=%08X colormask=%08X tf0=%08X %08X %08X", (unsigned long long)d.psHash, Reg(0x2201), Reg(0x2104), Reg(0x4800), Reg(0x4801), Reg(0x4802));
        }
        if (n++ < 4)
        {
            uint32_t dw0 = Reg(0x4800 + 95 * 2), dw1 = Reg(0x4800 + 95 * 2 + 1);
            uint32_t a = 0xA0000000u | (dw0 & 0x1FFFFFFC);
            auto f = [&](uint32_t r) { uint32_t v = Reg(r); float x; memcpy(&x, &v, 4); return x; };
            uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = Load32(a + i * 4);
            float px, py, pz, pw; memcpy(&px, &raw[0], 4); memcpy(&py, &raw[1], 4); memcpy(&pz, &raw[2], 4); memcpy(&pw, &raw[3], 4);
            LOG("[ui] prim %u n %u vf95 %08X/%08X endian %u v0 = (%g %g %g %g) color %08X uv %08X %08X | VTE %08X c111 (%g %g %g %g) c154 (%g %g %g %g) vport %g %g %g %g",
                d.primType, d.numIndices, dw0, dw1, dw1 & 3, px, py, pz, pw, raw[4], raw[5], raw[6], Reg(0x2206),
                f(0x4000 + 111 * 4), f(0x4000 + 111 * 4 + 1), f(0x4000 + 111 * 4 + 2), f(0x4000 + 111 * 4 + 3),
                f(0x4000 + 154 * 4), f(0x4000 + 154 * 4 + 1), f(0x4000 + 154 * 4 + 2), f(0x4000 + 154 * 4 + 3),
                RegF(0x210F), RegF(0x2110), RegF(0x2111), RegF(0x2112));
        }
    }
    // Indices
    std::vector<uint32_t> idx;
    bool indexed = d.source != 2 || expand;
    uint32_t indexOffset = Reg(0x2102);   // VGT_INDX_OFFSET: se suma a cada indice
    {
        static int logged = 0;
        if (indexOffset && logged < 4) { logged++; LOG("[d3d12] VGT_INDX_OFFSET = %u (draw de %u indices)", indexOffset, d.numIndices); }
    }
    if (d.source == 0)
    {
        idx.resize(d.numIndices);
        const uint8_t* src = g_base + (0xA0000000u | (d.indexAddr & 0x1FFFFFFF));
        for (uint32_t i = 0; i < d.numIndices; i++)
        {
            uint32_t v = d.index32 ? *(const uint32_t*)(src + i * 4) : *(const uint16_t*)(src + i * 2);
            if (d.index32) { if (d.indexEndian == 2) v = __builtin_bswap32(v); else if (d.indexEndian == 3) v = (v << 16) | (v >> 16); }
            else if (d.indexEndian == 1 || d.indexEndian == 2) v = __builtin_bswap16((uint16_t)v);
            idx[i] = v + indexOffset;   // VGT_INDX_OFFSET (sesion 21)
        }
    }
    else if (d.source == 1) idx = d.immediate;
    else if (expand) { idx.resize(d.numIndices); for (uint32_t i = 0; i < d.numIndices; i++) idx[i] = i; }
    if (expand)
    {
        std::vector<uint32_t> out;
        if (d.primType == 5) for (size_t i = 2; i < idx.size(); i++) { out.push_back(idx[0]); out.push_back(idx[i - 1]); out.push_back(idx[i]); }
        else for (size_t i = 0; i + 3 < idx.size() + 0 && i + 3 <= idx.size() - 1 + 1; i += 4) { if (i + 3 >= idx.size()) break; out.insert(out.end(), { idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3] }); }
        idx.swap(out);
    }

    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    D3D12_CPU_DESCRIPTOR_HANDLE edramRtv = Rtv(rt->rtv);
    // Sesion 24: aliasing de EDRAM. Un rectangulo de solo profundidad (modo 5, z siempre y con escritura) cuya base de profundidad
    // coincide con la del color escribe D24S8 encima del color en la consola: el juego lo usa para borrar la escena a negro antes
    // de pintar los brillos del bloom (y en los retratos). Aqui color y profundidad son recursos distintos: se aplica al color.
    static bool noAlias = getenv("GH6_NODEPTHALIAS") != nullptr;
    if (!noAlias && (Reg(0x2208) & 7) == 5 && (Reg(0x2002) & 0xFFF) == (Reg(0x2001) & 0xFFF) && getenv("GH6_ALIASLOG"))
    {
        static int nl = 0;
        if (nl++ < 80) LOG("[edram] alias candidato: surf %08X rect %d prim %u dc %08X stencilref %08X", Reg(0x2000), (int)rect, d.primType, dc, Reg(0x210C));
    }
    if (!noAlias && rect && (Reg(0x2208) & 7) == 5 && (dc & 6) == 6 && ((dc >> 4) & 7) == 7 &&
        (Reg(0x2002) & 0xFFF) == (Reg(0x2001) & 0xFFF) && rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM && !vs.info.vertexFetchSlots.empty() &&
        (dc & 1) && ((dc >> 8) & 7) == 7 && ((dc >> 14) & 7) == 2)   // stencil "siempre" + "reemplazar": se escriben los 32 bits
    {
        uint32_t stride = 8;
        size_t sp = vs.info.hlsl.find("; vfStride = ");   // la asignacion del fetch (no la declaracion "uint vfStride = 0")
        if (sp != std::string::npos) stride = (uint32_t)strtoul(vs.info.hlsl.c_str() + sp + 13, nullptr, 10);
        if (stride < 8) stride = 8;
        uint32_t vf = vs.info.vertexFetchSlots[0], vdw0 = Reg(0x4800 + vf * 2), vdw1 = Reg(0x4800 + vf * 2 + 1);
        const uint8_t* vb = g_base + (0xA0000000u | (vdw0 & 0x1FFFFFFC));
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, z = 0;
        for (int i = 0; i < 3; i++)
        {
            float f[3];
            for (int k = 0; k < 3; k++) { uint32_t u = *(const uint32_t*)(vb + i * stride + k * 4); if ((vdw1 & 3) == 2) u = __builtin_bswap32(u); memcpy(&f[k], &u, 4); }
            minx = std::min(minx, f[0]); maxx = std::max(maxx, f[0]); miny = std::min(miny, f[1]); maxy = std::max(maxy, f[1]);
            if (i == 0) z = stride >= 12 ? f[2] : 0.0f;
        }
        z = z * RegF(0x2113) + RegF(0x2114);   // ZSCALE / ZOFF (sin efecto si el VTE no los usa: aqui solo se usan como aproximacion)
        if (!(z >= -0.01f && z <= 1.01f)) z = 0;
        uint32_t stencil = (dc & 1) ? (Reg(0x210C) & 0xFF) : 0;
        uint32_t dw = ((uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8) | stencil;
        // Disposicion de k_8_8_8_8 en la EDRAM: R en los bits 0-7 ... A en 24-31
        float col[4] = { (dw & 0xFF) / 255.0f, ((dw >> 8) & 0xFF) / 255.0f, ((dw >> 16) & 0xFF) / 255.0f, (dw >> 24) / 255.0f };
        uint32_t amsx, amsy; MsaaScale(amsx, amsy);
        int wox, woy; WindowOffset(wox, woy);
        D3D12_RECT r{ (LONG)std::max(0.0f, (minx + 0.5f + wox) * amsx), (LONG)std::max(0.0f, (miny + 0.5f + woy) * amsy),
                      (LONG)std::min((float)rt->w, (maxx + 0.5f + wox) * amsx), (LONG)std::min((float)rt->h, (maxy + 0.5f + woy) * amsy) };
        if (r.right > r.left && r.bottom > r.top) g_list->ClearRenderTargetView(edramRtv, col, 1, &r);
        static int logged = 0;
        if (logged < 60) { logged++; LOG("[edram] borrado por aliasing profundidad->color: surf %08X f%llu base %u, rect %ld,%ld-%ld,%ld, z %g, dword %08X (stride %u, endian %u, x %g..%g, y %g..%g)", Reg(0x2000), (unsigned long long)g_frameCounter, rt->base, r.left, r.top, r.right, r.bottom, z, dw, stride, vdw1 & 3, minx, maxx, miny, maxy); }
    }
    DepthRt* drt = useDepth ? GetDepthRt() : nullptr;
    if (useDepth && !drt) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = drt ? Dsv(drt->dsv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (rt1) { D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { edramRtv, Rtv(rt1->rtv) }; g_list->OMSetRenderTargets(2, rtvs, FALSE, drt ? &dsvH : nullptr); }
    else g_list->OMSetRenderTargets(1, &edramRtv, FALSE, drt ? &dsvH : nullptr);
    if (drt) g_list->OMSetStencilRef(Reg(0x210C) & 0xFF);
    g_boundRt = rt;
    // Viewport en muestras: el VS mapea pixeles logicos (pitch x kEdramH) y el viewport los escala por el MSAA
    uint32_t msx, msy; MsaaScale(msx, msy);
    D3D12_VIEWPORT vp{ 0, 0, (float)rt->w, (float)(kEdramH * msy), 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)rt->w, (LONG)rt->h };
    // Sesion 24: scissor (como GetScissor de Xenia): PA_SC_WINDOW_SCISSOR (+ PA_SC_WINDOW_OFFSET salvo window_offset_disable)
    // ∩ PA_SC_SCREEN_SCISSOR. La interfaz recorta con el las "ventanas" (p. ej. el viewport de fondo del lobby, fuera de pantalla).
    static bool noScissor = getenv("GH6_NOSCISSOR") != nullptr;
    // GH6_SCISSORBIG=1: solo en superficies de >= 640 de ancho (prueba; el resultado en canciones fue el mismo)
    static bool scissorBig = getenv("GH6_SCISSORBIG") != nullptr;
    uint32_t sPitch = Reg(0x2000) & 0x3FFF;
    if (!noScissor && (!scissorBig || sPitch >= 640))
    {
        uint32_t wtl = Reg(0x2081), wbr = Reg(0x2082), stl = Reg(0x200E), sbr = Reg(0x200F), wo = Reg(0x2080);
        auto s15 = [](uint32_t v) { return (int32_t)(v << 17) >> 17; };
        int32_t tlx = (int32_t)(wtl & 0x3FFF), tly = (int32_t)((wtl >> 16) & 0x3FFF), brx = (int32_t)(wbr & 0x3FFF), bry = (int32_t)((wbr >> 16) & 0x3FFF);
        if (!(wtl >> 31)) { int32_t ox = s15(wo & 0x7FFF), oy = s15((wo >> 16) & 0x7FFF); tlx += ox; tly += oy; brx += ox; bry += oy; }
        tlx = std::max(tlx, s15(stl & 0x7FFF)); tly = std::max(tly, s15((stl >> 16) & 0x7FFF));
        brx = std::min(brx, s15(sbr & 0x7FFF)); bry = std::min(bry, s15((sbr >> 16) & 0x7FFF));
        tlx = std::max(tlx, 0); tly = std::max(tly, 0); brx = std::max(brx, tlx); bry = std::max(bry, tly);
        sr.left = std::min<LONG>(tlx * (LONG)msx, (LONG)rt->w); sr.top = std::min<LONG>(tly * (LONG)msy, (LONG)rt->h);
        sr.right = std::min<LONG>(brx * (LONG)msx, (LONG)rt->w); sr.bottom = std::min<LONG>(bry * (LONG)msy, (LONG)rt->h);
    }
    g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
    g_list->SetPipelineState(pso.Get());
    g_list->SetGraphicsRootSignature(g_drawRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_VIRTUAL_ADDRESS ringVa = g_ring[g_fi]->GetGPUVirtualAddress();
    g_list->SetGraphicsRootConstantBufferView(0, ringVa + vsCbOff);
    g_list->SetGraphicsRootConstantBufferView(1, ringVa + psCbOff);
    g_list->SetGraphicsRootShaderResourceView(2, ringVa);
    {
        // Sesion 18: antes 8192 descriptores por frame = solo 512 draws; los demas (la interfaz, que va al final) salian blancos
        UINT baseSlot = 32 + g_fi * kDescPerFrame + g_descCursor;
        if (g_descCursor + kDescPerDraw > kDescPerFrame) { baseSlot = 1; g_skipped[5]++; } // sin descriptores: todo blanco
        else
        {
            g_descCursor += kDescPerDraw;
            static bool noTex = getenv("GH6_NOTEXTURES") != nullptr;
            struct TexTimer { double t0 = QpcMs(); ~TexTimer() { g_hTexMs += QpcMs() - t0; } } ttimer;
            for (uint32_t t = 0; t < 16; t++)
            {
                bool ok = (ps.info.textureMask & (1u << t)) && !noTex && BindTexture(t, SrvCpu(baseSlot + t));
                if (!ok) g_dev->CreateShaderResourceView(g_white.Get(), nullptr, SrvCpu(baseSlot + t));
                // Huecos 3D (t17-t32): textura de volumen o, si no hay, un SRV nulo 3D
                bool ok3 = (ps.info.texture3DMask & (1u << t)) && !noTex && BindTexture3D(t, SrvCpu(baseSlot + 16 + t));
                if (!ok3)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nd{}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.Texture3D.MipLevels = 1;
                    g_dev->CreateShaderResourceView(nullptr, &nd, SrvCpu(baseSlot + 16 + t));
                }
                // Huecos cubo (t33-t48)
                bool okc = (ps.info.textureCubeMask & (1u << t)) && !noTex && BindTextureCube(t, SrvCpu(baseSlot + 32 + t));
                if (!okc)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nd{}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.TextureCube.MipLevels = 1;
                    g_dev->CreateShaderResourceView(nullptr, &nd, SrvCpu(baseSlot + 32 + t));
                }
                // Registro de texturas por draw en el frame de GH6_DRAWLOG (sesion 23)
                uint32_t used = (ps.info.textureMask | ps.info.texture3DMask | ps.info.textureCubeMask) & (1u << t);
                if (used && (long long)g_frameCounter == DrawLogFrame())
                {
                    uint32_t c0 = Reg(0x4800 + t * 6), c1 = Reg(0x4800 + t * 6 + 1), c2 = Reg(0x4800 + t * 6 + 2), c5 = Reg(0x4800 + t * 6 + 5);
                    LOG("[drawtex] tf%u dw0 %08X dw3 %08X uso %s dim %u fmt %u base %08X tam %08X tipo %u -> %s", t, c0, Reg(0x4800 + t * 6 + 3),
                        (ps.info.texture3DMask >> t) & 1 ? "3D" : (ps.info.textureCubeMask >> t) & 1 ? "cubo" : "2D",
                        (c5 >> 9) & 3, c1 & 0x3F, c1 & 0xFFFFF000, c2, c0 & 3, (ok || ok3 || okc) ? "cargada" : "BLANCA/NULA");
                }
            }
        }
        g_list->SetGraphicsRootDescriptorTable(3, SrvGpu(baseSlot));
    }
    CmdTrace("draw p%u n%u idx %zu indexado %d rect %d rt %ux%u fmt %d depth %d", d.primType, d.numIndices, idx.size(), (int)indexed, (int)rect, rt->w, rt->h, (int)rt->fmt, (int)useDepth);
    g_list->IASetPrimitiveTopology(topo);
    if (indexed)
    {
        if (idx.empty()) return;
        UINT64 o = RingAlloc(idx.size() * 4, 16);
        if (o == ~0ull) { g_skipped[3]++; return; }
        memcpy(g_ringMap[g_fi] + o, idx.data(), idx.size() * 4);
        D3D12_INDEX_BUFFER_VIEW ib{ ringVa + o, (UINT)(idx.size() * 4), DXGI_FORMAT_R32_UINT };
        g_list->IASetIndexBuffer(&ib);
        g_list->DrawIndexedInstanced((UINT)idx.size(), 1, 0, 0, 0);
    }
    else g_list->DrawInstanced(d.numIndices, 1, 0, 0);
    g_frameDraws++;
    {   // Aliasing de EDRAM color -> profundidad (sesion 24): color 8888 escrito en la base del buffer de profundidad (modo 4)
        static const bool noC2D0 = getenv("GH6_NOCOLOR2DEPTH") != nullptr;
        static const double c2dOffAt = getenv("GH6_C2DOFFAT") ? atof(getenv("GH6_C2DOFFAT")) : -1;   // diagnostico: quitarlo desde el segundo N
        static const double c2dStart = QpcMs();
        const bool noC2D = noC2D0 || (c2dOffAt >= 0 && QpcMs() - c2dStart >= c2dOffAt * 1000);
        if (!noC2D && g_colorToDepthPso && rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM && (Reg(0x2208) & 7) == 4 &&
            (Reg(0x2001) & 0xFFF) == (Reg(0x2002) & 0xFFF) && (Reg(0x2104) & 0xF) != 0)
        {
            if (DepthRt* zr = GetDepthRt())
            {
                if (zr->w == rt->w)
                {
                    D3D12_RESOURCE_BARRIER b0 = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    g_list->ResourceBarrier(1, &b0);
                    UINT zslot = 6 + g_fi;
                    if (g_descCursor + 1 <= kDescPerFrame) { zslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
                    D3D12_SHADER_RESOURCE_VIEW_DESC zsd{}; zsd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; zsd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    zsd.Texture2D.MipLevels = 1; zsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    g_dev->CreateShaderResourceView(rt->res.Get(), &zsd, SrvCpu(zslot));
                    D3D12_CPU_DESCRIPTOR_HANDLE zdsv = Dsv(zr->dsv);
                    g_list->OMSetRenderTargets(0, nullptr, FALSE, &zdsv);
                    g_boundRt = nullptr;
                    D3D12_VIEWPORT zvp{ 0, 0, (float)rt->w, (float)rt->h, 0, 1 };
                    g_list->RSSetViewports(1, &zvp); g_list->RSSetScissorRects(1, &sr);
                    g_list->SetPipelineState(g_colorToDepthPso.Get());
                    g_list->SetGraphicsRootSignature(g_colorToDepthRootSig.Get());
                    ID3D12DescriptorHeap* zheaps[] = { g_srvHeap.Get() };
                    g_list->SetDescriptorHeaps(1, zheaps);
                    g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(zslot));
                    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    g_list->DrawInstanced(3, 1, 0, 0);
                    D3D12_RESOURCE_BARRIER b1 = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    g_list->ResourceBarrier(1, &b1);
                    static int logged = 0;
                    if (logged++ < 4) LOG("[edram] aliasing color->profundidad: base %u, pitch %u, rect %ld,%ld-%ld,%ld (PS %016llX)", rt->base, rt->w, sr.left, sr.top, sr.right, sr.bottom, (unsigned long long)d.psHash);
                }
            }
        }
    }
}

// GH6_DUMPFRAME=n: guarda el render target EDRAM del frame n en frame_n.bmp
static void DumpEdram(UINT64 frame)
{
    // Vuelca TODAS las superficies EDRAM (recortadas a 720 de alto): edram_<frame>_b<base>_p<pitch>.bmp
    for (auto& kv : g_edramRts)
    {
        EdramRt& rt = kv.second;
        // 8888 tal cual; R32_FLOAT y R16G16_SNORM se convierten a gris/rojo-verde para diagnostico (sesion 23)
        bool isR32 = rt.fmt == DXGI_FORMAT_R32_FLOAT, isRG16 = rt.fmt == DXGI_FORMAT_R16G16_SNORM;
        if (rt.fmt != DXGI_FORMAT_B8G8R8A8_UNORM && !isR32 && !isRG16) continue;
        static int onlyPitch = getenv("GH6_DUMPPITCH") ? atoi(getenv("GH6_DUMPPITCH")) : 0;   // filtro opcional por pitch
        if (onlyPitch && (int)rt.w != onlyPitch) continue;
        UINT w = rt.w, h = std::min<UINT>(rt.h, kRtH), rowPitch = (w * 4 + 255) & ~255u;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(rowPitch) * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
        ComPtr<ID3D12Resource> buf;
        if (FAILED(g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf)))) continue;
        ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l;
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
        auto t1 = Transition(rt.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); l->ResourceBarrier(1, &t1);
        D3D12_TEXTURE_COPY_LOCATION dst{ buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { rt.fmt, w, h, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION src{ rt.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_BOX dbox{ 0, 0, 0, w, h, 1 };
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, &dbox);
        auto t2 = Transition(rt.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); l->ResourceBarrier(1, &t2);
        l->Close();
        ID3D12CommandList* ls[] = { l.Get() }; g_queue->ExecuteCommandLists(1, ls);
        g_queue->Signal(g_fence.Get(), g_nextFence); g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 5000);
        uint8_t* p = nullptr; buf->Map(0, nullptr, (void**)&p);
        if (isR32 || isRG16)
        {
            float mn = 1e30f, mx = -1e30f;
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) { float v; if (isR32) memcpy(&v, p + y * rowPitch + x * 4, 4); else v = *(int16_t*)(p + y * rowPitch + x * 4) / 32767.0f; mn = std::min(mn, v); mx = std::max(mx, v); }
            LOG("[edram] volcado %s b%u p%u: min %g max %g", isR32 ? "R32F" : "RG16", rt.base, rt.w, mn, mx);
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++)
            {
                uint8_t* q = p + y * rowPitch + x * 4; uint8_t R, G;
                if (isR32) { float v; memcpy(&v, q, 4); R = G = (uint8_t)std::clamp((v - mn) / std::max(mx - mn, 1e-20f) * 255.0f, 0.0f, 255.0f); }
                else { R = (uint8_t)std::clamp(*(int16_t*)q / 32767.0f * 127.5f + 127.5f, 0.0f, 255.0f); G = (uint8_t)std::clamp(*(int16_t*)(q + 2) / 32767.0f * 127.5f + 127.5f, 0.0f, 255.0f); }
                q[0] = isR32 ? R : 0; q[1] = G; q[2] = R; q[3] = 255;
            }
        }
        char name[96]; snprintf(name, sizeof(name), "edram_%llu_b%u_p%u%s.bmp", (unsigned long long)frame, rt.base, rt.w, isR32 ? "_r32f" : isRG16 ? "_rg16" : "");
        if (FILE* f = fopen(name, "wb"))
        {
            uint32_t size = w * h * 4;
            uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
            fwrite(hdr, 1, 54, f);
            for (UINT y = 0; y < h; y++) fwrite(p + y * rowPitch, 1, w * 4, f);
            fclose(f);
        }
        buf->Unmap(0, nullptr);
    }
    LOG("[d3d12] volcadas %zu superficies EDRAM del frame %llu", g_edramRts.size(), (unsigned long long)frame);
}

// Presenta un frame: si hubo draws, el render target EDRAM; si no, el frontbuffer (BGRA w x h) o nada.
// Presenta directamente la textura resuelta por GPU del frontbuffer (sesion 21). Devuelve false si no hay (se usa la memoria).
// readback != nullptr: ademas copia la imagen a la CPU (BGRA, como ReadFrontbuffer) para las capturas.
bool D3D12Present(HWND hwnd, const uint32_t* pixels, uint32_t w, uint32_t h);
// Texturas intermedias del posproceso (mismo tamano que la imagen resuelta), con RTV propio y SRV fijo (kPpSrvSlot + i)
bool EnsurePpTex(UINT w, UINT h)
{
    if (g_ppTex[0] && g_ppW == w && g_ppH == h) return true;
    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!g_ppRtvHeap) { D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 2; if (FAILED(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_ppRtvHeap)))) return false; }
    if (g_ppTex[0]) { FlushAndWait(); g_ppTex[0].Reset(); g_ppTex[1].Reset(); }   // cambio de tamano: esperar a que la GPU deje las anteriores
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 2; i++)
    {
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_ppTex[i])))) return false;
        g_ppState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        auto rtv = g_ppRtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += i * inc;
        g_dev->CreateRenderTargetView(g_ppTex[i].Get(), nullptr, rtv);
        g_dev->CreateShaderResourceView(g_ppTex[i].Get(), nullptr, SrvCpu(kPpSrvSlot + i));
    }
    g_ppW = w; g_ppH = h;
    LOG("[d3d12] texturas de posproceso %ux%u", w, h);
    return true;
}
// Una pasada de posproceso: lee el SRV srcSlot y escribe en g_ppTex[idx]
void PostPass(ID3D12PipelineState* pso, UINT srcSlot, int idx)
{
    if (g_ppState[idx] != D3D12_RESOURCE_STATE_RENDER_TARGET) { auto b = Transition(g_ppTex[idx].Get(), g_ppState[idx], D3D12_RESOURCE_STATE_RENDER_TARGET); g_list->ResourceBarrier(1, &b); }
    auto rtv = g_ppRtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += idx * g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, (float)g_ppW, (float)g_ppH, 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)g_ppW, (LONG)g_ppH };
    g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
    g_list->SetPipelineState(pso);
    g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(srcSlot));
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    auto b = Transition(g_ppTex[idx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b);
    g_ppState[idx] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
bool D3D12FlushPendingResolves() { if (!g_dev || g_pendingResolvesEmpty()) return false; FlushPendingResolves(); return true; }

bool D3D12PresentResolved(HWND hwnd, uint32_t fb, std::vector<uint32_t>* readback, uint32_t& outW, uint32_t& outH)
{
    if (g_dev) FlushPendingResolves();
    if (g_failed || !g_dev || !hwnd || !fb) return false;
    auto last = g_resolvedLast.find(fb & 0xFFFFF000);
    if (last == g_resolvedLast.end()) return false;
    auto it = g_resolved.find(last->second);
    if (it == g_resolved.end() || !it->second.res || g_frameCounter - it->second.frame > 2) return false;
    ResolvedTex& r = it->second;
    OpenFrame();
    UINT fi = g_fi;
    outW = r.w; outH = r.h;
    if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b); r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    sd.Shader4ComponentMapping = r.swapRB ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                                          : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    g_dev->CreateShaderResourceView(r.res.Get(), &sd, SrvCpu(3 + fi));
    // Sesion 24: posproceso a la resolucion del juego antes del escalado: FXAA (suavizado de bordes) y CAS (nitidez)
    bool doFxaa = AaMode() && g_fxaaPso, doCas = g_casPso != nullptr;
    UINT curSlot = 3 + fi; int lastPp = -1;
    if ((doFxaa || doCas) && EnsurePpTex(r.w, r.h))
    {
        if (doFxaa) { PostPass(g_fxaaPso.Get(), curSlot, 0); curSlot = kPpSrvSlot; lastPp = 0; }
        if (doCas) { PostPass(g_casPso.Get(), curSlot, 1); curSlot = kPpSrvSlot + 1; lastPp = 1; }
    }
    bool aa = lastPp >= 0;
    if (readback)
    {
        // Copia a la CPU para la captura (con suavizado, la imagen suavizada: sus bytes ya estan en orden B G R)
        ID3D12Resource* src = aa ? g_ppTex[lastPp].Get() : r.res.Get();
        bool swapRB = aa ? true : r.swapRB;
        { auto b = Transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &b); }
        UINT rowPitch = (r.w * 4 + 255) & ~255u;
        D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, r.w, r.h, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION sl{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
        if (g_readback)
        {
            CheckedCopy(&dst, 0, 0, 0, &sl, nullptr);
            { auto b = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b); }
            FlushAndWait();
            uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
            if (p)
            {
                readback->resize(size_t(r.w) * r.h);
                for (UINT y = 0; y < r.h; y++)
                    for (UINT x = 0; x < r.w; x++)
                    {
                        const uint8_t* px = p + y * rowPitch + x * 4;   // B G R A del RT
                        // Igual que el camino por memoria: con swapRB la imagen sale con los colores del RT; sin el, con R y B cambiados
                        uint32_t b = swapRB ? px[0] : px[2], g = px[1], rr = swapRB ? px[2] : px[0];
                        (*readback)[size_t(y) * r.w + x] = b | (g << 8) | (rr << 16) | 0xFF000000u;
                    }
                g_readback->Unmap(0, nullptr);
            }
        }
        else { auto b = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b); }
    }
    auto toRt = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->ResourceBarrier(1, &toRt);
    D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = Rtv(fi);
    g_list->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    { const float black[4] = { 0, 0, 0, 1 }; g_list->ClearRenderTargetView(bbRtv, black, 0, nullptr); }
    D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
    g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
    g_list->SetPipelineState(g_blitPso.Get());
    g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(curSlot));
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    auto toPresent = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_list->ResourceBarrier(1, &toPresent);
    HRESULT phr = g_list->Close();
    if (SUCCEEDED(phr)) { ID3D12CommandList* lists[] = { g_list.Get() }; g_queue->ExecuteCommandLists(1, lists); }
    else { static int pn = 0; if (pn++ < 10) LOG("[d3d12] Close de la lista fallo al presentar (0x%08lX, draws %llu): se descarta", (unsigned long)phr, (unsigned long long)g_frameDraws); DumpCmdTrace(phr); }
    g_cmdTrace.clear();
    {
        // GH6_VSYNC=0: presentar sin esperar al vsync (el Present bloqueaba el hilo de comandos de la GPU hasta 16 ms)
        static const UINT syncInterval = getenv("GH6_VSYNC") ? (UINT)atoi(getenv("GH6_VSYNC")) : 1u;
        double tp = QpcMs(); g_swap->Present(syncInterval, 0); g_hPresentMs += QpcMs() - tp;
    }
    g_fenceValues[fi] = g_nextFence;
    g_queue->Signal(g_fence.Get(), g_nextFence++);
    g_frameOpen = false;
    ResizeSwapIfNeeded();
    SavePsoLibrary(false);
    {   // Tirones: intervalo entre presentaciones por encima del umbral, con el desglose de lo que costo ese frame
        static const double hitchMs = getenv("GH6_HITCHMS") ? atof(getenv("GH6_HITCHMS")) : 40.0;
        static double last = 0; static uint64_t hn = 0, hitches = 0; double now = QpcMs(); hn++;
        double dt = last > 0 ? now - last : 0; last = now;
        {   // GH6_FRAMELOG=<desde>:<cuantos>: draws y tiempo de cada frame presentado (ritmo de la escena 3D, sesion 24)
            static long long flFrom = -1, flCount = 0;
            static bool flInit = false;
            if (!flInit) { flInit = true; if (const char* s = getenv("GH6_FRAMELOG")) { flFrom = atoll(s); if (const char* c = strchr(s, ':')) flCount = atoll(c + 1); } }
            if (flFrom >= 0 && (long long)hn >= flFrom && (long long)hn < flFrom + flCount)
                LOG("[framelog] %llu: %.1f ms, draws %llu (%.1f ms), present %.1f ms, espera GPU %.1f ms", (unsigned long long)hn, dt, (unsigned long long)g_frameDraws, g_hDrawMs, g_hPresentMs, g_hWaitMs);
        }
        if (dt > hitchMs && hitches++ < 2000)
            LOG("[tiron] frame %llu: %.0f ms | compilar %.0f ms (%u nuevos, %u de cache) | PSO %.0f ms (%u) | texturas %.0f ms | espera GPU %.0f ms | vaciados %.0f ms (%u)",
                (unsigned long long)hn, dt, g_hCompileMs, g_hCompiles, g_hCacheHits, g_hPsoMs, g_hPsos, g_hTexMs, g_hWaitMs, g_hFlushMs, g_hFlushes);
        g_hCompileMs = g_hPsoMs = g_hTexMs = g_hWaitMs = g_hFlushMs = g_hDrawMs = g_hPresentMs = 0; g_hCompiles = g_hCacheHits = g_hPsos = g_hFlushes = 0;
    }
    static uint64_t n = 0;
    if (++n % 120 == 0)
    {
        LOG("[d3d12] presentacion directa %llu: resolves %llu (por GPU %llu) | por frame: texturas subidas %.1f (%.1f MB), vertices copiados %.1f MB",
            (unsigned long long)n, (unsigned long long)g_resolves, (unsigned long long)g_gpuResolves, g_texUploads / 120.0, g_texUploadBytes / 120.0 / 1048576.0, g_vbCopyBytes / 120.0 / 1048576.0);
        g_texUploads = g_texUploadBytes = g_vbCopyBytes = 0;
        LOG("[d3d12] por frame: resolves por GPU %.1f, por CPU %.1f, volcados GPU->memoria %.1f", g_gpuResolvesWin / 120.0, g_cpuResolvesWin / 120.0, g_writeBacks / 120.0);
        g_gpuResolvesWin = g_cpuResolvesWin = g_writeBacks = 0;
        if (g_flushTotal) LOG("[d3d12] resolves por CPU diferidos: %.1f por frame en %.1f esperas (%.1f por solape con texturas/vertices)", g_deferredCount / 120.0, g_flushTotal / 120.0, g_flushOverlap / 120.0);
        g_deferredCount = g_flushTotal = g_flushOverlap = 0;
    }
    return true;
}

bool D3D12Present(HWND hwnd, const uint32_t* pixels, uint32_t w, uint32_t h)
{
    if (g_failed || !hwnd) return false;
    if (!g_dev && !Init(hwnd)) return false;
    OpenFrame();
    UINT fi = g_fi;
    static bool showEdram = getenv("GH6_SHOWEDRAM") != nullptr;
    bool useEdram = showEdram && g_frameDraws > 0;
    auto toRt = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->ResourceBarrier(1, &toRt);
    const float clear[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    g_list->ClearRenderTargetView(Rtv(fi), clear, 0, nullptr);
    UINT srvSlot = 0; bool blit = false;
    if (useEdram)
    {
        auto b = Transition(g_edram.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b);
        srvSlot = 17; blit = true;
    }
    else if (pixels && w && h)
    {
        if (!g_fbTexture || g_fbW != w || g_fbH != h)
        {
            g_fbW = w; g_fbH = h;
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_fbTexture));
            g_dev->CreateShaderResourceView(g_fbTexture.Get(), nullptr, SrvCpu(0));
        }
        UINT64 pitch = (UINT64(w) * 4 + 255) & ~255ull;
        UINT64 o = RingAlloc(pitch * h, 512);
        if (o != ~0ull)
        {
            for (uint32_t y = 0; y < h; y++) memcpy(g_ringMap[fi] + o + y * pitch, pixels + size_t(y) * w, size_t(w) * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{ g_fbTexture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{ g_ring[fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = o; src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, (UINT)pitch };
            auto a = Transition(g_fbTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_list->ResourceBarrier(1, &a);
            CheckedCopy(&dst, 0, 0, 0, &src, nullptr);
            auto b = Transition(g_fbTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_list->ResourceBarrier(1, &b);
            srvSlot = 0; blit = true;
        }
    }
    if (blit)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = Rtv(fi);
        g_list->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
        g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
        g_list->SetPipelineState(g_blitPso.Get());
        g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        g_list->SetDescriptorHeaps(1, heaps);
        g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(srvSlot));
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_list->DrawInstanced(3, 1, 0, 0);
    }
    if (useEdram) { auto b = Transition(g_edram.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); g_list->ResourceBarrier(1, &b); }
    auto toPresent = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_list->ResourceBarrier(1, &toPresent);
    HRESULT phr = g_list->Close();
    if (SUCCEEDED(phr)) { ID3D12CommandList* lists[] = { g_list.Get() }; g_queue->ExecuteCommandLists(1, lists); }
    else { static int pn = 0; if (pn++ < 10) LOG("[d3d12] Close de la lista fallo al presentar (0x%08lX, draws %llu): se descarta", (unsigned long)phr, (unsigned long long)g_frameDraws); DumpCmdTrace(phr); }
    g_cmdTrace.clear();
    HRESULT hr = g_swap->Present(1, 0);
    g_fenceValues[fi] = g_nextFence;
    g_queue->Signal(g_fence.Get(), g_nextFence++);
    g_frameOpen = false;
    ResizeSwapIfNeeded();
    SavePsoLibrary(false);
    static uint64_t frames = 0;
    static long long dumpAt = getenv("GH6_DUMPFRAME") ? atoll(getenv("GH6_DUMPFRAME")) : -1;
    ++frames;
    if ((long long)frames == dumpAt) { g_fence->SetEventOnCompletion(g_nextFence - 1, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 5000); DumpEdram(frames); }
    if (frames % 120 == 0) LOG("[d3d12] frame %llu: draws %llu, omitidos prim=%llu shader=%llu pso=%llu mem=%llu, shaders %zu, psos %zu, resolves %llu (frontbuffer %llu), superficies %zu",
        (unsigned long long)frames, (unsigned long long)g_frameDraws, (unsigned long long)g_skipped[0], (unsigned long long)g_skipped[1],
        (unsigned long long)g_skipped[2], (unsigned long long)g_skipped[3], g_shaders.size(), g_psos.size(), (unsigned long long)g_resolves, (unsigned long long)g_fbResolves, g_edramRts.size());
    if (FAILED(hr)) { LOG("[d3d12] Present fallo (0x%08lX)", (unsigned long)hr); g_failed = true; return false; }
    return true;
}

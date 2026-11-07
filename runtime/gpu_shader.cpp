// Traductor de microcodigo Xenos -> HLSL SM5 (sesion 8).
// Formato del microcodigo y semantica de operandos segun Xenia (BSD): ucode.h / shader_translator.cc.
// Soportado: exec/exec_end/cexec (bool)/cexec_pred, ALU vectorial+escalar emparejadas, vfetch (formatos float/8888/16),
// tfetch 2D. No soportado aun: bucles, llamadas, saltos (el shader se marca como no valido y el draw se omite).
#include "gpu_shader.h"
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <set>
#include <cmath>
#include "runtime.h"

const char* kXenosInterfaceHlsl = R"(
struct XVSOut { float4 pos : SV_Position; float4 t[16] : TEXCOORD0; };
cbuffer XConsts : register(b0)
{
    float4 C[256];
    uint4 XB[2];          // 256 constantes booleanas
    uint4 XFetch[8];      // x = offset en el buffer de vertices, y = endian
    float4 XVte;          // x = flags VTE, y = ancho RT, z = alto RT
    float4 XVport0;       // xscale, xoffset, yscale, yoffset
    float4 XVport1;       // zscale, zoffset
    float4 XAlpha;        // x = funcion de la prueba de alfa (0-7; 8 = desactivada), y = referencia, z = param gen (-1 = no), w = msx*8+msy (sesion 24)
    uint4 XLoop[8];       // 32 constantes de bucle (0x4908+): cuenta bits 0-7, inicio 8-15, paso 16-23 (sesion 24)
    uint4 XTexMode[16];   // por textura: x = clamp xyz (3 bits c/u), y = filtro mag, z = filtro min, w = color de borde (sesion 24)
};
ByteAddressBuffer XVB : register(t0);
Texture2D XTex[16] : register(t1);
Texture3D XTex3[16] : register(t17);
TextureCube XTexC[16] : register(t33);
// Sesion 24: multiplicacion de Xenos (como D3D9 y Xenia): 0 * cualquier cosa = 0 (incluido inf/NaN)
float4 XMul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float XMulS(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float XDot4(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z + m.w; }
float XDot3(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z; }
float XDot2(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y; }
float XClampInf(float r, float rep) { return isinf(r) ? (r > 0.0 ? rep : -rep) : r; }
SamplerState XSamp : register(s0);     // lineal, repetir
SamplerState XSampP : register(s1);    // punto, repetir
// Modo de borde de Xenos (ClampMode) sobre una coordenada normalizada; los samplers repiten (sesion 24)
float XAddr(float u, uint m, float size, inout bool brd)
{
    float h = 0.5 / max(size, 1.0);
    if (m == 1u) { float t = frac(u * 0.5) * 2.0; return t > 1.0 ? 2.0 - t : t; }
    if (m == 2u || m == 4u) return clamp(u, h, 1.0 - h);
    if (m == 3u || m == 5u) return clamp(abs(u), h, 1.0 - h);
    if (m == 6u) { if (u < 0.0 || u > 1.0) brd = true; return u; }
    if (m == 7u) { if (abs(u) > 1.0) brd = true; return abs(u); }
    return u;
}
float4 XBorder(uint c) { return (c == 1u) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0); }
uint XSwap(uint v, uint e)
{
    if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (e == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (e == 3) return (v << 16) | (v >> 16);
    return v;
}
bool XBool(uint i) { return ((XB[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
)";
// Interfaz comun VS/GS/PS. Sesion 25 (panoramico): XScreen = escala de la superficie. Sesion 27: + "pano" (PANODIM), 1 = normal,
// 0 = franja lateral del panoramico (el PS la pinta de negro). Sin panoramico el HLSL no cambia.
std::string XenosInterface()
{
    std::string h = kXenosInterfaceHlsl;
    static const bool pano = getenv("GH6_PANORAMICO") && *getenv("GH6_PANORAMICO") && _stricmp(getenv("GH6_PANORAMICO"), "16:9") != 0;
    if (!pano) return h;
    size_t q = h.find("uint4 XTexMode[16];");
    if (q != std::string::npos) h.insert(h.find('\n', q) + 1, "    float4 XScreen;       // x, y = pixeles de superficie por pixel del juego; z = compresion de la interfaz (0 = no)\n");
    q = h.find("float4 t[16] : TEXCOORD0; };");
    if (q != std::string::npos) h.replace(q, 29, "float4 t[16] : TEXCOORD0; float pano : PANODIM; };");
    return "#define XPANO_DIM 1\n" + h;
}


bool g_xlatNoGrad = false;   // sesion 24: retraduccion sin gradientes (ver GetShader de gpu_d3d12.cpp)
namespace
{
uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

struct Gen
{
    std::string out;
    void L(const char* fmt, ...)
    {
        char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        out += "    "; out += buf; out += "\n";
    }
};

const char* kComp = "xyzw";

// Operando de la ALU (i = 1..3) con swizzle completo de 4 componentes
std::string AluSrc(const uint32_t* w, int i, bool isPs)
{
    uint32_t reg = i == 1 ? Bits(w[2], 16, 8) : i == 2 ? Bits(w[2], 8, 8) : Bits(w[2], 0, 8);
    bool temp = i == 1 ? Bits(w[2], 31, 1) : i == 2 ? Bits(w[2], 30, 1) : Bits(w[2], 29, 1);
    uint32_t swz = i == 1 ? Bits(w[1], 16, 8) : i == 2 ? Bits(w[1], 8, 8) : Bits(w[1], 0, 8);
    bool neg = i == 1 ? Bits(w[1], 26, 1) : i == 2 ? Bits(w[1], 25, 1) : Bits(w[1], 24, 1);
    std::string s;
    if (temp)
    {
        // Sesion 24: temporal relativo al indice de bucle (bit 6: r[aL + n])
        s = (reg & 0x40) ? "r[clamp(aL + " + std::to_string(reg & 0x3F) + ", 0, 63)]" : "r[" + std::to_string(reg & 0x3F) + "]";
        if (reg & 0x80) s = "abs(" + s + ")";
    }
    else
    {
        // Constante direccionada (regla de Xenia): const_0_rel_abs / const_1_rel_abs segun cuantos operandos previos son constantes;
        // relativa a a0 si const_address_register_relative, si no a aL
        bool t1 = Bits(w[2], 31, 1), t2 = Bits(w[2], 30, 1);
        bool rel0 = Bits(w[1], 31, 1), rel1 = Bits(w[1], 30, 1), useA0 = Bits(w[1], 29, 1);
        bool addressed = i == 1 ? rel0 : i == 2 ? (t1 ? rel0 : rel1) : ((t1 && t2) ? rel0 : rel1);
        s = addressed ? "C[clamp(" + std::to_string(reg) + " + " + (useA0 ? "a0" : "aL") + ", 0, 255)]" : "C[" + std::to_string(reg) + "]";
        if (Bits(w[0], 7, 1)) s = "abs(" + s + ")";
    }
    std::string sw = ".";
    for (int c = 0; c < 4; c++) sw += kComp[(Bits(swz, 2 * c, 2) + c) & 3];
    if (sw != ".xyzw") s += sw;
    return neg ? "(-" + s + ")" : s;
}

// Componente del operando escalar (src3) en el slot dado (3 = "a", 0 = "b")
std::string ScalarComp(const uint32_t* w, int slot)
{
    uint32_t reg = Bits(w[2], 0, 8);
    bool temp = Bits(w[2], 29, 1);
    uint32_t swz = Bits(w[1], 0, 8);
    bool neg = Bits(w[1], 24, 1);
    std::string s;
    if (temp) s = (reg & 0x40) ? "r[clamp(aL + " + std::to_string(reg & 0x3F) + ", 0, 63)]" : "r[" + std::to_string(reg & 0x3F) + "]";
    else
    {
        bool t1 = Bits(w[2], 31, 1), t2 = Bits(w[2], 30, 1);
        bool addressed = (t1 && t2) ? Bits(w[1], 31, 1) : Bits(w[1], 30, 1);
        s = addressed ? "C[clamp(" + std::to_string(reg) + " + " + (Bits(w[1], 29, 1) ? "a0" : "aL") + ", 0, 255)]" : "C[" + std::to_string(reg) + "]";
    }
    if ((temp && (reg & 0x80)) || (!temp && Bits(w[0], 7, 1))) s = "abs(" + s + ")";
    s += "."; s += kComp[(Bits(swz, 2 * slot, 2) + slot) & 3];
    return neg ? "(-" + s + ")" : s;
}

std::string ExportTarget(uint32_t reg, bool isPs, bool& valid)
{
    valid = true;
    if (!isPs)
    {
        if (reg == 62) return "o.pos";
        if (reg < 16) return "o.t[" + std::to_string(reg) + "]";
    }
    else if (reg == 0) return "oC0";
    else if (reg < 4) return "oC" + std::to_string(reg);   // MRT: oC1-oC3 -> SV_Target1-3 (sesion 24)
    else if (reg == 61) return "oDepthV";   // exportacion de profundidad (sesion 23)
    valid = false;
    return "oDummy";
}

static const bool g_mulZero = getenv("GH6_NOMULZERO") == nullptr;   // sesion 24
void EmitAlu(Gen& g, const uint32_t* w, bool isPs)
{
    // Sesion 24: maxa/maxas/maxasf escriben a0 al final de la instruccion: las fuentes relativas de la misma instruccion
    // (p. ej. "max r4, c[128+a0]" con maxas en la parte escalar) leen el a0 anterior. Antes se asignaba antes y las colas de
    // las notas largas leian otra entrada de la tabla de constantes (lineas horizontales por toda la pantalla).
    static const bool a0Early = getenv("GH6_A0EARLY") != nullptr;
    std::string a0Expr;
    uint32_t vdst = Bits(w[0], 0, 6), sdst = Bits(w[0], 8, 6);
    bool exportData = Bits(w[0], 15, 1);
    uint32_t vmask = Bits(w[0], 16, 4), smask = Bits(w[0], 20, 4);
    bool vclamp = Bits(w[0], 24, 1), sclamp = Bits(w[0], 25, 1);
    uint32_t sop = Bits(w[0], 26, 6), vop = Bits(w[2], 24, 5);
    bool pred = Bits(w[1], 28, 1), predCond = Bits(w[1], 27, 1);
    std::string a = AluSrc(w, 1, isPs), b = AluSrc(w, 2, isPs), c = AluSrc(w, 3, isPs);

    if (pred) g.L("if (p0 == %s) {", predCond ? "true" : "false");
    // --- vectorial ---
    std::string v;
    switch (vop)
    {
    case 0: v = a + " + " + b; break;
    case 1: v = g_mulZero ? "XMul(" + a + ", " + b + ")" : a + " * " + b; break;
    case 2: v = "max(" + a + ", " + b + ")"; break;
    case 3: v = "min(" + a + ", " + b + ")"; break;
    case 4: v = "float4(" + a + " == " + b + ")"; break;
    case 5: v = "float4(" + a + " > " + b + ")"; break;
    case 6: v = "float4(" + a + " >= " + b + ")"; break;
    case 7: v = "float4(" + a + " != " + b + ")"; break;
    case 8: v = "frac(" + a + ")"; break;
    case 9: v = "trunc(" + a + ")"; break;
    case 10: v = "floor(" + a + ")"; break;
    case 11: v = g_mulZero ? "(XMul(" + a + ", " + b + ") + " + c + ")" : a + " * " + b + " + " + c; break;
    case 12: v = "(" + a + " == 0.0 ? " + b + " : " + c + ")"; break;
    case 13: v = "(" + a + " >= 0.0 ? " + b + " : " + c + ")"; break;
    case 14: v = "(" + a + " > 0.0 ? " + b + " : " + c + ")"; break;
    case 15: v = g_mulZero ? "XDot4(" + a + ", " + b + ").xxxx" : "dot(" + a + ", " + b + ").xxxx"; break;
    case 16: v = g_mulZero ? "XDot3(" + a + ", " + b + ").xxxx" : "dot((" + a + ").xyz, (" + b + ").xyz).xxxx"; break;
    case 17: v = g_mulZero ? "(XDot2(" + a + ", " + b + ") + (" + c + ").x).xxxx" : "(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x).xxxx"; break;
    case 19: v = "max(max((" + a + ").x, (" + a + ").y), max((" + a + ").z, (" + a + ").w)).xxxx"; break;
    case 20: case 21: case 22: case 23:
    {
        static const char* cmp[] = { "==", "!=", ">", ">=" };
        g.L("p0 = ((%s).w %s 0.0) && ((%s).w %s 0.0);", a.c_str(), cmp[vop - 20], b.c_str(), cmp[vop - 20]);
        v = "float4(0, 0, 0, 0)"; break;
    }
    case 24: case 25: case 26: case 27:
    {
        static const char* cmp[] = { "==", ">", ">=", "!=" };
        if (isPs) g.L("if (any(%s %s %s)) discard;", a.c_str(), cmp[vop - 24], b.c_str());
        v = "(any(" + a + " " + cmp[vop - 24] + " " + b + ") ? 1.0 : 0.0).xxxx"; break;
    }
    case 28: v = "float4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"; break;
    case 29: a0Expr = "(int)clamp(floor((" + a + ").w + 0.5), -256.0, 255.0)"; v = "max(" + a + ", " + b + ")"; break;
    case 18:
    {
        // CUBE (sesion 21, como el interprete de Xenia): operando a = .z_xy de la direccion. Resultado: T, S, 2 * eje mayor, cara.
        // Se guarda la direccion en cubeDir para que el tfetch de cubo muestree un TextureCube con ella.
        g.L("{ float4 ca = %s; float cx = ca.z, cy = ca.w, cz = ca.x; cubeDir = float3(cx, cy, cz);", a.c_str());
        g.L("  float4 cr; if (abs(cz) >= abs(cx) && abs(cz) >= abs(cy)) cr = float4(-cy, cz < 0 ? -cx : cx, cz, cz < 0 ? 5 : 4);");
        g.L("  else if (abs(cy) >= abs(cx)) cr = float4(cy < 0 ? -cz : cz, cx, cy, cy < 0 ? 3 : 2);");
        g.L("  else cr = float4(-cy, cx < 0 ? cz : -cz, cx, cx < 0 ? 1 : 0); cr.z *= 2.0; cubeRes = cr; }");
        v = "cubeRes"; break;
    }
    default: v = "float4(0, 0, 0, 0)"; break; // cube y otros: pendiente
    }
    if (vclamp) v = "saturate(" + v + ")";
    // --- escalar ---
    std::string sa = ScalarComp(w, 3), sb = ScalarComp(w, 0), s;
    switch (sop)
    {
    case 0: s = sa + " + " + sb; break;
    case 1: s = sa + " + ps"; break;
    case 2: s = g_mulZero ? "XMulS(" + sa + ", " + sb + ")" : sa + " * " + sb; break;
    case 3: case 4: s = g_mulZero ? "XMulS(" + sa + ", ps)" : sa + " * ps"; break;
    case 5: s = "max(" + sa + ", " + sb + ")"; break;
    case 6: s = "min(" + sa + ", " + sb + ")"; break;
    case 7: s = "(" + sa + " == 0.0 ? 1.0 : 0.0)"; break;
    case 8: s = "(" + sa + " > 0.0 ? 1.0 : 0.0)"; break;
    case 9: s = "(" + sa + " >= 0.0 ? 1.0 : 0.0)"; break;
    case 10: s = "(" + sa + " != 0.0 ? 1.0 : 0.0)"; break;
    case 11: s = "frac(" + sa + ")"; break;
    case 12: s = "trunc(" + sa + ")"; break;
    case 13: s = "floor(" + sa + ")"; break;
    case 14: s = "exp2(" + sa + ")"; break;
    // Sesion 24 (como Xenia): logc, rcpc y rsqc saturan +-inf a +-FLT_MAX; rcpf y rsqf los convierten en 0
    case 15: s = "XClampInf(log2(" + sa + "), 3.402823466e+38)"; break;
    case 16: s = "log2(" + sa + ")"; break;
    case 17: s = "XClampInf(1.0 / " + sa + ", 3.402823466e+38)"; break;
    case 18: s = "XClampInf(1.0 / " + sa + ", 0.0)"; break;
    case 19: s = "(1.0 / " + sa + ")"; break;
    case 20: s = "XClampInf(rsqrt(" + sa + "), 3.402823466e+38)"; break;
    case 21: s = "XClampInf(rsqrt(" + sa + "), 0.0)"; break;
    case 22: s = "rsqrt(" + sa + ")"; break;
    case 23: a0Expr = "(int)clamp(floor(" + sa + " + 0.5), -256.0, 255.0)"; s = "max(" + sa + ", " + sb + ")"; break;
    case 24: a0Expr = "(int)clamp(floor(" + sa + "), -256.0, 255.0)"; s = "max(" + sa + ", " + sb + ")"; break;
    case 25: s = sa + " - " + sb; break;
    case 26: s = sa + " - ps"; break;
    case 27: g.L("p0 = (%s == 0.0);", sa.c_str()); s = "(p0 ? 0.0 : 1.0)"; break;
    case 28: g.L("p0 = (%s != 0.0);", sa.c_str()); s = "(p0 ? 0.0 : 1.0)"; break;
    case 29: g.L("p0 = (%s > 0.0);", sa.c_str()); s = "(p0 ? 0.0 : 1.0)"; break;
    case 30: g.L("p0 = (%s >= 0.0);", sa.c_str()); s = "(p0 ? 0.0 : 1.0)"; break;
    case 31: g.L("p0 = (%s == 1.0);", sa.c_str()); s = "(p0 ? 0.0 : (" + sa + " == 0.0 ? 1.0 : " + sa + "))"; break;
    case 32: g.L("p0 = (%s - 1.0 <= 0.0);", sa.c_str()); s = "(p0 ? 0.0 : " + sa + " - 1.0)"; break;
    case 33: g.L("p0 = false;"); s = "3.402823466e+38"; break;
    case 34: g.L("p0 = (%s == 0.0);", sa.c_str()); s = sa; break;
    case 35: case 36: case 37: case 38: case 39:
    {
        static const char* cmp[] = { "== 0.0", "> 0.0", ">= 0.0", "!= 0.0", "== 1.0" };
        if (isPs) g.L("if (%s %s) discard;", sa.c_str(), cmp[sop - 35]);
        s = "(" + sa + " " + cmp[sop - 35] + " ? 1.0 : 0.0)"; break;
    }
    case 40: s = "sqrt(" + sa + ")"; break;
    case 42: case 43: case 44: case 45: case 46: case 47:
    {
        uint32_t treg = (sop & 1) | (Bits(w[2], 29, 1) << 1) | (Bits(w[1], 0, 8) & 0x3C);
        uint32_t swz = Bits(w[1], 0, 8);
        std::string k = "C[" + std::to_string(Bits(w[2], 0, 8)) + "]." + kComp[(Bits(swz, 6, 2) + 3) & 3];
        std::string t = "r[" + std::to_string(treg) + "]." + kComp[(Bits(swz, 0, 2) + 0) & 3];
        if (Bits(w[1], 24, 1)) { k = "(-" + k + ")"; t = "(-" + t + ")"; }
        const char* op = sop <= 43 ? " * " : sop <= 45 ? " + " : " - ";
        s = (sop <= 43 && g_mulZero) ? "XMulS(" + k + ", " + t + ")" : k + op + t; break;
    }
    case 48: s = "sin(" + sa + ")"; break;
    case 49: s = "cos(" + sa + ")"; break;
    default: s = "ps"; break; // retain_prev y desconocidos
    }
    if (sclamp) s = "saturate(" + s + ")";
    if (!a0Expr.empty()) { if (a0Early) g.L("a0 = %s;", a0Expr.c_str()); else g.L("{ int a0next = %s;", a0Expr.c_str()); }
    g.L("{ float4 vr = %s; float sr = %s;", v.c_str(), s.c_str());
    // --- escritura ---
    if (exportData)
    {
        bool valid; std::string t = ExportTarget(vdst, isPs, valid);
        for (int i = 0; i < 4; i++)
        {
            bool vm = vmask & (1 << i), sm = smask & (1 << i);
            if (vm && sm) g.L("%s.%c = 1.0;", t.c_str(), kComp[i]);
            else if (vm) g.L("%s.%c = vr.%c;", t.c_str(), kComp[i], kComp[i]);
            else if (sm) g.L("%s.%c = sr;", t.c_str(), kComp[i]);
            else if (Bits(w[0], 14, 1)) g.L("%s.%c = 0.0;", t.c_str(), kComp[i]);
        }
    }
    else
    {
        // Sesion 24: destinos relativos a aL (vector_dest_rel bit 6, scalar_dest_rel bit 14)
        std::string vd = Bits(w[0], 6, 1) ? "r[clamp(aL + " + std::to_string(vdst) + ", 0, 63)]" : "r[" + std::to_string(vdst) + "]";
        std::string sd = Bits(w[0], 14, 1) ? "r[clamp(aL + " + std::to_string(sdst) + ", 0, 63)]" : "r[" + std::to_string(sdst) + "]";
        for (int i = 0; i < 4; i++) if (vmask & (1 << i)) g.L("%s.%c = vr.%c;", vd.c_str(), kComp[i], kComp[i]);
        for (int i = 0; i < 4; i++) if (smask & (1 << i)) g.L("%s.%c = sr;", sd.c_str(), kComp[i]);
    }
    if (sop != 50) g.L("ps = sr;");
    g.L("}");
    if (!a0Expr.empty() && !a0Early) g.L("a0 = a0next; }");
    if (pred) g.L("}");
}

// Lectura de un componente de vertice segun formato
void EmitVertexFetch(Gen& g, const uint32_t* w, TranslatedShader& ts, bool& haveIndex)
{
    uint32_t src = Bits(w[0], 5, 6), dst = Bits(w[0], 12, 6), cidx = Bits(w[0], 20, 5), csel = Bits(w[0], 25, 2);
    uint32_t srcSwz = Bits(w[0], 30, 2), dstSwz = Bits(w[1], 0, 12);
    bool isSigned = Bits(w[1], 12, 1), isInt = Bits(w[1], 13, 1), rounded = Bits(w[1], 15, 1);
    uint32_t fmt = Bits(w[1], 16, 6), mini = Bits(w[1], 30, 1);
    bool pred = Bits(w[1], 31, 1), predCond = Bits(w[2], 31, 1);
    uint32_t stride = Bits(w[2], 0, 8) * 4, offset = Bits(w[2], 8, 23) * 4;
    uint32_t vf = cidx * 3 + csel;
    auto it = std::find(ts.vertexFetchSlots.begin(), ts.vertexFetchSlots.end(), vf);
    uint32_t slot = uint32_t(it - ts.vertexFetchSlots.begin());
    if (it == ts.vertexFetchSlots.end()) { if (slot >= 8) { ts.error = "demasiados streams"; return; } ts.vertexFetchSlots.push_back(vf); }
    if (pred) g.L("if (p0 == %s) {", predCond ? "true" : "false");
    if (!mini)
    {
        g.L("vfIndex = (uint)%s(r[%u].%c); vfStride = %uu;", rounded ? "round" : "floor", src, kComp[srcSwz], stride);
        haveIndex = true;
    }
    g.L("{ uint vbase = XFetch[%u].x + vfIndex * vfStride + %uu; uint ve = XFetch[%u].y; float4 f = float4(0, 0, 0, 1);", slot, offset, slot);
    switch (fmt)
    {
    case 36: g.L("f.x = asfloat(XSwap(XVB.Load(vbase), ve));"); break;
    case 37: g.L("f.x = asfloat(XSwap(XVB.Load(vbase), ve)); f.y = asfloat(XSwap(XVB.Load(vbase + 4), ve));"); break;
    case 57: g.L("f.xyz = float3(asfloat(XSwap(XVB.Load(vbase), ve)), asfloat(XSwap(XVB.Load(vbase + 4), ve)), asfloat(XSwap(XVB.Load(vbase + 8), ve)));"); break;
    case 38: g.L("f = float4(asfloat(XSwap(XVB.Load(vbase), ve)), asfloat(XSwap(XVB.Load(vbase + 4), ve)), asfloat(XSwap(XVB.Load(vbase + 8), ve)), asfloat(XSwap(XVB.Load(vbase + 12), ve)));"); break;
    case 6:
        g.L("uint p = XSwap(XVB.Load(vbase), ve); uint4 u = uint4(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24);");
        if (isInt) g.L("f = %s;", isSigned ? "float4(int4(u << 24) >> 24)" : "float4(u)");
        else g.L("f = %s;", isSigned ? "max(float4(int4(u << 24) >> 24) / 127.0, -1.0)" : "float4(u) / 255.0");
        break;
    case 25: case 26:
        g.L("uint p0w = XSwap(XVB.Load(vbase), ve); uint p1w = %s;", fmt == 26 ? "XSwap(XVB.Load(vbase + 4), ve)" : "0");
        g.L("uint4 u = uint4(p0w & 0xFFFF, p0w >> 16, p1w & 0xFFFF, p1w >> 16);");
        if (isInt) g.L("f = %s;", isSigned ? "float4(int4(u << 16) >> 16)" : "float4(u)");
        else g.L("f = %s;", isSigned ? "max(float4(int4(u << 16) >> 16) / 32767.0, -1.0)" : "float4(u) / 65535.0");
        if (fmt == 25) g.L("f.zw = float2(0, 1);");
        break;
    case 31: case 32:
        g.L("uint p0w = XSwap(XVB.Load(vbase), ve); uint p1w = %s;", fmt == 32 ? "XSwap(XVB.Load(vbase + 4), ve)" : "0x3C000000u");
        g.L("f = float4(f16tof32(p0w & 0xFFFF), f16tof32(p0w >> 16), f16tof32(p1w & 0xFFFF), f16tof32(p1w >> 16));");
        if (fmt == 31) g.L("f.zw = float2(0, 1);");
        break;
    // Sesion 21: formatos empaquetados (normales/tangentes). Anchos por componente x, y, z, w (bits desde el menos significativo).
    case 7: case 16: case 17:
    {
        int wx = fmt == 7 ? 10 : fmt == 16 ? 11 : 10, wy = fmt == 7 ? 10 : 11, wz = fmt == 7 ? 10 : fmt == 16 ? 10 : 11, ww = fmt == 7 ? 2 : 0;
        g.L("uint p = XSwap(XVB.Load(vbase), ve);");
        g.L("uint4 u = uint4(p & %uu, (p >> %d) & %uu, (p >> %d) & %uu, %s);", (1u << wx) - 1, wx, (1u << wy) - 1, wx + wy, (1u << wz) - 1,
            ww ? "p >> 30" : "0u");
        if (isSigned)
        {
            g.L("int4 s = int4(u << uint4(%d, %d, %d, %d)) >> int4(%d, %d, %d, %d);", 32 - wx, 32 - wy, 32 - wz, ww ? 30 : 0, 32 - wx, 32 - wy, 32 - wz, ww ? 30 : 0);
            if (isInt) g.L("f = float4(s);");
            else g.L("f = max(float4(s) / float4(%d.0, %d.0, %d.0, %d.0), -1.0);", (1 << (wx - 1)) - 1, (1 << (wy - 1)) - 1, (1 << (wz - 1)) - 1, ww ? 1 : 1);
        }
        else
        {
            if (isInt) g.L("f = float4(u);");
            else g.L("f = float4(u) / float4(%d.0, %d.0, %d.0, %d.0);", (1 << wx) - 1, (1 << wy) - 1, (1 << wz) - 1, ww ? 3 : 1);
        }
        if (!ww) g.L("f.w = 1.0;");
        break;
    }
    default:
    {
        static std::set<uint32_t> seen;
        if (seen.insert(fmt).second) LOG("[shader] formato de vertice %u sin soporte", fmt);
        g.L("/* formato de vertice %u sin soporte */", fmt);
        break;
    }
    }
    // exp_adjust (dword1 bits 24-29, con signo): escala por 2^n los formatos de punto fijo
    int expAdj = (int)Bits(w[1], 24, 6); if (expAdj & 0x20) expAdj -= 64;
    if (expAdj && fmt != 36 && fmt != 37 && fmt != 38 && fmt != 57 && fmt != 31 && fmt != 32) g.L("f *= %g;", ldexp(1.0, expAdj));
    for (int i = 0; i < 4; i++)
    {
        uint32_t s = Bits(dstSwz, 3 * i, 3);
        if (s < 4) g.L("r[%u].%c = f.%c;", dst, kComp[i], kComp[s]);
        else if (s == 4) g.L("r[%u].%c = 0.0;", dst, kComp[i]);
        else if (s == 5) g.L("r[%u].%c = 1.0;", dst, kComp[i]);
    }
    g.L("}");
    if (pred) g.L("}");
}

static bool g_curIsPs = false;
void EmitTextureFetch(Gen& g, const uint32_t* w, TranslatedShader& ts)
{
    uint32_t op = Bits(w[0], 0, 5);
    uint32_t src = Bits(w[0], 5, 6), dst = Bits(w[0], 12, 6), tf = Bits(w[0], 20, 5), srcSwz = Bits(w[0], 26, 6);
    uint32_t dstSwz = Bits(w[1], 0, 12);
    if (op != 1) { g.L("/* fetch op %u no soportado */", op); return; }
    tf &= 15;   // de momento 16 huecos por dimension
    uint32_t dim = Bits(w[2], 14, 2);   // 0 = 1D, 1 = 2D, 2 = 3D, 3 = cubo (sesion 21)
    if (dim == 3 || dim == 0)
    {
        static int logged = 0;
        if (logged++ < 20) LOG("[shader] tfetch con dimension %s en tf%u (sin soporte: se lee como 2D)", dim == 3 ? "cubo" : "1D", tf);
    }
    if (dim == 3)
    {
        ts.textureCubeMask |= 1u << tf;
        g.L("{ float4 f = XTexC[%u].SampleLevel(XSamp, cubeDir, 0);", tf);
    }
    else if (dim == 2)
    {
        // Textura de volumen (p. ej. la tabla 3D de correccion de color)
        ts.texture3DMask |= 1u << tf;
        g.L("{ float3 uvw = float3(r[%u].%c, r[%u].%c, r[%u].%c);", src, kComp[Bits(srcSwz, 0, 2)], src, kComp[Bits(srcSwz, 2, 2)],
            src, kComp[Bits(srcSwz, 4, 2)]);
        g.L("float3 tsz3; XTex3[%u].GetDimensions(tsz3.x, tsz3.y, tsz3.z); uint4 tm = XTexMode[%u]; bool brd = false;", tf, tf);
        g.L("uvw = float3(XAddr(uvw.x, tm.x & 7u, tsz3.x, brd), XAddr(uvw.y, (tm.x >> 3) & 7u, tsz3.y, brd), XAddr(uvw.z, (tm.x >> 6) & 7u, tsz3.z, brd));");
        g.L("float4 f = XTex3[%u].SampleLevel(XSamp, uvw, 0); if (brd) f = XBorder(tm.w);", tf);
    }
    else
    {
        ts.textureMask |= 1u << tf;
        // Sesion 24: tx_coord_denorm (w0 bit 25) = coordenadas en texeles; offset_x/offset_y (w2 bits 16-20 / 21-25, con signo,
        // en medios texeles). Los usa p. ej. la restauracion de color/profundidad del frame anterior (coordenadas en pixeles).
        static bool noDenorm = getenv("GH6_NODENORM") != nullptr;
        bool denorm = !noDenorm && Bits(w[0], 25, 1);
        int ox = (int)(Bits(w[2], 16, 5) << 27) >> 27, oy = (int)(Bits(w[2], 21, 5) << 27) >> 27;
        if (noDenorm) ox = oy = 0;
        g.L("{ float2 uv = float2(r[%u].%c, r[%u].%c);", src, kComp[Bits(srcSwz, 0, 2)], src, kComp[Bits(srcSwz, 2, 2)]);
        g.L("float2 tsz; XTex[%u].GetDimensions(tsz.x, tsz.y);", tf);
        if (ox || oy) g.L("uv += float2(%d, %d) * 0.5 / (%s);", ox, oy, denorm ? "float2(1, 1)" : "tsz");
        // Sesion 24: coordenadas en texeles enteras (p. ej. posicion de pixel de PsParamGen) caen en la esquina entre 4 texeles; con
        // filtro lineal mezclaban la profundidad empaquetada que restaura C316F389 y la luz de los focos fallaba a rayas en los
        // personajes. Se lee el centro del texel (como el texel que elegiria el filtro de punto). GH6_NODENORMCENTER=1 = antes.
        static bool noDenormCenter = getenv("GH6_NODENORMCENTER") != nullptr;
        // Sesion 25: con resolucion interna las texturas resueltas son Rs veces mayores; XTexMode.z bits 8-11 = ese factor (0/1 = normal).
        // Solo se emite con GH6_RESOLUCION > 1 / panoramico para no cambiar el HLSL (ni la cache de shaders) a 720p.
        static const bool resScaled = getenv("GH6_RESOLUCION") && atoi(getenv("GH6_RESOLUCION")) > 1;
        static const bool panoT = getenv("GH6_PANORAMICO") && *getenv("GH6_PANORAMICO") && _stricmp(getenv("GH6_PANORAMICO"), "16:9") != 0;
        if (denorm && (resScaled || panoT))
        {
            // La posicion de pixel llega como pos / escala: entero = pixel del juego (los calculos enteros del shader no cambian) y
            // fraccion = subpixel. Textura escalada: se lee el subpixel exacto (o el centro del primero si la coordenada es entera);
            // textura normal: el centro del texel, como antes.
            if (panoT) g.L("float2 tsc = float2(((XTexMode[%u].z >> 8) & 255u) ? ((XTexMode[%u].z >> 8) & 255u) / 16.0 : 1.0, ((XTexMode[%u].z >> 16) & 255u) ? ((XTexMode[%u].z >> 16) & 255u) / 16.0 : 1.0);", tf, tf, tf, tf);
            else g.L("float2 tsc = (float)max(1u, (XTexMode[%u].z >> 8) & 15u);", tf);
            if (!ox && !oy && !noDenormCenter) g.L("{ float2 fD = frac(uv); uv = (tsc > 1.0) ? floor(uv) + (fD > 0.0 ? fD : 0.5 / tsc) : floor(uv) + 0.5; }");
            g.L("uv /= tsz / tsc;");
        }
        else
        {
            if (denorm && !ox && !oy && !noDenormCenter) g.L("uv += 0.5;");
            if (denorm) g.L("uv /= tsz;");
        }
        // Sesion 24: modo de borde y filtro de la constante de fetch (o de la instruccion: 0 punto, 1 lineal, 3 = constante)
        uint32_t magF = Bits(w[1], 12, 2), mipF = Bits(w[1], 16, 2);
        g.L("uint4 tm = XTexMode[%u]; bool brd = false;", tf);
        // Sesion 24: mipmaps en el PS con gradientes de la coordenada sin envolver (salvo "basemap" o retraduccion sin gradientes)
        static bool noMipsS = getenv("GH6_NOMIPS") != nullptr;
        bool grad = g_curIsPs && !g_xlatNoGrad && !noMipsS && mipF != 2;
        if (grad) g.L("float2 gdx = ddx(uv), gdy = ddy(uv); if (((tm.z >> 2) & 3u) == 2u) { gdx = 0; gdy = 0; }");
        g.L("uv = float2(XAddr(uv.x, tm.x & 7u, tsz.x, brd), XAddr(uv.y, (tm.x >> 3) & 7u, tsz.y, brd));");
        auto smp = [&](const char* sm) { char b[160]; if (grad) snprintf(b, sizeof(b), "XTex[%u].SampleGrad(%s, uv, gdx, gdy)", tf, sm); else snprintf(b, sizeof(b), "XTex[%u].SampleLevel(%s, uv, 0)", tf, sm); return std::string(b); };
        if (magF == 0) g.L("float4 f = %s;", smp("XSampP").c_str());
        else if (magF == 3) g.L("float4 f = (tm.y == 0u) ? %s : %s;", smp("XSampP").c_str(), smp("XSamp").c_str());
        else g.L("float4 f = %s;", smp("XSamp").c_str());
        g.L("if (brd) f = XBorder(tm.w);");
    }
    static bool noFetchPred = getenv("GH6_NOFETCHPRED") != nullptr;
    bool fpred = !noFetchPred && Bits(w[1], 31, 1);
    if (fpred) g.L("if (p0 == %s) {", Bits(w[2], 31, 1) ? "true" : "false");
    for (int i = 0; i < 4; i++)
    {
        uint32_t s = Bits(dstSwz, 3 * i, 3);
        if (s < 4) g.L("r[%u].%c = f.%c;", dst, kComp[i], kComp[s]);
        else if (s == 4) g.L("r[%u].%c = 0.0;", dst, kComp[i]);
        else if (s == 5) g.L("r[%u].%c = 1.0;", dst, kComp[i]);
    }
    if (fpred) g.L("}");
    g.L("}");
}
} // namespace

TranslatedShader TranslateXenosShader(uint32_t type, const uint32_t* words, uint32_t count)
{
    TranslatedShader ts;
    bool isPs = type == 1;
    g_curIsPs = isPs;
    Gen g;
    // Control de flujo: pares de 48 bits en grupos de 3 dwords
    struct Cf { uint32_t op, a, b; };
    std::vector<Cf> cfs;
    bool end = false;
    for (uint32_t i = 0; !end && i + 3 <= count; i += 3)
    {
        uint32_t d0 = words[i], d1 = words[i + 1], d2 = words[i + 2];
        Cf pair[2] = { { Bits(d1, 12, 4), d0, d1 & 0xFFFF }, { Bits(d2 >> 16, 12, 4), (d1 >> 16) | (d2 << 16), d2 >> 16 } };
        for (auto& c : pair) { cfs.push_back(c); if (c.op == 2 || c.op == 4 || c.op == 6 || c.op == 14) end = true; }
    }
    bool haveIndex = false;
    uint32_t loopDepth = 0;
    for (auto& c : cfs)
    {
        switch (c.op)
        {
        case 0: case 12: case 15: break; // nop, alloc, mark_vs_fetch_done
        case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14:
        {
            uint32_t addr = Bits(c.a, 0, 12), n = Bits(c.a, 12, 3), seq = Bits(c.a, 16, 12);
            bool cond = false;
            if (c.op == 3 || c.op == 4) { g.L("if (XBool(%u) == %s) {", Bits(c.b, 2, 8), Bits(c.b, 10, 1) ? "true" : "false"); cond = true; }
            else if (c.op == 5 || c.op == 6 || c.op == 13 || c.op == 14) { g.L("if (p0 == %s) {", Bits(c.b, 10, 1) ? "true" : "false"); cond = true; }
            for (uint32_t k = 0; k < n; k++)
            {
                uint32_t at = (addr + k) * 3;
                if (at + 3 > count) { ts.error = "instruccion fuera de rango"; return ts; }
                const uint32_t* w = words + at;
                if (Bits(seq, 2 * k, 1))
                {
                    if (Bits(w[0], 0, 5) == 0) { if (isPs) { ts.error = "vfetch en PS"; return ts; } EmitVertexFetch(g, w, ts, haveIndex); }
                    else EmitTextureFetch(g, w, ts);
                }
                else EmitAlu(g, w, isPs);
                if (!ts.error.empty()) return ts;
            }
            if (cond) g.L("}");
            break;
        }
        case 7:   // LOOP_START (sesion 24): aL = inicio; repetir "cuenta" veces; constante de bucle 0x4908 + id
        {
            uint32_t id = Bits(c.a, 16, 5);
            g.L("{ uint lc%u = XLoop[%u][%u]; int lstep%u = int(lc%u << 8) >> 24; aL = int((lc%u >> 8) & 0xFF);", loopDepth, id / 4, id % 4, loopDepth, loopDepth, loopDepth);
            g.L("[loop] for (uint li%u = 0; li%u < (lc%u & 0xFFu); li%u++) {", loopDepth, loopDepth, loopDepth, loopDepth);
            loopDepth++;
            break;
        }
        case 8:   // LOOP_END: (ruptura predicada opcional) aL += paso
        {
            if (loopDepth == 0) { ts.error = "LOOP_END sin LOOP_START"; return ts; }
            loopDepth--;
            if (Bits(c.a, 21, 1)) g.L("if (p0 == %s) break;", Bits(c.b, 10, 1) ? "true" : "false");
            g.L("aL += lstep%u; } }", loopDepth);
            break;
        }
        default:
            ts.error = "control de flujo no soportado (op " + std::to_string(c.op) + ")";
            return ts;
        }
    }
    std::string h = XenosInterface();
    static const bool pano = getenv("GH6_PANORAMICO") && *getenv("GH6_PANORAMICO") && _stricmp(getenv("GH6_PANORAMICO"), "16:9") != 0;
    if (!isPs)
    {
        static const bool panoV = getenv("GH6_PANORAMICO") && *getenv("GH6_PANORAMICO") && _stricmp(getenv("GH6_PANORAMICO"), "16:9") != 0;
        h += "XVSOut main(uint vid : SV_VertexID)\n{\n    XVSOut o = (XVSOut)0; float4 r[64]; float ps = 0; bool p0 = false; int a0 = 0; int aL = 0; float3 cubeDir = float3(0, 0, 1); float4 cubeRes = 0; uint vfIndex = vid; uint vfStride = 0;\n";
        h += "    [unroll] for (int i = 0; i < 64; i++) r[i] = float4(0, 0, 0, 0);\n    r[0].x = (float)vid;\n";
        h += g.out;
        // Transformacion de viewport de Xenos (PA_CL_VTE_CNTL) -> NDC del render target
        h += R"(    uint vte = (uint)XVte.x; float4 p = o.pos;
    if ((vte & 0x100u) == 0 && p.w != 0) p.xy /= p.w;       // VTX_XY_FMT = 0: dividir por w
    if ((vte & 0x200u) == 0 && p.w != 0) p.z /= p.w;
    if (vte & 1u) p.x = p.x * XVport0.x; if (vte & 2u) p.x += XVport0.y;
    if (vte & 4u) p.y = p.y * XVport0.z; if (vte & 8u) p.y += XVport0.w;
    if (vte & 16u) p.z = p.z * XVport1.x; if (vte & 32u) p.z += XVport1.y;
    p.xy += XVport1.zw;   // PA_SC_WINDOW_OFFSET (sesion 23)
    float w = ((vte & 0x100u) == 0) ? o.pos.w : 1.0;
    PANO_UI
    float2 ndc = float2(p.x / (XVte.y * 0.5) - 1.0, 1.0 - p.y / (XVte.z * 0.5));
    o.pos = float4(ndc * w, saturate(p.z) * w, w);
    return o;
}
)";
        {   // Sesion 25 (panoramico): la interfaz 2D (w = 1, coordenadas de pantalla) se comprime hacia el centro para no estirarse con la
            // superficie; los vertices en los bordes (pases a pantalla completa, fondos) se quedan donde estan.
            size_t q = h.find("    PANO_UI\n");
            if (q != std::string::npos)
                h.replace(q, 12, panoV ? "    o.pano = 1.0;\n" : "");   // sesion 25: la compresion la hace el GS; sesion 27: marca de oscurecer
                (void)panoV;
        }
    }
    else
    {
        ts.writesDepth = g.out.find("oDepthV") != std::string::npos;
        ts.mrtMask = 0;
        for (int m = 1; m < 4; m++) if (g.out.find("oC" + std::to_string(m) + ".") != std::string::npos) ts.mrtMask |= 1u << m;
        {
            std::string sig = "float4 main(XVSOut i, bool xFront : SV_IsFrontFace";
            for (int m = 1; m < 4; m++) if (ts.mrtMask & (1u << m)) sig += ", out float4 oT" + std::to_string(m) + " : SV_Target" + std::to_string(m);
            if (ts.writesDepth) sig += ", out float zOut : SV_Depth";
            sig += ") : SV_Target0\n{\n";
            if (ts.writesDepth) sig += "    float4 oDepthV = 0;\n";
            h += sig;
        }
        h += "    float4 r[64]; float ps = 0; bool p0 = false; int a0 = 0; int aL = 0; float3 cubeDir = float3(0, 0, 1); float4 cubeRes = 0; float4 oC0 = float4(0, 0, 0, 1); float4 oC1 = 0, oC2 = 0, oC3 = 0; float4 oDummy = 0;\n";
        h += "    [unroll] for (int k = 0; k < 16; k++) r[k] = i.t[k]; [unroll] for (int k2 = 16; k2 < 64; k2++) r[k2] = float4(0, 0, 0, 0);\n";
        // Sesion 24: PsParamGen (SQ_PROGRAM_CNTL bit 18, SQ_CONTEXT_MISC.param_gen_pos), como Xenia: XY = pixel (entero) con el signo de X
        // = cara trasera; Z, W = 0. Lo usan los shaders que leen texturas por posicion de pixel (restauracion de la escena anterior).
        // Sesion 25: con resolucion interna, posicion con fraccion (pos / escala - 0.5): la lectura en texeles con centro (+0.5) cae
        // justo en el pixel correspondiente de la textura grande. Con floor, la profundidad del frame anterior se restauraba en bloques
        // de 2x2 y al redibujar los personajes la prueba LEQUAL fallaba a filas/columnas alternas (rayado fino en caras y piel).
        static const bool resScaledPg = getenv("GH6_RESOLUCION") && atoi(getenv("GH6_RESOLUCION")) > 1 && !getenv("GH6_PGENFLOOR");
        static const bool panoP = getenv("GH6_PANORAMICO") && *getenv("GH6_PANORAMICO") && _stricmp(getenv("GH6_PANORAMICO"), "16:9") != 0;
        // Sesion 25 (lineas verticales cada 80 pixeles): con pos / escala - 0.5, el entero de la posicion caia en el pixel anterior en
        // la primera columna de cada pixel del juego; los shaders del juego que calculan baldosas de la EDRAM con la posicion entera
        // fallaban en cada borde de 80 pixeles. Ahora pp = pos / escala: floor(pp) = pixel del juego, la fraccion = subpixel.
        if (panoP)
            h += "    if (XAlpha.z >= 0) { float2 pp = i.pos.xy / max(XScreen.xy, 1.0);\n"
                 "        r[(int)XAlpha.z] = float4(xFront ? pp.x : -pp.x, pp.y, 0, 0); }\n";
        else if (resScaledPg)
            h += "    if (XAlpha.z >= 0) { uint xm = (uint)XAlpha.w; bool pfl = xm >= 64u; xm &= 63u; float2 pq = i.pos.xy / float2(max(xm >> 3, 1u), max(xm & 7u, 1u));\n"
                 "        float2 pp = pfl ? floor(pq) : pq;\n"
                 "        r[(int)XAlpha.z] = float4(xFront ? pp.x : -pp.x, pp.y, 0, 0); }\n";
        else
        h += "    if (XAlpha.z >= 0) { uint xm = (uint)XAlpha.w; float2 pp = floor(i.pos.xy / float2(max(xm >> 3, 1u), max(xm & 7u, 1u)));\n"
             "        r[(int)XAlpha.z] = float4(xFront ? pp.x : -pp.x, pp.y, 0, 0); }\n";
        h += g.out;
        if (ts.writesDepth) h += "    zOut = saturate(oDepthV.x);\n";
        for (int m = 1; m < 4; m++) if (ts.mrtMask & (1u << m)) h += "    oT" + std::to_string(m) + " = oC" + std::to_string(m) + ";\n";
        // XVte.w: escala de salida del render target (1/32 en superficies 16_16 de Xenos, rango -32..32 -> SNORM). 0 = sin escala.
        // Prueba de alfa (RB_COLORCONTROL bits 0-3, RB_ALPHA_REF), como Xenia: se descarta el pixel si no la pasa (sesion 24)
        h += "    if (XAlpha.x < 7.5) { uint af = (uint)XAlpha.x; float aa = oC0.w, ar = XAlpha.y;\n"
             "        bool ap = af == 1 ? aa < ar : af == 2 ? aa == ar : af == 3 ? aa <= ar : af == 4 ? aa > ar : af == 5 ? aa != ar : af == 6 ? aa >= ar : false;\n"
             "        if (!ap) discard; }\n";
        if (pano) h += "    oC0.rgb *= i.pano;\n";   // sesion 27: franjas laterales del panoramico en negro (0) o sin cambios (1)
        h += "    return XVte.w != 0 ? oC0 * XVte.w : oC0;\n}\n";
    }
    ts.hlsl = h;
    ts.ok = true;
    return ts;
}

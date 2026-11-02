"""Anade a XenonRecomp/recompiler.cpp las instrucciones que usa GH:WoR y que faltan.
Idempotente: si ya esta aplicado no hace nada. python -I patch_recompiler.py RUTA/recompiler.cpp"""
import sys

MARK = "// ---- GH:WoR instrucciones anadidas ----"

D_LOAD = """    case PPC_INST_{N}:
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\\t{{}}.{T} = {LOAD}({{}});", r(insn.operands[0]), ea());
        println("\\t{{}}.u32 = {{}};", r(insn.operands[2]), ea());
        break;
"""
X_LOAD = """    case PPC_INST_{N}:
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\\t{{}}.{T} = {LOAD}({{}});", r(insn.operands[0]), ea());
        println("\\t{{}}.u32 = {{}};", r(insn.operands[1]), ea());
        break;
"""
D_STORE = """    case PPC_INST_{N}:
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\\t{STORE}({{}}, {{}}.{T});", ea(), r(insn.operands[0]));
        println("\\t{{}}.u32 = {{}};", r(insn.operands[2]), ea());
        break;
"""
X_STORE = """    case PPC_INST_{N}:
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\\t{STORE}({{}}, {{}}.{T});", ea(), r(insn.operands[0]));
        println("\\t{{}}.u32 = {{}};", r(insn.operands[1]), ea());
        break;
"""

blocks = [MARK]
blocks.append(D_LOAD.format(N="LHZU", T="u64", LOAD="PPC_LOAD_U16"))
blocks.append(X_LOAD.format(N="LHZUX", T="u64", LOAD="PPC_LOAD_U16"))
blocks.append(D_LOAD.format(N="LHAU", T="s64", LOAD="(int16_t)PPC_LOAD_U16"))
blocks.append(X_LOAD.format(N="LBZUX", T="u64", LOAD="PPC_LOAD_U8"))
blocks.append(X_LOAD.format(N="LWZUX", T="u64", LOAD="PPC_LOAD_U32"))
blocks.append(D_STORE.format(N="STHU", T="u16", STORE="PPC_STORE_U16"))
blocks.append(X_STORE.format(N="STHUX", T="u16", STORE="PPC_STORE_U16"))
blocks.append(X_STORE.format(N="STBUX", T="u8", STORE="PPC_STORE_U8"))
blocks.append(X_STORE.format(N="STDUX", T="u64", STORE="PPC_STORE_U64"))

# coma flotante con update
blocks.append("""    case PPC_INST_LFSU:
    case PPC_INST_LFSUX:
        printSetFlushMode(false);
        if (id == PPC_INST_LFSU)
            println("\\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        else
            println("\\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
        println("\\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp());
        println("\\t{}.u32 = {};", r(insn.operands[id == PPC_INST_LFSU ? 2 : 1]), ea());
        break;

    case PPC_INST_STFSU:
    case PPC_INST_STFSUX:
        printSetFlushMode(false);
        if (id == PPC_INST_STFSU)
            println("\\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        else
            println("\\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2]));
        println("\\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0]));
        println("\\tPPC_STORE_U32({}, {}.u32);", ea(), temp());
        println("\\t{}.u32 = {};", r(insn.operands[id == PPC_INST_STFSU ? 2 : 1]), ea());
        break;

    case PPC_INST_LFDU:
        printSetFlushMode(false);
        println("\\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\\t{}.u64 = PPC_LOAD_U64({});", f(insn.operands[0]), ea());
        println("\\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;

    case PPC_INST_STFDU:
        printSetFlushMode(false);
        println("\\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2]));
        println("\\tPPC_STORE_U64({}, {}.u64);", ea(), f(insn.operands[0]));
        println("\\t{}.u32 = {};", r(insn.operands[2]), ea());
        break;

    case PPC_INST_LHBRX:
        print("\\t{}.u64 = __builtin_bswap16(PPC_LOAD_U16(", r(insn.operands[0]));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32));", r(insn.operands[2]));
        break;

    case PPC_INST_DCBST:
        // no op (cache)
        break;
""")

# ramas con CTR
blocks.append("""    case PPC_INST_BDZF:
    case PPC_INST_BDNZT:
    {
        static const char* crBits[] = { "lt", "gt", "eq", "so" };
        println("\\t--{}.u64;", ctr());
        if (id == PPC_INST_BDZF)
            print("\\tif ({}.u32 == 0 && !{}.{}) ", ctr(), cr(insn.operands[0] / 4), crBits[insn.operands[0] % 4]);
        else
            print("\\tif ({}.u32 != 0 && {}.{}) ", ctr(), cr(insn.operands[0] / 4), crBits[insn.operands[0] % 4]);
        if (insn.operands[1] < fn.base || insn.operands[1] >= fn.base + fn.size)
        {
            // destino fuera de la funcion: llamada + return (como printConditionalBranch)
            println("{{");
            print("\\t");
            printFunctionCall(insn.operands[1]);
            println("\\t\\treturn;");
            println("\\t}}");
        }
        else
            println("goto loc_{:X};", insn.operands[1]);
        break;
    }
""")

# enteros
blocks.append("""    case PPC_INST_EQV:
        println("\\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_SUBFZE:
        println("\\t{}.s64 = ~{}.s64 + {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\\t{}.ca = {}.u32 < ~{}.u32 || ({}.ca && ~{}.u32 == 0xFFFFFFFF);", xer(), temp(), r(insn.operands[1]), xer(), r(insn.operands[1]));
        println("\\t{}.s64 = {}.s64;", r(insn.operands[0]), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDME:
        println("\\t{}.s64 = {}.s64 + {}.ca - 1;", temp(), r(insn.operands[1]), xer());
        println("\\t{}.ca = ({}.u32 != 0) || {}.ca;", xer(), r(insn.operands[1]), xer());
        println("\\t{}.s64 = {}.s64;", r(insn.operands[0]), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDC:
        println("\\t{}.ca = {}.u32 + {}.u32 < {}.u32;", xer(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]));
        println("\\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_MULHDU:
        println("\\t{}.u64 = uint64_t((unsigned __int128)({}.u64) * (unsigned __int128)({}.u64) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\\t{}.compare<int64_t>({}.s64, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;
""")

# coma flotante escalar
blocks.append("""    case PPC_INST_FRSQRTE:
        printSetFlushMode(false);
        println("\\t{}.f64 = 1.0 / sqrt({}.f64);", f(insn.operands[0]), f(insn.operands[1]));
        break;

    case PPC_INST_FNMADD:
        printSetFlushMode(false);
        println("\\t{}.f64 = -({}.f64 * {}.f64 + {}.f64);", f(insn.operands[0]), f(insn.operands[1]), f(insn.operands[2]), f(insn.operands[3]));
        break;
""")

# vectores
blocks.append("""    case PPC_INST_VANDC:
        println("\\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VSEL128:
        // vsel128 vD, vA, vB: vD = (vA & ~vD) | (vB & vD)
        println("\\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8))));", v(insn.operands[0]), v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[0]), v(insn.operands[2]));
        break;

    case PPC_INST_VPKSWSS:
    case PPC_INST_VPKSWSS128:
        println("\\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), simde_mm_load_si128((simde__m128i*){}.s32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VSUBSHS:
        println("\\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_subs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), simde_mm_load_si128((simde__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VSUBUWM:
        println("\\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*){}.u32), simde_mm_load_si128((simde__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VADDSWS:
        for (size_t i = 0; i < 4; i++)
        {
            println("\\t{}.s64 = int64_t({}.s32[{}]) + int64_t({}.s32[{}]);", temp(), v(insn.operands[1]), i, v(insn.operands[2]), i);
            println("\\t{}.s32[{}] = {}.s64 > INT32_MAX ? INT32_MAX : ({}.s64 < INT32_MIN ? INT32_MIN : int32_t({}.s64));", v(insn.operands[0]), i, temp(), temp(), temp());
        }
        break;

    case PPC_INST_VCFPUXWS128:
    {
        printSetFlushMode(true);
        double scale = double(1u << insn.operands[2]);
        for (size_t i = 0; i < 4; i++)
        {
            println("\\t{}.f64 = double({}.f32[{}]) * {};", temp(), v(insn.operands[1]), i, scale);
            println("\\t{}.u32[{}] = ({}.f64 != {}.f64 || {}.f64 <= 0.0) ? 0u : ({}.f64 >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t({}.f64));", v(insn.operands[0]), i, temp(), temp(), temp(), temp(), temp());
        }
        break;
    }
""")


def main():
    p = sys.argv[1]
    s = open(p, encoding="utf-8").read()
    if MARK in s:
        print("ya aplicado")
        return
    # LVEHX: igual que LVEWX (carga el vector alineado completo)
    s = s.replace("    case PPC_INST_LVEWX:\n", "    case PPC_INST_LVEHX:\n    case PPC_INST_LVEWX:\n", 1)
    anchor = "    case PPC_INST_BEQ:\n"
    assert anchor in s
    s = s.replace(anchor, "\n".join(blocks) + "\n" + anchor, 1)
    open(p, "w", encoding="utf-8").write(s)
    print("parche aplicado")


if __name__ == "__main__":
    main()

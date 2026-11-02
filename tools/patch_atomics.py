"""Sesion 6: lwarx/stwcx con reserva versionada (evita ABA). Idempotente."""
p = "G:/Guitar Hero Warrior of rock/Recomp/XenonRecomp/XenonRecomp/recompiler.cpp"
s = open(p, encoding="utf-8").read()
if "ppc_reserve(" in s:
    print("ya aplicado"); raise SystemExit
reps = [
 # LWARX: registrar la reserva antes de leer
 (r'''    case PPC_INST_LWARX:
        print("\t{}.u32 = *(uint32_t*)(base + ", reserved());''',
  r'''    case PPC_INST_LWARX:
        print("\tppc_reserve(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        print("\t{}.u32 = *(uint32_t*)(base + ", reserved());'''),
 (r'''    case PPC_INST_LDARX:
        print("\t{}.u64 = *(uint64_t*)(base + ", reserved());''',
  r'''    case PPC_INST_LDARX:
        print("\tppc_reserve(");
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32);", r(insn.operands[2]));
        print("\t{}.u64 = *(uint64_t*)(base + ", reserved());'''),
 # STWCX / STDCX: CAS + version de la reserva
 (r'''        print("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32), {}.s32, __builtin_bswap32({}.s32));", r(insn.operands[2]), reserved(), r(insn.operands[0]));''',
  r'''        print("\t{}.eq = ppc_stwcx32(base, ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u32, __builtin_bswap32({}.u32));", r(insn.operands[2]), reserved(), r(insn.operands[0]));'''),
 (r'''        print("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32), {}.s64, __builtin_bswap64({}.s64));", r(insn.operands[2]), reserved(), r(insn.operands[0]));''',
  r'''        print("\t{}.eq = ppc_stdcx64(base, ", cr(0));
        if (insn.operands[1] != 0)
            print("{}.u32 + ", r(insn.operands[1]));
        println("{}.u32, {}.u64, __builtin_bswap64({}.u64));", r(insn.operands[2]), reserved(), r(insn.operands[0]));'''),
]
for o, n in reps:
    assert s.count(o) == 1, o[:60]
    s = s.replace(o, n)
open(p, "w", encoding="utf-8").write(s)
print("parche aplicado")

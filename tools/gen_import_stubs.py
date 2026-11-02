"""Genera runtime/import_stubs.cpp (stubs para imports no implementados en kernel.cpp) y runtime/var_imports.inc."""
import re
R = "G:/Guitar Hero Warrior of rock/Recomp/"
impl = set()
for src in ("kernel.cpp", "threads.cpp", "fs.cpp", "gpu_stub.cpp", "xam.cpp", "xma.cpp"):
    impl |= set(re.findall(r"GUEST_IMPL\((\w+)\)", open(R + "runtime/" + src).read()))
names = [l.strip() for l in open(R + "xex/import_names.txt") if l.strip()]
out = ["// GENERADO por tools/gen_import_stubs.py - no editar a mano.",
       "// Stubs: registran la primera llamada y devuelven 0 (STATUS_SUCCESS / ERROR_SUCCESS).",
       '#include "runtime.h"', "#include <ppc_recomp_shared.h>", ""]
n = 0
for nm in names:
    if nm in impl: continue
    n += 1
    out.append('GUEST_IMPL(%s) { static bool once = false; if (!once) { once = true; LOG("[stub] %s"); } ctx.r3.u64 = 0; }' % (nm, nm))
open(R + "runtime/import_stubs.cpp", "w").write("\n".join(out) + "\n")
sizes = {"XboxHardwareInfo": 0x10, "XboxKrnlVersion": 0x8, "KeTimeStampBundle": 0x18, "ExLoadedCommandLine": 0x200}
vars_ = []
seen = {}
for l in open(R + "xex/imports_named.txt"):
    lib, o, kind, addr, name = l.rstrip("\n").split("\t")
    seen.setdefault(name, []).append((kind, addr))
inc = []
for name, ents in seen.items():
    if all(k == "var" for k, _ in ents):
        for _, addr in ents:
            inc.append('    { %s, "%s", 0x%X },' % (addr[1:], name, sizes.get(name, 0x100)))
open(R + "runtime/var_imports.inc", "w").write("\n".join(inc) + "\n")
print("stubs:", n, "implementados:", len(impl & set(names)), "no-importados en kernel.cpp:", sorted(impl - set(names)), "vars:", len(inc))

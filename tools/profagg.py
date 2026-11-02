"""Agrega las lineas [prof] de un log (GH6_PROFILE, y GH6_PROFILEALL=1 para todos los hilos) por funcion.
Uso: python profagg.py logs/runNN.log [n]"""
import subprocess, sys, os, collections
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "project", "build", "GH6.exe")
sym = os.path.join(root, "toolchain", "clang+llvm-23.1.4-x86_64-pc-windows-msvc", "bin", "llvm-symbolizer.exe")
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
threads = collections.defaultdict(list)
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    if l.startswith("[prof]") and "fin" not in l and "sin hilo" not in l:
        f = l.split()[1:]
        if f and f[0].startswith("T") and ":" in f[0]: threads[f[0]].append(f[1:])
        else: threads["GPU"].append(f)
addrs = sorted({a for ss in threads.values() for s in ss for a in s if not a.startswith("m")})
names = {}
for i in range(0, len(addrs), 400):
    chunk = addrs[i:i + 400]
    out = subprocess.run([sym, "--obj=" + exe, "--functions=short", "--no-inlines"] + ["0x" + a for a in chunk], capture_output=True, text=True).stdout.split("\n\n")
    for a, block in zip(chunk, out): names[a] = block.strip().split("\n")[0]
def nm(a): return a[1:] if a.startswith("m") else names.get(a, a)
for t, samples in sorted(threads.items(), key=lambda kv: -len(kv[1])):
    own, incl = collections.Counter(), collections.Counter()
    for s in samples:
        if not s: continue
        own[nm(s[0])] += 1
        for f in set(nm(a) for a in s): incl[f] += 1
    n = len(samples)
    busy = n - sum(c for f, c in own.items() if f in ("ntdll.dll", "KERNELBASE.dll", "win32u.dll"))
    print(f"===== hilo {t}: {n} muestras, {100*busy/max(n,1):.0f}% fuera de esperas del sistema")
    print("--- propia ---")
    for f, c in own.most_common(top): print(f"{100*c/n:5.1f}%  {f}")
    print("--- inclusiva ---")
    for f, c in incl.most_common(top): print(f"{100*c/n:5.1f}%  {f}")

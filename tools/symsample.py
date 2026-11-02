"""Traduce las lineas [sample] de un log a nombres de funcion (llvm-symbolizer).
Uso: python symsample.py logs\runNN.log [ronda]"""
import re, subprocess, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "project", "build", "GH6.exe")
sym = os.path.join(root, "toolchain", "clang+llvm-23.1.4-x86_64-pc-windows-msvc", "bin", "llvm-symbolizer.exe")
want = int(sys.argv[2]) if len(sys.argv) > 2 else 1
lines, rnd = [], 0
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    if "---- muestra" in l: rnd += 1; continue
    if "[sample] tid" in l and rnd == want: lines.append(l.strip())
addrs = sorted({a for l in lines for a in l.split("host:")[1].split()})
out = subprocess.run([sym, "--obj=" + exe, "--functions=short", "--no-inlines"] + ["0x" + a for a in addrs],
                     capture_output=True, text=True).stdout.split("\n\n")
names = {}
for a, block in zip(addrs, out):
    names[a] = block.strip().split("\n")[0]
for l in lines:
    head, stack = l.split("pila host:")
    fr = [names.get(a, a) for a in stack.split()]
    fr = [f.replace("__imp__", "") for f in fr if f not in ("??",)]
    print(head.replace("[sample] ", ""), " <- ".join(fr))

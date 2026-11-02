# Sesión 2 — TOML de XenonRecomp y C++ generado

Fecha: 2026-10-26. Sin commits.

## Resultado

- **XenonAnalyse y XenonRecomp compilados con MSVC 14.44** (Build Tools de VS 2022, que traen CMake y Ninja).
  MSVC no conoce `__builtin_bswap*`/`__builtin_ffsll`: se resuelve sin tocar el código con
  `compat/msvc_compat.h`, incluido a la fuerza (`/FI`). Script: `tools/build_msvc.bat`.
  Binarios: `XenonRecomp/build-msvc/XenonAnalyse/XenonAnalyse.exe` y `.../XenonRecomp/XenonRecomp.exe`.
- **C++ generado completo en `ppc/`** (164 archivos, unos 128 MB), con **0 errores y 0 instrucciones sin reconocer**.
- 304 imports con nombre (XenonRecomp usa las tablas de Xenia) → `xex/import_names.txt`;
  los 317 (incluidas 13 variables) en `xex/imports_named.txt`.

## Cambios a XenonRecomp (copia local, sin commit)

1. **XenonAnalyse** (`XenonAnalyse/main.cpp`): nuevo patrón de jump table `lis / rlwinm / addi / lwzx / mtctr / bctr`
   (el compilador de GH cambia el orden respecto a Unleashed). Ha pasado de 20 a **356 tablas** → `config/gh6_switch_tables.toml`.
   Quedan unas 85 secuencias con `lwzx`+`bctr` que no detecta (ver «Pendiente»).
2. **XenonRecomp** (`XenonRecomp/recompiler.cpp`, aplicado con `tools/patch_recompiler.py`, idempotente):
   35 instrucciones nuevas que usa GH:WoR:
   - Rama: `bdzf` (1280 usos: los `switch` pequeños se compilan como cadenas de `bdzf`), `bdnzt`.
   - Cargas/almacenamientos con update: `lhzu lhzux lhau lbzux lwzux sthu sthux stbux stdux lfsu lfsux stfsu stfsux lfdu stfdu`, y `lhbrx`.
   - Enteros: `eqv subfze addme addc mulhdu`. Coma flotante: `frsqrte fnmadd`. Caché: `dcbst` (no-op).
   - VMX: `vandc vsel128 vpkswss(128) vsubshs vsubuwm vaddsws vcfpuxws128`; `lvehx` reutiliza la ruta de `lvewx`.

## Config (`config/gh6.toml`)

- Direcciones de save/restore, setjmp y longjmp de la sesión 1.
- `functions = [...]`: **11 funciones** declaradas a mano. Son código sin entrada en `.pdata` y con jump table,
  donde el analizador cortaba la función en el `bctr`. Generadas con `tools/gap_functions.py`
  (inicio = último destino de `bl` antes del switch o inicio del hueco; fin = siguiente destino de `bl` tras el último caso).
  Para regenerarlas: quitar `functions` del TOML, ejecutar XenonRecomp, pasar el log a `gap_functions.py`.
- Optimizaciones desactivadas, como recomienda XenonRecomp.

## Comandos

```
tools/build_msvc.bat                                  # compila las herramientas
XenonAnalyse.exe xex/default.xex config/gh6_switch_tables.toml
XenonRecomp.exe config/gh6.toml XenonRecomp/XenonUtils/ppc_context.h   # escribe en ppc/ (debe existir)
```

## Pendiente / riesgos

- Jump tables no detectadas (~85): las que llevan un `b`/`li` antes o usan `lhzx ... ori ... add` (10 casos).
  Sin tabla, el `bctr` se traduce como llamada indirecta (PPC_CALL_INDIRECT_FUNC) a una etiqueta, lo que fallará en ejecución.
  Hay que ampliar los patrones de XenonAnalyse o añadirlas a mano.
- `vsel128`: se asumió el orden de operandos (vD como selector). Verificar con un caso real.
- Al principio de `.text` (0x820D0000..) se generan "funciones" de relleno con `.long 0x0`: son inofensivas.

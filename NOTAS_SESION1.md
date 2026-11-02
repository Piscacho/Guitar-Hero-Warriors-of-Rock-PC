# Sesión 1 — Extracción y mapeo del XEX (Guitar Hero: Warriors of Rock, X360)

Fecha: 2026-10-26. Sin commits (por decisión del usuario).

## Qué se hizo

1. **ISO → default.xex**: `tools/xdvdfs.py` (extractor XDVDFS propio, Python 3.8).
   - Partición del juego en `0xFD90000` (XGD2), confirmada por el magic `MICROSOFT*XBOX*MEDIA`.
   - `xex/default.xex` (10.006.528 bytes, SHA1 `baca48b6e3472dcb0977a3b483a881e195a02ba2`).
   - `xex/iso_filelist.txt`: 1375 entradas (ruta, tamaño, offset absoluto en el ISO).
2. **XEX → imagen PE**: `tools/xex2.py` (parser XEX2 propio; AES con `openssl` de MSYS2).
   - Cifrado: AES-128 retail (clave de sesión = AES-ECB(file_key, clave retail), luego CBC IV=0).
   - Compresión: **basic** (sin LZX, no hace falta descompresor).
   - `xex/basefile.bin` (13.172.736 bytes = 0xC90000, SHA1 `92a0610c587cbb0539fb5701c8a348b243d73a76`).
     Es la imagen **mapeada en memoria** (offset de archivo = VA − 0x82000000), no el layout de archivo PE.
   - `xex/xex_info.txt`: cabeceras opcionales, regiones, librerías de import y thunks.
3. **Imports**: `tools/imports.py` → `xex/imports.txt` (lib, ordinal, var/func, dirección).
4. **Funciones y helpers**: `tools/find_helpers.py`, `tools/analyze.py` → `xex/functions_pdata.txt`.
5. **XenonRecomp** clonado (shallow, con submódulos) en `XenonRecomp/`. **No compilado**:
   no hay cmake/clang en PATH, aunque sí existe `C:\Program Files (x86)\Microsoft Visual Studio\2022`
   (revisar en sesión 2 si trae cmake/clang-cl/ninja vía "Developer PowerShell").

## REA

**REA no soporta PowerPC**: `open_binary` rechaza el PE con
`unsupported PE architecture (COFF machine 0x01f2)` y el modo `raw-binary` solo acepta
x86/x86_64/arm/arm64/6502/m68k. Por eso no hay snapshot en `rea/` (ver `rea/LEEME.md`).
Todo el análisis se hizo con scripts Python propios sobre `basefile.bin`.
Alternativas para sesiones futuras: Ghidra + plugin XEX (XEXLoaderWV), IDA con idaxex,
o el propio `XenonAnalyse` de XenonRecomp.

## Hallazgos

| Dato | Valor |
|---|---|
| Title ID | 41560883 |
| Base de carga | 0x82000000 |
| Tamaño imagen | 0xC90000 |
| **Entry point** | **0x822CB0A0** |
| Máquina PE | 0x1F2 (PowerPC big-endian) |
| Funciones en `.pdata` | **19.191** (0x820D0040 … 0x827CDAB8) |
| Imports | xam.xex: 132 funciones · xboxkrnl.exe: 172 funciones + 13 variables (317 únicos) |

### Secciones PE (VA)

| Sección | VA | Tamaño virtual | Tipo |
|---|---|---|---|
| .rdata | 0x82000400 | 0x9BE30 | RO |
| .pdata | 0x8209C400 | 0x257B8 | RO (tabla de funciones) |
| BINKCONS | 0x820C1C00 | 0x2920 | RO |
| **.text** | **0x820D0000** | **0x6EE6A4** | código |
| BINK | 0x827BE800 | 0xFBD0 | código (Bink video) |
| BINKBSS | 0x827D0000 | 0x3B8 | bss |
| .data | 0x827D0400 | 0x3E5B95 | RW |
| .XBMOVIE | 0x82BB6000 | 0xC | RW |
| BINKDATA | 0x82BB6200 | 0x3D68 | RW |
| .idata | 0x82BC0000 | 0x56C | RW |
| .XBLD | 0x82BD0000 | 0x150 | RO |
| .reloc | 0x82BD0200 | 0x5D8C8 | RO |

Regiones de páginas del XEX: RO 0x82000000-0x820D0000, código 0x820D0000-0x827D0000,
datos 0x827D0000-0x82BC0000, RO 0x82BC0000-0x82C90000.

Nota: los thunks de import (funciones) están al final de `.text`, en ~0x827BD3A4–0x827BDA24;
los descriptores de import (variables) en 0x82000400+ (`.rdata`). Ordinales completos en `xex/imports.txt`
(sin nombres: en sesión 2/3 cruzar con `xboxkrnl_table.inc` / `xam_table.inc` de Xenia).

### Direcciones para el TOML de XenonRecomp

```toml
[main]
file_path = "../xex/default.xex"
out_directory_path = "../ppc"
# switch_table_file_path = "gh6_switch_tables.toml"   # generar con XenonAnalyse

restgprlr_14_address = 0x826ABBA0
savegprlr_14_address = 0x826ABB50
restfpr_14_address   = 0x826ABDCC
savefpr_14_address   = 0x826ABD80
restvmx_14_address   = 0x826AD188
savevmx_14_address   = 0x826ACEF0
restvmx_64_address   = 0x826AD21C
savevmx_64_address   = 0x826ACF84

longjmp_address = 0x82798C70
setjmp_address  = 0x82798F90
```

Verificación:
- save/rest gpr/fpr/vmx: patrones exactos del README de XenonRecomp (`std r14,-0x98(r1)`,
  `stfd f14,-0x90(r12)`, `li r11,-0x120 / stvx v14,r11,r12`, `li r11,-0x400 / stvx128 v64,r11,r12`, etc.). Un único acierto por patrón.
- **longjmp 0x82798C70** (entrada .pdata, len 0x314): restaura f14–f31 desde jmp_buf+0x00, r13–r31 desde +0x98,
  v64+ desde +0x140; antes llama a 0x826C2280 (probablemente el unwind; confirmar).
- **setjmp 0x82798F90** (sin entrada .pdata; hueco entre 0x82798F84 y 0x82799260):
  `lis r4,0x82BB; lwz r0,0x5E64(r4); cmpwi r0,0; mtctr r0; bnectr` (hook opcional) y luego `mflr r0; mfcr r4`
  y guarda el mismo layout que longjmp restaura. **Ojo**: al no estar en .pdata, quizás haya que
  añadirlo en `functions = [...]` del TOML (calcular su tamaño buscando el `blr`).

## Herramientas creadas (`tools/`)

- `xdvdfs.py` — `python -I tools/xdvdfs.py ISO list salida.txt` / `extract RUTA DESTINO`
- `xex2.py` — `python -I tools/xex2.py xex/default.xex xex` (soporta basic; rama LZX escrita pero sin `lzx.py`, no hizo falta)
- `imports.py`, `find_helpers.py`, `analyze.py` — imports, helpers de registros, .pdata.

## Próximos pasos (sesión 2)

1. Toolchain: comprobar si VS 2022 trae `cmake`, `ninja` y `clang-cl` (componente "C++ Clang tools");
   si no, instalar LLVM/Clang 18+ y CMake. XenonRecomp requiere Clang.
2. Compilar XenonRecomp (`XenonAnalyse` + `XenonRecomp`).
3. Ejecutar `XenonAnalyse xex/default.xex` para generar las jump tables (switch tables).
4. Escribir `config/gh6.toml` con las direcciones de arriba (+ setjmp en `functions` si hace falta).
5. Generar el C++ (`ppc/`), revisar errores de funciones mal delimitadas (añadir `functions`/`invalid_instructions`)
   y conseguir que compile con un `main` mínimo + stubs de imports.
6. Nombrar los 317 imports con las tablas de Xenia (BSD).

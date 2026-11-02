# Sesión 3 — Runtime base

Fecha: 2026-10-26. Sin commits.

## Toolchain

- **LLVM/Clang 23.1.4 portable** (release oficial, `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.xz`, 859 MB),
  descomprimido en `toolchain/clang+llvm-23.1.4-x86_64-pc-windows-msvc/`. Se descargó con permiso del usuario; no se instaló nada en el sistema.
  El `.tar.zst` (263 MB) no se pudo usar: el `tar` de Windows falla con «Frame requires too much memory» y no hay `zstd`.
  El instalador `toolchain/dl/llvm.tar.xz` se puede borrar.
- `clang-cl` + `lld-link` + librerías de MSVC 14.44 (vcvars64). Script: `tools/build_game.bat` (CMake + Ninja).

## Proyecto (`project/CMakeLists.txt`)

- `GH6RecompLib`: todo `ppc/*.cpp` (`/O1 -fno-strict-aliasing -msse4.1`).
- `GH6.exe`: `runtime/main.cpp`, `runtime/kernel.cpp`, `runtime/import_stubs.cpp`.
- Salida: `project/build/GH6.exe`. Uso: `GH6.exe basefile.bin default.xex`
  (por defecto busca ambos en el directorio actual).

## Runtime (`runtime/`)

- `main.cpp`: reserva 4 GB de memoria de invitado (`VirtualAlloc`), copia `basefile.bin` en 0x82000000,
  copia las cabeceras XEX en 0x82BF0000, rellena la tabla de llamadas indirectas (`PPC_LOOKUP_FUNC`) con
  `PPCFuncMappings`, inicializa las variables del kernel, crea PCR/TLS/pila y llama a `_xstart`.
- `kernel.cpp`: imports hechos a mano (30). Son los asignadores bump (virtual 0x40000000+, física 0xA0000000+, objetos de kernel 0x7EFF0000+),
  la PCR (X_KPCR de Xenia: tls 0x00, pcr 0x30, pila 0x70/0x74, hilo 0x100) y 64 slots TLS (cabecera 0x00020104).
  Además: secciones críticas (de momento de un solo hilo), `KeTls*`, `NtAllocate/FreeVirtualMemory`, `MmAllocatePhysicalMemoryEx`,
  `MmGetPhysicalAddress`, `RtlImageXexHeaderField`, `XGet*`, `KeQuery*`, `DbgPrint`, `KeBugCheck*`, etc.
  Las 13 variables importadas (`XboxHardwareInfo`, `XboxKrnlVersion`, `KeTimeStampBundle`, `XexExecutableModuleHandle`…)
  se crean en memoria de invitado y su descriptor en `.rdata` se sobrescribe con la dirección.
- `import_stubs.cpp`: **generado** con `tools/gen_import_stubs.py` (262 stubs: avisan en la 1.ª llamada y devuelven 0).
  Hay que regenerarlo tras añadir `GUEST_IMPL(...)` en `kernel.cpp` (excluye automáticamente los ya implementados).
- `var_imports.inc`: generado por el mismo script.

## Estado de la compilación y la ejecución

- **`project/build/GH6.exe` compila y enlaza** (29 MB). Los 166 objetos tardan unos 45 s con 12 hilos.
  Un arreglo necesario: `bdzf`/`bdnzt` con destino fuera de la función se convierten en llamada + `return`
  (ya incluido en `tools/patch_recompiler.py`).
- **Ejecución** (`GH6.exe xex/basefile.bin xex/default.xex`, logs `run1..3.log`):
  1. Registra 41.099 funciones, entra en `_xstart`, pasa el CRT y el init del juego.
  2. Intenta `XexLoadImage("xam.xex")` (se responde «no encontrado»), crea un evento, un mutex y su primer hilo
     (`ExCreateThread start=0x824247B8`, todavía **no se ejecuta**).
  3. Empieza a leer archivos: `NtOpenFile`, `NtQueryDirectoryFile`, `NtCreateFile`, `NtQueryInformationFile`, `NtReadFile` (stubs).
  4. Se para en una **llamada indirecta a 0x826B4254** (lr=0x826B3E38). Es un caso de una jump table no detectada
     en la función 0x826B3D28 (`bctr` en 0x826B4158). Patrón: `... addi r12,r12,lo / lhzx r0,r12,r0 / lis r12,0x826B /
     addi r12,r12,0x415C / nop / add r12,r12,r0 / mtctr r12 / bctr` (offsets de 16 bits sumados a una base; tiene un `nop`
     que el patrón `wordOffsetSwitch` de XenonAnalyse no espera).
- Diagnóstico añadido: `compat/ppc_indirect.h` (incluido a la fuerza en el código PPC) desvía `PPC_CALL_INDIRECT_FUNC`
  a `GuestCallIndirect`, que avisa de la dirección de invitado y del `lr` en vez de saltar a 0. También hay un
  manejador de excepciones (`[crash] ...`) en `main.cpp`.
- Imports implementados a mano: 42. Stubs: 262.

## Próximos pasos

0. **Jump tables pendientes** (bloquea ya la ejecución): añadir a XenonAnalyse la variante con `lhzx` + `nop`
   (y revisar las otras ~85 sin detectar); regenerar `config/gh6_switch_tables.toml` y `ppc/`, y recompilar.
1. Hilos: `ExCreateThread` con hilos nativos, cada uno con su `PPCContext`, PCR, TLS y pila; secciones críticas y
   eventos reales (`NtCreateEvent`, `KeWaitForSingleObject`, `NtWaitForSingleObjectEx`…). Usar como referencia la implementación de UnleashedRecomp.
2. Sistema de archivos: `NtCreateFile`/`NtReadFile`/`NtQueryInformationFile`… redirigidos al ISO extraído (`game:\` → carpeta), con `tools/xdvdfs.py` para extraer.
3. Completar las jump tables pendientes (ver NOTAS_SESION2).
4. Sesión 4 (GPU): `VdInitializeEngines`, `VdSwap`, ring buffer del Xenos → backend D3D12/Vulkan (reutilizar Xenia, BSD).

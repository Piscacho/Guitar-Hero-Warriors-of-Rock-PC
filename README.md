# Guitar Hero: Warriors of Rock para PC

Recompilación estática para PC de **Guitar Hero: Warriors of Rock** (Xbox 360, 41560883) para Windows 10/11.

> **No se incluyen archivos del juego.** Necesitas tu propio ISO de Xbox 360 de Guitar Hero: Warriors of Rock
> (versión USA/Europa: *Guitar Hero - Warriors of Rock (USA, Europe) (En,Fr,De,Es,It)*).
> Guitar Hero es una marca de Activision. Este proyecto no está afiliado a Activision.

## Descargar y jugar

1. Descarga el `.zip` desde la página de
   [Releases](https://github.com/Piscacho/Guitar-Hero-Warriors-of-Rock-PC/releases/latest).
2. Descomprímelo en una carpeta, por ejemplo `C:\Juegos\GHWoR`.
3. Abre **`Guitar Hero Warriors of Rock.exe`**. Si Windows muestra "Windows protegió tu PC", pulsa
   **Más información → Ejecutar de todas formas**.
4. En el launcher, pestaña **Juego**, elige el ISO con **Examinar...** y pulsa **JUGAR**.

- **Controles**: un mando de Xbox o de PlayStation se convierte en guitarra o batería (o mando normal para la voz),
  con preajustes para el mando de Xbox (clásico o con gatillos). Las guitarras y baterías XInput para PC se detectan
  solas. Hasta 4 jugadores, cada uno con su instrumento, y botones reasignables (comunes o por jugador) en **Controles**.
  **Guitarra CRKD (Gibson Les Paul)**: en **Controles → Preajustes** elige *Guitarra CRKD* (palanca e inclinación).
  ¿Otra guitarra que no responde bien? **Controles → Informe de mandos** graba lo que envía y genera `mandos.txt`
  para poder configurarla.
- **Imagen**: suavizado de bordes FXAA, filtrado anisotrópico de texturas y nitidez CAS (pestaña **Imagen y audio**).
- **Micrófono**: cualquier micrófono de Windows hace de micrófono USB de la consola (pestaña **Micrófono**).
- **DLC**: copia tus paquetes de Xbox 360 (archivos LIVE/PIRS/CON) en `dlc\`; se importan solos al arrancar.
- **Sincronía**: calibra una vez en el juego, en **Options → Calibrate Lag → Calibrate**.
- **Actualizar**: el launcher avisa de versiones nuevas (**Juego → Actualizaciones**) y se actualiza solo,
  conservando la configuración, las partidas y el DLC.

### Dónde se guarda todo

- Partidas, DLC importado y respaldos: carpeta `save\` junto al ejecutable (consérvala si actualizas a mano).
- **Empezar de cero**: botón en **Juego → Partidas**; la partida anterior se guarda en `save\respaldos\`
  y se recupera con **Restaurar respaldo**.
- Configuración: `gh6.ini`. Registro de la última partida: `gh6.log`.

### Teclas

F11 o Alt+Enter: pantalla completa · F3: contador de FPS · F12: captura · F6: cambia el instrumento del jugador del teclado.
Teclado: flechas, Enter = Start, 1-5 = trastes, Retroceso = poder estrella.

## Compilar desde el código

El repositorio **no contiene nada del juego**: ni el ISO, ni el XEX, ni el código C++ que XenonRecomp genera a partir
de él (`ppc/`). Cada persona lo genera desde su propio ISO.

Requisitos:
- Visual Studio 2022 Build Tools (MSVC 14.4x, con CMake y Ninja).
- LLVM 23 para Windows descomprimido en `toolchain/clang+llvm-23.1.4-x86_64-pc-windows-msvc/`.
- FFmpeg 8 compartido (`avcodec-62`, `avutil-60`, `swresample-6`) en `toolchain/ffmpeg/` (`include/`, `lib/`, `bin/`).
- Python 3.8+ (Pillow para `tools/make_icon.py`) y `openssl` en el PATH para `tools/xex2.py`.

Pasos (desde la raíz del repositorio):

```bat
git clone --recursive https://github.com/Piscacho/Guitar-Hero-Warriors-of-Rock-PC.git
cd Guitar-Hero-Warriors-of-Rock-PC

rem 1. Sacar el ejecutable del ISO y descifrarlo (genera xex\basefile.bin)
python -I tools\xdvdfs.py "<ruta al ISO>" extract default.xex xex\default.xex
python -I tools\xex2.py xex\default.xex xex

rem 2. XenonRecomp con los cambios para este juego, compilado con MSVC en XenonRecomp\build-msvc
cd XenonRecomp
git apply ..\xenonrecomp_gh6.diff
cd ..

rem 3. Generar el código C++ del juego
mkdir ppc
XenonRecomp\build-msvc\XenonRecomp\XenonRecomp.exe config\gh6.toml XenonRecomp\XenonUtils\ppc_context.h

rem 4. Compilar y crear la carpeta para jugar / la release
tools\build_game.bat
tools\make_dist.bat
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1
```

Modo desarrollo: `project\build\GH6.exe xex\basefile.bin xex\default.xex [ISO] [datos]` (sin launcher). Las opciones
de diagnóstico `GH6_*` están documentadas en `NOTAS_SESION*.md`.

### Estructura

| Carpeta | Contenido |
|---|---|
| `runtime/` | Kernel y XAM de Xbox 360, GPU Xenos → Direct3D 12, audio (XMA/WASAPI), entrada, micrófono, launcher y actualizador |
| `config/` | Configuración de XenonRecomp (funciones y tablas de saltos del juego) |
| `project/` | CMake, icono y recursos del ejecutable |
| `tools/` | Extracción del ISO/XEX, análisis, compilación, distribución y release |
| `scripts/` | Guiones de entrada para pruebas automáticas (`GH6_INPUTSCRIPT`) |
| `dist_files/` | Archivos que acompañan al ejecutable (ini, LEEME y documentos de la release) |
| `NOTAS_SESION*.md` | Diario técnico de cada sesión de trabajo |

## Requisitos para jugar

- Windows 10 (1803 o posterior) / 11 de 64 bits.
- Tarjeta gráfica con Direct3D 12 y controlador actualizado.

## Créditos

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): recompilador de PowerPC a C++.
- [Xenia](https://github.com/xenia-project/xenia): documentación y referencia del comportamiento de la GPU y del kernel de Xbox 360.
- [FFmpeg](https://ffmpeg.org): decodificación de audio XMA.

## Licencia

MIT ([LICENSE](LICENSE)). Los componentes de terceros mantienen sus propias licencias
([THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)).

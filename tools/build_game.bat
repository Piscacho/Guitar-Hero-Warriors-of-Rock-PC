@echo off
rem Compila project\build\GH6.exe. Requiere VS 2022 Build Tools (MSVC + CMake + Ninja) y LLVM en toolchain\ (ver README).
set ROOT=%~dp0..
set VSBT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set LLVM=%ROOT%\toolchain\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin
set PATH=%LLVM%;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%ROOT%\project"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link || exit /b 1
cmake --build build %* -- -k 0

@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=G:\Guitar Hero Warrior of rock\Recomp\toolchain\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "G:\Guitar Hero Warrior of rock\Recomp\project\build"
ninja %*

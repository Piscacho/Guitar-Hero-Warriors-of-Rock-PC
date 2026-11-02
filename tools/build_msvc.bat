@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "G:\Guitar Hero Warrior of rock\Recomp\XenonRecomp"
cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_CXX_FLAGS=/EHsc /utf-8 /FI\"G:/Guitar Hero Warrior of rock/Recomp/compat/msvc_compat.h\"" "-DCMAKE_C_FLAGS=/utf-8" || exit /b 1
cmake --build build-msvc --target XenonAnalyse XenonRecomp -j 8

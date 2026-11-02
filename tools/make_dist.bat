@echo off
rem Crea la carpeta de distribucion dist\GH6WoR (sesion 23): ejecutable, DLL de FFmpeg (LGPL), gh6.ini de ejemplo y LEEME.
rem Uso: tools\make_dist.bat  (desde Recomp\)
setlocal
set DIST=%~dp0..\dist\GH6WoR
set RUNDIR=%~dp0..\run
if not exist "%DIST%" mkdir "%DIST%"
if not exist "%DIST%\dlc" mkdir "%DIST%\dlc"
copy /y "%~dp0..\project\build\GH6.exe" "%DIST%\GH6WoR.exe" >nul
copy /y "%~dp0..\project\build\avcodec-62.dll" "%DIST%" >nul
copy /y "%~dp0..\project\build\avutil-60.dll" "%DIST%" >nul
copy /y "%~dp0..\project\build\swresample-6.dll" "%DIST%" >nul
copy /y "%~dp0..\dist_files\LEEME.txt" "%DIST%" >nul
rem Sesion 24: shaders vistos (microcodigo) y cache de shaders compilados: la primera partida no se congela al entrar en canciones
if exist "%RUNDIR%\shaders" xcopy /d /y /q /i "%RUNDIR%\shaders\*.bin" "%DIST%\shaders\" >nul
if exist "%RUNDIR%\shadercache" xcopy /d /y /q /i "%RUNDIR%\shadercache\*.cso" "%DIST%\shadercache\" >nul
if not exist "%DIST%\gh6.ini" copy /y "%~dp0..\dist_files\gh6.ini" "%DIST%" >nul
echo Distribucion creada en %DIST%
endlocal

# Crea la release para GitHub (sesion 24), al estilo de las de Persona:
#   <Out>\Guitar Hero Warriors of Rock\          carpeta lista para jugar (no pisa gh6.ini ni las partidas si ya existe)
#   <Out>\Guitar-Hero-Warriors-of-Rock-PC-v<ver>-windows.zip   (+ .sha256)  para subir como asset de la release
# Uso (desde Recomp\):  powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 [-Out "G:\Guitar Hero Warrior of rock"] [-NoBuild]
param([string]$Out = "", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$R = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Split-Path $R -Parent }
$name = "Guitar Hero Warriors of Rock"

# Version: la de updater.cpp (AppVersion) debe coincidir con project\gh6.rc
$ver = [regex]::Match((Get-Content "$R\runtime\updater.cpp" -Raw), 'AppVersion\(\) \{ return "([0-9.]+)"').Groups[1].Value
$rcVer = [regex]::Match((Get-Content "$R\project\gh6.rc" -Raw), 'FILEVERSION (\d+),(\d+),(\d+)')
if ("$($rcVer.Groups[1].Value).$($rcVer.Groups[2].Value).$($rcVer.Groups[3].Value)" -ne $ver) { throw "Version distinta en updater.cpp ($ver) y gh6.rc: actualiza las dos" }
Write-Host "Release v$ver"

if (-not $NoBuild) {
    cmd /c "`"$R\tools\build_game.bat`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "La compilacion fallo" }
}
$bin = "$R\project\build"

# 1. Carpeta limpia de preparacion
$stage = "$R\release_staging\$name"
Remove-Item "$R\release_staging" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\licenses", "$stage\dlc", "$stage\save" | Out-Null
Copy-Item "$bin\GH6.exe" "$stage\$name.exe"
Copy-Item "$bin\avcodec-62.dll", "$bin\avutil-60.dll", "$bin\swresample-6.dll" $stage
Copy-Item "$R\dist_files\release\*" $stage
Copy-Item "$R\toolchain\ffmpeg\LICENSE.txt" "$stage\licenses\FFmpeg-LGPL-3.0.txt"
Copy-Item "$R\XenonRecomp\LICENSE.md" "$stage\licenses\XenonRecomp-MIT.txt"
Copy-Item "$R\runtime\smaa\LICENSE.txt" "$stage\licenses\SMAA-MIT.txt"
# gh6.ini de la release: sin rutas personales, partidas en save\
$ini = Get-Content "$R\dist_files\gh6.ini" -Encoding UTF8
$ini = $ini -replace '^iso=.*', 'iso=' -replace '^datos=.*', 'datos=save' -replace '^dlc=.*', 'dlc=dlc'
$ini += @('; ---- Actualizaciones ----', '; actualizar_al_abrir: 1 = el launcher busca versiones nuevas en GitHub al abrirse', 'actualizar_al_abrir=1',
          '; actualizar_repo: repositorio de GitHub de donde se descargan las actualizaciones', 'actualizar_repo=Piscacho/Guitar-Hero-Warriors-of-Rock-PC')
[IO.File]::WriteAllLines("$stage\gh6.ini", $ini, (New-Object Text.UTF8Encoding $false))
[IO.File]::WriteAllText("$stage\dlc\LEEME.txt", "Copia aqui tus paquetes de DLC de Xbox 360 (archivos LIVE/PIRS/CON). Se importan solos al arrancar el juego.`r`n", (New-Object Text.UTF8Encoding $false))
[IO.File]::WriteAllText("$stage\save\LEEME.txt", "Aqui se guardan tus partidas, el DLC importado y los respaldos.`r`nPara empezar de cero usa el boton 'Empezar de cero' del launcher (pestana Juego): tu partida anterior queda en save\respaldos.`r`nConserva esta carpeta si actualizas a mano.`r`n", (New-Object Text.UTF8Encoding $false))
# Nada del juego ni de pruebas en el paquete
$bad = Get-ChildItem $stage -Recurse -Include "*.iso", "*.xex", "*.bin", "*.cso", "*.log", "shot_*" -File
if ($bad) { throw "Archivos que no deben ir en la release: $($bad.FullName -join ', ')" }

# 2. Zip (una carpeta superior con el nombre del juego) + suma SHA-256
$zip = "$Out\Guitar-Hero-Warriors-of-Rock-PC-v$ver-windows.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C "$R\release_staging" $name
if ($LASTEXITCODE -ne 0) { throw "No se pudo crear el zip" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ASCII

# 3. Copia en <Out>\<nombre>: actualiza el programa y los documentos; respeta gh6.ini, save\ y dlc\ existentes
$dst = "$Out\$name"
New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($f in Get-ChildItem $stage) {
    $target = Join-Path $dst $f.Name
    if ($f.PSIsContainer) {
        if (@("save", "dlc") -contains $f.Name) { if (-not (Test-Path $target)) { Copy-Item $f.FullName $target -Recurse } }
        else { Copy-Item $f.FullName $dst -Recurse -Force }
    }
    elseif ($f.Name -eq "gh6.ini") { if (-not (Test-Path $target)) { Copy-Item $f.FullName $target } }
    else { Copy-Item $f.FullName $target -Force }
}
Write-Host ""
Write-Host "Carpeta: $dst"
Write-Host "Zip:     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Host "SHA-256: $hash"
Write-Host "Etiqueta para GitHub: v$ver  (el launcher compara esta etiqueta con su version)"

# Gera a versão de distribuição: build Release, pasta portátil, zip e instalador.
# Uso (PowerShell 7, na raiz do repositório): pwsh scripts/build-release.ps1
# Requer: CMake, Visual Studio 2022 (C++), FFmpeg mínimo em third_party/ffmpeg (bash scripts/build-ffmpeg.sh)
# e Inno Setup 6 (para o instalador).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# A distribuição usa o FFmpeg mínimo (3 DLLs, ~3 MB). O completo (fetch-ffmpeg.ps1) funciona, mas pesa ~120 MB.
if (Test-Path third_party\ffmpeg\bin\avfilter-*.dll) {
    throw "third_party\ffmpeg tem o FFmpeg completo. Gere o mínimo com: bash scripts/build-ffmpeg.sh"
}

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(Qlip VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "Qlip $version"

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "Falha ao configurar o CMake." }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Falha no build." }

# Pasta portátil: exe, DLLs do FFmpeg e licenças.
$portable = Join-Path $root "dist\Qlip-portable"
if (Test-Path $portable) { Remove-Item -Recurse -Force $portable }
New-Item -ItemType Directory -Force $portable | Out-Null
# Só as bibliotecas que o Qlip carrega.
Copy-Item build\Release\Qlip.exe $portable -Force
foreach ($pattern in "avcodec-*.dll", "avformat-*.dll", "avutil-*.dll") {
    Copy-Item "build\Release\$pattern" $portable -Force
}
Copy-Item README.md, THIRD_PARTY.md $portable -Force
Copy-Item LICENSE (Join-Path $portable "LICENSE.txt") -Force
Copy-Item third_party\ffmpeg\LICENSE.txt (Join-Path $portable "FFMPEG-LICENSE.txt") -Force -ErrorAction SilentlyContinue

Compress-Archive -Path "$portable\*" -DestinationPath dist\Qlip-win64.zip -Force

# Instalador.
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 não encontrado (winget install JRSoftware.InnoSetup)." }
& $iscc /Q "/DAppVersion=$version" installer\Qlip.iss
if ($LASTEXITCODE -ne 0) { throw "Falha ao gerar o instalador." }

Get-Item dist\Qlip-win64.zip, dist\Qlip-Setup.exe | ForEach-Object { "{0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB) }

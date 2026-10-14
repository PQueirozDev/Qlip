# Gera a versão de distribuição: build Release, pasta portátil, zip e instalador.
# Uso (PowerShell 7, na raiz do repositório): pwsh scripts/build-release.ps1
# Requer: CMake, Visual Studio 2022 (C++), FFmpeg em third_party/ffmpeg (scripts/fetch-ffmpeg.ps1)
# e Inno Setup 6 (para o instalador).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

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
# Só as bibliotecas que o Qlip carrega (avdevice/avfilter não são usadas).
Copy-Item build\Release\Qlip.exe $portable -Force
foreach ($pattern in "avcodec-*.dll", "avformat-*.dll", "avutil-*.dll", "swresample-*.dll", "swscale-*.dll") {
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

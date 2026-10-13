# Baixa a build LGPL shared do FFmpeg 9.0 (BtbN) para third_party/ffmpeg.
# Contém headers, import libs (.lib) e DLLs. É LGPL (sem x264/x265), o que permite distribuir o Qlip
# como MIT com as DLLs ao lado do executável.
#
# Uso (na raiz do repositório):  pwsh scripts/fetch-ffmpeg.ps1

$ErrorActionPreference = "Stop"
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip"
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root "third_party\ffmpeg"

if (Test-Path (Join-Path $target "include\libavcodec\avcodec.h")) {
    Write-Host "FFmpeg já está em $target"
    exit 0
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("qlip-ffmpeg-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $temp | Out-Null
try {
    $zip = Join-Path $temp "ffmpeg.zip"
    Write-Host "Baixando $url"
    Invoke-WebRequest $url -OutFile $zip
    Expand-Archive $zip -DestinationPath (Join-Path $temp "x")
    $extracted = Get-ChildItem (Join-Path $temp "x") -Directory | Select-Object -First 1
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item (Join-Path $extracted.FullName "*") $target -Recurse -Force
    Write-Host "FFmpeg pronto em $target"
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}

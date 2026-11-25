# Terceiros

## FFmpeg (LGPL 2.1+, build shared mínima, sem componentes GPL)

- Fonte: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, compilado por `scripts/build-ffmpeg.sh` (MinGW-w64).
  O script contém a configuração exata usada (`--disable-everything` mais só os componentes do Qlip:
  `h264_nvenc`, `aac`, muxer `mp4`/`mov`, demuxers `mov`/`matroska`, D3D11VA), o que permite reproduzir as DLLs.
- Localização no projeto: `third_party/ffmpeg/` (headers, import libs e DLLs: avcodec, avformat e avutil)
- Licença completa: `third_party/ffmpeg/LICENSE.txt` (cópia da LGPL 2.1)
- Encoder usado: `h264_nvenc` (NVENC da NVIDIA, acessado pelo driver em tempo de execução)
- Redistribuição: as DLLs de `third_party/ffmpeg/bin` são copiadas ao lado do executável. Para distribuir,
  mantenha a build LGPL (sem `--enable-gpl` nem `--enable-nonfree`) e inclua o `LICENSE.txt` e este arquivo.
- Alternativa: `scripts/fetch-ffmpeg.ps1` baixa a build LGPL shared completa da BtbN
  (https://github.com/BtbN/FFmpeg-Builds), que também funciona, mas pesa ~120 MB.

## nv-codec-headers (MIT)

- Headers do NVENC usados só para compilar o FFmpeg (tag `n12.1.14.0`, drivers NVIDIA 531 ou mais novos).
  Baixados por `scripts/build-ffmpeg.sh` de https://github.com/FFmpeg/nv-codec-headers. Não são redistribuídos.

## nlohmann/json (MIT)

- Baixado pelo CMake via FetchContent (v3.11.3).

## Windows SDK / C++/WinRT (Microsoft)

- Usado para Windows Graphics Capture. Não é redistribuído.

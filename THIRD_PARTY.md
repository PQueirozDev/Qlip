# Terceiros

## FFmpeg (LGPL 2.1+, build shared sem x264/x265)

- Fonte: https://github.com/BtbN/FFmpeg-Builds (`ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip`)
- Localização no projeto: `third_party/ffmpeg/` (headers, `.lib` de importação e DLLs)
- Licença completa: `third_party/ffmpeg/LICENSE.txt`
- Encoder usado: `h264_nvenc` (NVENC da NVIDIA, acessado pelo driver; não há SDK da NVIDIA embutido)
- Redistribuição: as DLLs de `third_party/ffmpeg/bin` são copiadas ao lado do executável. Para distribuir,
  mantenha esta build LGPL (sem componentes GPL) e inclua o `LICENSE.txt` e este arquivo.

## nlohmann/json (MIT)

- Baixado pelo CMake via FetchContent (v3.11.3).

## Windows SDK / C++/WinRT (Microsoft)

- Usado para Windows Graphics Capture. Não é redistribuído.

#!/usr/bin/env bash
# Compila um FFmpeg LGPL mínimo para o Qlip: só o que o app usa.
#   - encoders: h264_nvenc (entrada D3D11, zero-copy) e aac
#   - muxer mp4/mov; demuxers mov (gravação temporária) e matroska (temporários de versões antigas)
#   - sem avfilter, avdevice, swscale, swresample, rede ou programas
# Resultado em third_party/ffmpeg (headers, DLLs, .dll.a e .lib para o MSVC).
#
# Requisitos (Windows): Git Bash, MinGW-w64 gcc (ex.: WinLibs UCRT), nasm, mingw32-make, e o
# Visual Studio Build Tools (lib.exe, para gerar as import libs .lib).
# Uso (na raiz do repositório):  bash scripts/build-ffmpeg.sh

set -euo pipefail

FFMPEG_VERSION="9.0.2"
# SDK 12.1 do NVENC: funciona com drivers NVIDIA 531+ (versões mais novas do header exigiriam drivers mais novos).
NVHEADERS_TAG="n12.1.14.0"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/third_party/ffmpeg-build"
PREFIX="$ROOT/third_party/ffmpeg"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"

mkdir -p "$WORK/tmp"
# O configure do FFmpeg não lida com o TEMP do Windows (C:\...): usa uma pasta própria, em formato POSIX.
export TMPDIR="$WORK/tmp" TEMP="$WORK/tmp" TMP="$WORK/tmp"
cd "$WORK"

if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
    echo "Baixando FFmpeg $FFMPEG_VERSION"
    curl -fL --retry 3 -o "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    tar -xJf "ffmpeg-$FFMPEG_VERSION.tar.xz"
fi

if [ ! -d "nv-codec-headers" ]; then
    echo "Baixando nv-codec-headers $NVHEADERS_TAG"
    curl -fL --retry 3 -o nv.tar.gz "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/$NVHEADERS_TAG.tar.gz"
    mkdir -p nv-codec-headers
    tar -xzf nv.tar.gz -C nv-codec-headers --strip-components=1
fi

# Os headers do NVENC são só .h: instalados num prefixo local, achados por um pkg-config mínimo
# (o Git Bash não tem pkg-config, e o configure do FFmpeg só detecta o ffnvcodec por ele).
NVPREFIX="$WORK/nv-prefix"
mkdir -p "$NVPREFIX/include/ffnvcodec" "$WORK/bin"
cp nv-codec-headers/include/ffnvcodec/*.h "$NVPREFIX/include/ffnvcodec/"
NVVERSION="${NVHEADERS_TAG#n}"
cat > "$WORK/bin/pkg-config" <<EOF
#!/usr/bin/env bash
# pkg-config mínimo: responde só pelo ffnvcodec.
for arg in "\$@"; do
    case "\$arg" in
        --version) echo "0.29.2"; exit 0 ;;
        --cflags) echo "-I$NVPREFIX/include"; exit 0 ;;
        --libs) echo ""; exit 0 ;;
        --modversion) echo "$NVVERSION"; exit 0 ;;
    esac
done
for arg in "\$@"; do
    case "\$arg" in ffnvcodec*) exit 0 ;; esac
done
exit 1
EOF
chmod +x "$WORK/bin/pkg-config"
export PATH="$WORK/bin:$PATH"

cd "ffmpeg-$FFMPEG_VERSION"
if [ ! -f ffbuild/config.mak ] || [ "${RECONFIGURE:-0}" = "1" ]; then
    ./configure \
        --prefix="$PREFIX" \
        --target-os=mingw32 --arch=x86_64 \
        --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
        --disable-network --disable-autodetect --disable-everything \
        --enable-w32threads \
        --enable-d3d11va --enable-ffnvcodec --enable-nvenc \
        --enable-encoder=h264_nvenc,aac \
        --enable-muxer=mp4,mov \
        --enable-demuxer=mov,matroska \
        --enable-parser=h264,aac \
        --enable-protocol=file \
        --pkg-config="$WORK/bin/pkg-config" \
        --extra-ldflags="-static-libgcc -static"
fi

mingw32-make -j"$JOBS"
rm -rf "$PREFIX"
mingw32-make install

# Import libs para o MSVC (avcodec.lib etc.) a partir dos .def gerados pelo link do MinGW.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
VSPATH="$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
LIBEXE="$(ls "$VSPATH"/VC/Tools/MSVC/*/bin/Hostx64/x64/lib.exe | tail -1)"
# Opções com "-" em vez de "/": o Git Bash converteria "/def:..." em caminho.
for def in lib*/*-[0-9]*.def; do
    [ -f "$def" ] || continue
    name="$(basename "$def" .def)"   # ex.: avcodec-63
    base="${name%-*}"                # ex.: avcodec
    "$LIBEXE" -nologo -machine:x64 -name:"$name.dll" -def:"$(cygpath -w "$def")" \
        -out:"$(cygpath -w "$PREFIX/lib/$base.lib")" >/dev/null
done
rm -f "$PREFIX"/lib/*.exp "$PREFIX"/bin/*.lib "$PREFIX"/lib/*.def

cp "LICENSE.md" "COPYING.LGPLv2.1" "$PREFIX/" 2>/dev/null || true
cp "COPYING.LGPLv2.1" "$PREFIX/LICENSE.txt"
strip --strip-unneeded "$PREFIX"/bin/*.dll

echo
echo "FFmpeg mínimo pronto em $PREFIX:"
ls -la "$PREFIX/bin"

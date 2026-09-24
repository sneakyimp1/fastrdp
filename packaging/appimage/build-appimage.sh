#!/usr/bin/env bash
# Builds fastrdp-<version>-x86_64.AppImage.
#
# Meant for Ubuntu 24.04 (what the GitHub release workflow runs on); the result runs on
# any x86_64 distribution with glibc 2.39 or newer. FFmpeg, SDL3 and FreeRDP are built
# from source so the AppImage carries a small H.264-only FFmpeg with VAAPI instead of
# Ubuntu's, which drags in every codec under the sun.
#
#   packaging/appimage/build-appimage.sh --install-deps   # first run: apt packages too
#   packaging/appimage/build-appimage.sh                  # rebuild
#
# Environment: VERSION (default: git describe), WORK (default: build-appimage/),
# SKIP_BUNDLE=1 to stop after building (no linuxdeploy).
#
# Keep the dependency versions in step with packaging/flatpak/io.github.sneakyimp1.fastrdp.yml.
set -euo pipefail

FFMPEG_TAG=n8.1.3
SDL_TAG=release-3.4.16
FREERDP_TAG=3.32.0
APP_ID=io.github.sneakyimp1.fastrdp

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$ROOT/build-appimage}
PREFIX=$WORK/prefix
APPDIR=$WORK/AppDir
JOBS=$(nproc)
VERSION=${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}
VERSION=${VERSION#v}

install_deps() {
    local sudo=; [ "$(id -u)" = 0 ] || sudo=sudo
    $sudo apt-get update
    $sudo apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git nasm file patchelf \
        libva-dev libdrm-dev libepoxy-dev libegl-dev libgles-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
        libxss-dev libxtst-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev \
        libssl-dev zlib1g-dev libfuse3-dev libkrb5-dev libcjson-dev libicu-dev \
        qt6-base-dev qt6-wayland qtkeychain-qt6-dev libsecret-1-dev
}

# fetch <url> <tag> <dir>: shallow clone of one tag.
fetch() {
    [ -d "$3" ] || git clone --depth 1 --branch "$2" "$1" "$3"
}

build_ffmpeg() {
    fetch https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_TAG" "$WORK/src/ffmpeg"
    cd "$WORK/src/ffmpeg"
    # Only what fastrdp and FreeRDP use: the H.264 decoder with VAAPI and DRM PRIME
    # export, plus swscale for FreeRDP. LGPL, no external codec libraries.
    ./configure --prefix="$PREFIX" --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-all \
        --enable-avcodec --enable-swscale \
        --enable-decoder=h264 --enable-parser=h264 \
        --enable-vaapi --enable-libdrm --enable-hwaccel=h264_vaapi --disable-xlib
    make -j"$JOBS"
    make install
}

build_sdl() {
    fetch https://github.com/libsdl-org/SDL.git "$SDL_TAG" "$WORK/src/SDL"
    cmake -S "$WORK/src/SDL" -B "$WORK/build/SDL" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
        -DSDL_RPATH=OFF -DSDL_STATIC=OFF
    ninja -C "$WORK/build/SDL" install
}

build_freerdp() {
    fetch https://github.com/FreeRDP/FreeRDP.git "$FREERDP_TAG" "$WORK/src/FreeRDP"
    # WITH_FFMPEG is required even though fastrdp decodes H.264 itself: without an H.264
    # backend FreeRDP never offers AVC420/AVC444 to the server. WITH_FUSE is what lets
    # files copied in Explorer be pasted on Linux.
    cmake -S "$WORK/src/FreeRDP" -B "$WORK/build/FreeRDP" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_TESTING=OFF -DWITH_MANPAGES=OFF -DWITH_SAMPLE=OFF -DWITH_CCACHE=OFF \
        -DWITH_CLANG_FORMAT=OFF -DWITH_SERVER=OFF -DWITH_SHADOW=OFF -DWITH_PROXY=OFF \
        -DWITH_PLATFORM_SERVER=OFF -DWITH_CLIENT=OFF -DWITH_X11=OFF -DWITH_WAYLAND=OFF \
        -DWITH_FFMPEG=ON -DWITH_VIDEO_FFMPEG=ON -DWITH_DSP_FFMPEG=OFF -DWITH_SWSCALE=ON \
        -DWITH_OPENH264=OFF -DWITH_CAIRO=OFF -DWITH_JPEG=OFF \
        -DWITH_PULSE=ON -DWITH_ALSA=OFF -DWITH_OSS=OFF -DWITH_CUPS=OFF -DWITH_FUSE=ON \
        -DWITH_PCSC=ON -DWITH_PKCS11=ON -DWITH_KRB5=ON -DWITH_WEBVIEW=OFF \
        -DCHANNEL_URBDRC=OFF -DCHANNEL_TSMF=OFF
    ninja -C "$WORK/build/FreeRDP" install
}

build_fastrdp() {
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" cmake -S "$ROOT" -B "$WORK/build/fastrdp" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$PREFIX"
    ninja -C "$WORK/build/fastrdp"
    rm -rf "$APPDIR"
    DESTDIR="$APPDIR" ninja -C "$WORK/build/fastrdp" install
}

bundle() {
    # libva is left out on purpose so the host's copy is used. It has to match the host's
    # VAAPI drivers: a libva only loads drivers built for its own API version or older,
    # and it looks for them in the host distribution's driver directory. (libpcsclite is
    # loaded at run time by FreeRDP, so it also comes from the host and matches its pcscd.)
    local tools=$WORK/tools
    mkdir -p "$tools"
    for t in linuxdeploy linuxdeploy-plugin-qt; do
        [ -x "$tools/$t-x86_64.AppImage" ] && continue
        curl -fsSL -o "$tools/$t-x86_64.AppImage" \
            "https://github.com/linuxdeploy/$t/releases/download/continuous/$t-x86_64.AppImage"
        chmod +x "$tools/$t-x86_64.AppImage"
    done

    cd "$WORK"
    export APPIMAGE_EXTRACT_AND_RUN=1           # no FUSE on CI runners
    export LD_LIBRARY_PATH="$PREFIX/lib"
    export QMAKE=/usr/lib/qt6/bin/qmake
    export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
    export EXTRA_QT_MODULES="waylandcompositor"  # the Wayland client plugins
    export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
    "$tools/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
        --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
        --exclude-library 'libva.so*' --exclude-library 'libva-drm.so*' \
        --plugin qt --output appimage
    mv "$WORK"/fastrdp-*.AppImage "$ROOT/" 2>/dev/null || true
    ls -l "$ROOT"/fastrdp-*.AppImage
}

mkdir -p "$WORK/src" "$WORK/build"
[ "${1:-}" = --install-deps ] && install_deps

[ -f "$PREFIX/lib/pkgconfig/libavcodec.pc" ] || build_ffmpeg
[ -f "$PREFIX/lib/pkgconfig/sdl3.pc" ] || build_sdl
[ -f "$PREFIX/lib/pkgconfig/freerdp3.pc" ] || build_freerdp
build_fastrdp
[ "${SKIP_BUNDLE:-0}" = 1 ] || bundle

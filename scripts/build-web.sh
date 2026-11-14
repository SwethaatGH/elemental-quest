#!/usr/bin/env bash
# Builds the browser version of Elemental Quest into build-web/web (the whole website).
#
# Needs the Emscripten SDK on PATH (emcc, emcmake). The GitHub Pages workflow runs this
# script; to run it yourself: https://emscripten.org/docs/getting_started/downloads.html
#
# SDL3 and SDL3_image are built for the browser once and kept in .web-deps/.
set -euo pipefail

SDL_TAG=release-3.4.16
SDL_IMAGE_TAG=release-3.4.6
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${WEB_DEPS_DIR:-$ROOT/.web-deps}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

command -v emcmake >/dev/null || { echo "emcmake not found: activate the Emscripten SDK first" >&2; exit 1; }

if [ ! -f "$DEPS/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    echo "== Building SDL3 ($SDL_TAG) for the browser"
    rm -rf "$DEPS/src/SDL"
    git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$DEPS/src/SDL"
    emcmake cmake -S "$DEPS/src/SDL" -B "$DEPS/src/SDL/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$DEPS/src/SDL/build" -j"$JOBS"
    cmake --install "$DEPS/src/SDL/build"
fi

if [ ! -f "$DEPS/lib/cmake/SDL3_image/SDL3_imageConfig.cmake" ]; then
    echo "== Building SDL3_image ($SDL_IMAGE_TAG) for the browser"
    rm -rf "$DEPS/src/SDL_image"
    git clone --depth 1 --branch "$SDL_IMAGE_TAG" https://github.com/libsdl-org/SDL_image.git "$DEPS/src/SDL_image"
    # PNG and JPEG through the bundled stb decoder; nothing else is needed
    emcmake cmake -S "$DEPS/src/SDL_image" -B "$DEPS/src/SDL_image/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_FIND_ROOT_PATH="$DEPS" \
        -DBUILD_SHARED_LIBS=OFF -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF -DSDLIMAGE_VENDORED=OFF \
        -DSDLIMAGE_BACKEND_STB=ON -DSDLIMAGE_PNG_LIBPNG=OFF -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF
    cmake --build "$DEPS/src/SDL_image/build" -j"$JOBS"
    cmake --install "$DEPS/src/SDL_image/build"
fi

echo "== Building the game for the browser"
emcmake cmake -S "$ROOT" -B "$ROOT/build-web" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_FIND_ROOT_PATH="$DEPS"
cmake --build "$ROOT/build-web" -j"$JOBS"

echo "== Done: the website is in build-web/web"
ls -lh "$ROOT/build-web/web"

# The Elemental Quest game server, for Render (or any Docker host).
# It runs the gateway (server/gateway), which starts one elemental_server per room.

# --- build the C++ game server ---------------------------------------------------------
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config libzmq3-dev cppzmq-dev \
    && rm -rf /var/lib/apt/lists/*

# SDL3 and SDL3_image: the engine links them, though the server never opens a window
RUN git clone --depth 1 --branch release-3.4.16 https://github.com/libsdl-org/SDL.git /src/SDL \
    && cmake -S /src/SDL -B /src/SDL/build -DCMAKE_BUILD_TYPE=Release -DSDL_UNIX_CONSOLE_BUILD=ON \
         -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build /src/SDL/build -j"$(nproc)" && cmake --install /src/SDL/build
RUN git clone --depth 1 --branch release-3.4.6 https://github.com/libsdl-org/SDL_image.git /src/SDL_image \
    && cmake -S /src/SDL_image -B /src/SDL_image/build -DCMAKE_BUILD_TYPE=Release \
         -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF -DSDLIMAGE_VENDORED=OFF -DSDLIMAGE_BACKEND_STB=ON -DSDLIMAGE_PNG_LIBPNG=OFF \
         -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF \
    && cmake --build /src/SDL_image/build -j"$(nproc)" && cmake --install /src/SDL_image/build

WORKDIR /src/game
COPY CMakeLists.txt ./
COPY engine ./engine
COPY game ./game
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target elemental_server -j"$(nproc)"

# --- run the gateway -------------------------------------------------------------------
FROM node:20-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libzmq5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/lib/libSDL3*.so* /usr/local/lib/
RUN ldconfig

WORKDIR /app
COPY server/gateway/package.json server/gateway/package-lock.json ./
RUN npm ci --omit=dev
COPY server/gateway/index.js ./
COPY --from=build /src/game/build/elemental_server ./elemental_server

ENV NODE_ENV=production SERVER_BIN=/app/elemental_server PORT=10000
EXPOSE 10000
CMD ["node", "index.js"]

# Multi-stage build for the lsm-kv TCP server.
# Build:  docker build -t lsmkv-server .
# Run:    docker run --rm -p 7379:7379 -v lsmkv-data:/data lsmkv-server

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY server/ server/

# Tests and examples are not needed in the image.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DLSMKV_BUILD_TESTS=OFF \
        -DLSMKV_BUILD_EXAMPLES=OFF \
        -DLSMKV_BUILD_SERVER=ON \
    && cmake --build build --target lsmkv_server -j"$(nproc)"

FROM debian:bookworm-slim

RUN useradd --system --create-home --uid 10001 lsmkv \
    && mkdir -p /data \
    && chown lsmkv:lsmkv /data

COPY --from=build /src/build/lsmkv_server /usr/local/bin/lsmkv_server

USER lsmkv
WORKDIR /data
EXPOSE 7379
VOLUME ["/data"]

ENTRYPOINT ["lsmkv_server"]
CMD ["--host", "0.0.0.0", "--port", "7379", "--db", "/data"]

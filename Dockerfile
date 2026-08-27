# Multi-stage build: the build stage pulls in a full C++ toolchain and
# CMake, but the final image only ships the compiled server binary — a
# statically-linked-library project like this one has no runtime shared
# objects of its own to carry over, so the runtime stage stays minimal.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    g++ \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Tests and benchmarks aren't needed in the shipped image, and skipping
# them avoids the GoogleTest FetchContent download entirely — a smaller,
# faster, network-independent image build.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DGOREDIS_BUILD_TESTS=OFF \
      -DGOREDIS_BUILD_BENCHMARKS=OFF \
    && cmake --build build -j

FROM debian:bookworm-slim AS runtime

RUN useradd --system --create-home --home-dir /data goredis
COPY --from=build /src/build/src/goredis-server /usr/local/bin/goredis-server

USER goredis
WORKDIR /data
EXPOSE 6380

# --rdb (not --aof): a plain volume mount at /data is enough to persist
# across container restarts either way, but RDB is this project's default
# persistence mode (see README.md) and needs no companion file to exist
# ahead of time the way replaying an --aof log implicitly expects one.
ENTRYPOINT ["goredis-server", "--port", "6380", "--rdb", "/data/dump.grdb"]

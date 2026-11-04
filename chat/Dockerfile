# Build context: the repository root (docker-compose.yml in world: context .., dockerfile chat/Dockerfile), because
# chthonic_chat compiles world's protocol/ and shared server code (service_core.cmake).
# --- deps: identical to world/Dockerfile's (same vcpkg.json), so Docker reuses that cached layer ---
FROM ubuntu:24.04 AS deps
ARG VCPKG_COMMIT=ed8445dd2a9ba44e0e3ace4e952b2e33130082d9
ENV DEBIAN_FRONTEND=noninteractive VCPKG_ROOT=/opt/vcpkg VCPKG_FORCE_SYSTEM_BINARIES=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git curl ca-certificates zip unzip tar pkg-config ninja-build cmake \
      autoconf automake libtool bison flex perl python3 linux-libc-dev libtirpc-dev \
    && rm -rf /var/lib/apt/lists/*
# Full clone: manifest versioning reads port trees from git history.
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT && cd $VCPKG_ROOT \
    && git checkout $VCPKG_COMMIT && ./bootstrap-vcpkg.sh -disableMetrics
WORKDIR /src
COPY world/vcpkg.json ./
RUN $VCPKG_ROOT/vcpkg install --triplet x64-linux-release --host-triplet x64-linux-release \
      --x-install-root=/opt/vcpkg_installed --clean-after-build

# --- build: the shared code from world, then the chat service with warnings-as-errors and its self-check ---
FROM deps AS build
COPY world/service_core.cmake world/
COPY world/protocol world/protocol
COPY world/server world/server
COPY chat chat
WORKDIR /src/chat
RUN cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
    && cmake --build --preset linux-release \
    && ctest --preset linux-release

# --- runtime: chthonic_chat + migrations, non-root (the world image's uid: it reads the world's dev key) ---
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 chat
WORKDIR /app
COPY --from=build /src/chat/build/linux-release/chthonic_chat ./
COPY chat/migrations ./migrations
USER chat
EXPOSE 7778 7779
CMD ["./chthonic_chat"]

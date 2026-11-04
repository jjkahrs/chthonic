# --- deps: vcpkg at the pinned baseline; this layer only rebuilds when vcpkg.json changes ---
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
COPY vcpkg.json ./
RUN $VCPKG_ROOT/vcpkg install --triplet x64-linux-release --host-triplet x64-linux-release \
      --x-install-root=/opt/vcpkg_installed --clean-after-build

# --- build: compile with warnings-as-errors, then run the self-check ---
FROM deps AS build
COPY . .
RUN cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
    && cmake --build --preset linux-release \
    && ctest --preset linux-release

# --- tsan (testing only; docker/tsan.override.yml): chthonic_world (and, in build-tsan, world_self_check) with -fsanitize=thread.
# Stages above the last one are skipped unless targeted, so the default image (runtime, below) is unchanged.
FROM deps AS build-tsan
COPY . .
# -U...GETADDRINFO: httplib's target turns on glibc getaddrinfo_a, whose resolver thread libc starts unseen by TSan
# (its first malloc crashes); the JWKS fetch resolves with plain getaddrinfo in this build.
RUN cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      "-DCMAKE_CXX_FLAGS=-fsanitize=thread -Wno-tsan -UCPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO" \
      -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
    && cmake --build --preset linux-release --target chthonic_world world_self_check

FROM ubuntu:24.04 AS tsan
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libtsan2 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 world
WORKDIR /app
COPY --from=build-tsan /src/build/linux-release/chthonic_world ./
COPY migrations ./migrations
COPY lua ./lua
RUN mkdir -p /app/certs /app/tsan && chown world:world /app/certs /app/tsan
USER world
EXPOSE 7777
# TSan needs a fixed address space layout: setarch -R (ASLR off for the process; needs seccomp=unconfined, set by the override).
ENTRYPOINT ["setarch", "x86_64", "-R"]
CMD ["./chthonic_world"]

# --- runtime: binaries (server, bot, navmesh bake) + migrations + sample Lua, non-root ---
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 world
WORKDIR /app
COPY --from=build /src/build/linux-release/chthonic_world /src/build/linux-release/chthonic_bot \
     /src/build/linux-release/chthonic_mapbake ./
COPY migrations ./migrations
# Sample Lua components and systems (LUA_ROOT); compose bind-mounts ./lua over it.
COPY lua ./lua
# Dev TLS cert lands here when none is configured; mount a volume to keep it.
RUN mkdir -p /app/certs && chown world:world /app/certs
USER world
EXPOSE 7777
CMD ["./chthonic_world"]

# =============================================================================
# Saber Discord Bot - Production Docker Build (uses vcpkg registry)
# =============================================================================
# Build: docker buildx build -t saber --load .
# Run:   docker run --env-file .env --rm saber
# =============================================================================

ARG ALPINE_VERSION=3.23
ARG VCPKG_BASE=jon115/alpine-vcpkg:2025.10.17

# =============================================================================
# Stage 1: Base image with build tools
# =============================================================================
FROM ${VCPKG_BASE} AS base

# Install additional build dependencies
RUN apk add --no-cache \
        autoconf automake bash file libtool \
        meson nasm re2c wget xz && \
    git config --global http.version HTTP/1.1

# =============================================================================
# Stage 2: Install vcpkg dependencies (cached layer)
# =============================================================================
FROM base AS deps

WORKDIR /app
ARG TARGETARCH

# Copy manifest and triplet files
COPY vcpkg.json vcpkg-configuration.json ./
COPY vcpkg-triplets/ /vcpkg-triplets/
COPY overlay-ports/ /overlay-ports/

ENV VCPKG_INSTALLED_DIR=/app/vcpkg_installed

# Install vcpkg dependencies with architecture-specific cache mounts
RUN --mount=type=cache,target=/vcpkg/downloads,id=vcpkg-dl-${TARGETARCH} \
    --mount=type=cache,target=/vcpkg/buildtrees,id=vcpkg-bt-${TARGETARCH} \
    --mount=type=cache,target=/vcpkg/packages,id=vcpkg-pkg-${TARGETARCH} \
    --mount=type=cache,target=/root/.cache/vcpkg,id=vcpkg-rc-${TARGETARCH} \
    rm -rf /vcpkg/downloads/tools && \
    case "${TARGETARCH}" in \
      amd64) TRIPLET="x64-linux-dynamic-release" ;; \
      arm64) TRIPLET="arm64-linux-dynamic-release" ;; \
      *) echo "Unsupported: ${TARGETARCH}" >&2; exit 1 ;; \
    esac && \
    echo "${TRIPLET}" > .vcpkg-triplet && \
    vcpkg install \
        --overlay-ports=/overlay-ports \
        --overlay-triplets=/vcpkg-triplets \
        --triplet="${TRIPLET}" \
        --x-no-default-features \
        --x-feature=boost \
        --x-feature=ffmpeg \
        --x-feature=openssl

# =============================================================================
# Stage 3: Build the application
# =============================================================================
FROM base AS build

WORKDIR /app

# Copy cached vcpkg dependencies
COPY --from=deps /app/vcpkg_installed/ build/vcpkg_installed/
COPY --from=deps /app/.vcpkg-triplet .

# Copy project files
COPY vcpkg-triplets/ /vcpkg-triplets/
COPY overlay-ports/ /overlay-ports/
COPY vcpkg.json vcpkg-configuration.json CMakeLists.txt ./
COPY commands/ commands/
COPY include/ include/
COPY src/ src/

# Configure and build (deps already installed, disable manifest install)
RUN TRIPLET="$(cat .vcpkg-triplet)" && \
    cmake -S . -B build -G Ninja \
        -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
        -DCMAKE_C_COMPILER=/usr/bin/gcc \
        -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
        -DVCPKG_OVERLAY_PORTS=/overlay-ports \
        -DVCPKG_OVERLAY_TRIPLETS=/vcpkg-triplets \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
        -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
        -DCMAKE_INSTALL_RPATH='$ORIGIN' && \
    cmake --build build --config MinSizeRel -j 12

# Copy shared libraries alongside binaries
RUN TRIPLET="$(cat .vcpkg-triplet)" && \
    find "build/vcpkg_installed/${TRIPLET}/lib" -name "*.so*" \
        -exec cp -P {} build/bin/ \; 2>/dev/null || true

# =============================================================================
# Stage 4: Strip binaries for minimal size
# =============================================================================
FROM alpine:${ALPINE_VERSION} AS strip

WORKDIR /out
COPY --from=build /app/build/bin/ ./

RUN apk add --no-cache binutils && \
    find . -type f \( -executable -o -name "*.so*" \) \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find . -name "*.a" -delete 2>/dev/null || true && \
    touch saber.log && chmod 666 saber.log

# =============================================================================
# Stage 5: Final minimal runtime image
# =============================================================================
FROM alpine:${ALPINE_VERSION} AS runtime

LABEL org.opencontainers.image.title="Saber" \
      org.opencontainers.image.description="Discord music bot" \
      org.opencontainers.image.source="https://github.com/115jon/saber"

# Runtime dependencies only
RUN apk add --no-cache ca-certificates libgcc libstdc++ && \
    addgroup -g 65532 -S saber && \
    adduser -u 65532 -S -G saber -H saber

WORKDIR /saber
COPY --from=strip --chown=saber:saber /out/ ./

USER saber

ENV PATH="/saber:${PATH}" \
    LD_LIBRARY_PATH="/saber" \
    TMPDIR="/tmp"

ENTRYPOINT ["/saber/saber"]
# syntax=docker/dockerfile:1.7-labs

FROM ubuntu:20.04 AS build
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

WORKDIR /app
ARG DEBIAN_FRONTEND=noninteractive

# vcpkg location (so CMake/tooling can find it consistently)
ENV CC=/usr/bin/gcc
ENV CXX=/usr/bin/g++
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=${PATH}:${VCPKG_ROOT}

RUN <<'EOF'
set -euxo pipefail
apt-get update
apt-get install -y --no-install-recommends \
  autoconf automake build-essential cmake git libtool ninja-build \
  ca-certificates curl zip unzip tar pkg-config \
  wget xz-utils file
rm -rf /var/lib/apt/lists/*
EOF

RUN <<'EOF'
set -euxo pipefail
if [ ! -d "${VCPKG_ROOT}" ]; then
  git clone --depth=1 https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
fi
cd "${VCPKG_ROOT}"
./bootstrap-vcpkg.sh
EOF

RUN --mount=type=bind,source=commands,target=commands \
    --mount=type=bind,source=include,target=include \
    --mount=type=bind,source=extern,target=extern \
    --mount=type=bind,source=src,target=src \
    --mount=type=bind,source=CMakeLists.txt,target=CMakeLists.txt \
    --mount=type=bind,source=vcpkg-configuration.json,target=vcpkg-configuration.json \
    --mount=type=bind,source=vcpkg.json,target=vcpkg.json \
    <<'EOF'
set -euxo pipefail

cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# Optional: strip to reduce size of copied artifacts
strip -s build/bin/* 2>/dev/null || true
strip -s build/lib/* 2>/dev/null || true

mkdir -p /saber
cp -a build/bin/* /saber/ 2>/dev/null || true
cp -a build/lib/* /saber/ 2>/dev/null || true

touch /saber/saber.log
chmod 666 /saber/saber.log
EOF

RUN <<'EOF'
set -euxo pipefail
ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then ARCH='amd64'; fi
if [ "$ARCH" = "aarch64" ]; then ARCH='arm64'; fi

wget -q "https://johnvansickle.com/ffmpeg/builds/ffmpeg-git-${ARCH}-static.tar.xz" -O /tmp/ffmpeg.tar.xz
mkdir -p /tmp/ffmpeg
tar -xJf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg

install -m 0755 "$(find /tmp/ffmpeg -type f -name ffmpeg  | head -n1)" /saber/ffmpeg
install -m 0755 "$(find /tmp/ffmpeg -type f -name ffprobe | head -n1)" /saber/ffprobe

if [ "$ARCH" = "amd64" ]; then
  wget -q 'https://github.com/yt-dlp/yt-dlp/releases/download/2025.12.08/yt-dlp_linux' -O /saber/yt-dlp
else
  wget -q "https://github.com/yt-dlp/yt-dlp/releases/download/2025.12.08/yt-dlp_linux_${ARCH}" -O /saber/yt-dlp
fi
chmod 0755 /saber/yt-dlp

rm -rf /tmp/*
EOF

FROM debian:bookworm-slim AS runtime
ARG DEBIAN_FRONTEND=noninteractive

RUN <<'EOF'
set -eux
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates \
  libstdc++6 \
  libgcc-s1
rm -rf /var/lib/apt/lists/*
EOF

WORKDIR /saber
COPY --from=build /saber/ /saber/

ENV PATH=/saber:${PATH}
# Ensure your own shared libs in /saber are discoverable (safe now that we are not vendoring glibc libs).
ENV LD_LIBRARY_PATH=/saber
ENV SSL_CERT_DIR=/etc/ssl/certs
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt

CMD ["./saber"]

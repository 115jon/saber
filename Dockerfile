FROM ubuntu:20.04 AS build

WORKDIR /app

ARG DEBIAN_FRONTEND=noninteractive

# vcpkg location (so CMake/tooling can find it consistently)
ENV CC=/usr/bin/gcc
ENV CXX=/usr/bin/g++
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH=${PATH}:${VCPKG_ROOT}

RUN --mount=type=bind,source=commands,target=commands \
    --mount=type=bind,source=include,target=include \
    --mount=type=bind,source=overlay-ports,target=overlay-ports \
    --mount=type=bind,source=src,target=src \
    --mount=type=bind,source=CMakeLists.txt,target=CMakeLists.txt \
    --mount=type=bind,source=vcpkg-configuration.json,target=vcpkg-configuration.json \
    --mount=type=bind,source=vcpkg.json,target=vcpkg.json \
    <<EOF
    set -ex
    apt-get update
    apt-get install -y \
        autoconf build-essential cmake git libtool ninja-build \
        ca-certificates curl zip unzip tar pkg-config

    # Install vcpkg (clone + bootstrap)
    if [ ! -d "${VCPKG_ROOT}" ]; then
        git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
    fi
    cd "${VCPKG_ROOT}"
    ./bootstrap-vcpkg.sh
    cd /app

    # If you want vcpkg dependencies wired into your build, keep this toolchain flag:
    cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

    cmake --build build --config Release

    mkdir /saber
    cp build/bin/* build/lib/* /saber
    cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /saber
    cp /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 /saber
    cp /lib/x86_64-linux-gnu/libdl.so.2 /saber
    cp /lib/x86_64-linux-gnu/librt.so.1 /saber
    touch /saber/saber.log
    chmod 777 /saber/saber.log
EOF

RUN <<EOF
    set -ex
    apt-get update
    apt-get install -y wget
    export ARCH=`uname -m`

    if [ "$ARCH" = "x86_64" ]; then ARCH='amd64'; fi
    if [ "$ARCH" = "aarch64" ]; then ARCH='arm64'; fi
    wget -q "https://johnvansickle.com/ffmpeg/builds/ffmpeg-git-${ARCH}-static.tar.xz" -O - | tar -xJ -C /tmp/ --one-top-level=ffmpeg
    chmod -R a+x /tmp/ffmpeg/*
    mv $(find /tmp/ffmpeg/* -name ffmpeg) /saber/
    mv $(find /tmp/ffmpeg/* -name ffprobe) /saber/
    rm -rf /tmp/*

    if [ "$ARCH" = "amd64" ]; then
        wget -q 'https://github.com/yt-dlp/yt-dlp/releases/download/2025.12.08/yt-dlp_linux' -O /saber/yt-dlp
        chmod a+x /saber/yt-dlp
    else
        wget -q "https://github.com/yt-dlp/yt-dlp/releases/download/2025.12.08/yt-dlp_linux_${ARCH}" -O /saber/yt-dlp
        chmod a+x /saber/yt-dlp
    fi
EOF

RUN <<EOF
    cp /lib/x86_64-linux-gnu/libpthread.so.0 /saber
EOF

WORKDIR /saber
ENV PATH=${PATH}:.
ENV SSL_CERT_DIR=/etc/ssl/certs
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt

CMD ["./saber"]

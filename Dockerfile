# syntax=docker/dockerfile:1

FROM debian:13 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG DEBIAN_MIRROR=http://deb.debian.org/debian
ARG DEBIAN_SECURITY_MIRROR=http://deb.debian.org/debian-security
ARG VCPKG_ROOT=/opt/vcpkg
ARG KVAI_DOCKER_OPTIONAL_BACKENDS=ON

RUN sed -i "s|http://deb.debian.org/debian-security|${DEBIAN_SECURITY_MIRROR}|g; s|http://deb.debian.org/debian|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources && \
    apt-get update && apt-get install -y \
    ca-certificates \
    git \
    curl \
    zip \
    unzip \
    tar \
    xz-utils \
    build-essential \
    pkg-config \
    ninja-build \
    nlohmann-json3-dev \
    protobuf-compiler \
    libprotobuf-dev \
    autoconf \
    automake \
    autoconf-archive \
    libtool \
    bison \
    flex \
    perl \
    python3 \
    python3-pip \
    python3-venv \
    gfortran \
    nasm \
    yasm && \
    rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/bootstrap && \
    /opt/bootstrap/bin/python -m pip install --upgrade pip cmake ninja && \
    ln -sf /opt/bootstrap/bin/cmake /usr/local/bin/cmake && \
    ln -sf /opt/bootstrap/bin/ctest /usr/local/bin/ctest && \
    ln -sf /opt/bootstrap/bin/cpack /usr/local/bin/cpack && \
    ln -sf /opt/bootstrap/bin/ninja /usr/local/bin/ninja

RUN if [ "${KVAI_DOCKER_OPTIONAL_BACKENDS}" = "ON" ]; then \
        git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" && \
        "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics; \
    else \
        mkdir -p "${VCPKG_ROOT}" /workspace/vcpkg_installed; \
    fi

WORKDIR /workspace
COPY vcpkg.json .
COPY vcpkg-configuration.json .
COPY vcpkg-overlay ./vcpkg-overlay

RUN --mount=type=bind,from=vcpkg_binary_cache,target=/opt/vcpkg-binary-cache,ro \
    --mount=type=cache,id=rangeveckv-vcpkg-archives,target=/root/.cache/vcpkg/archives \
    --mount=type=cache,id=rangeveckv-vcpkg-downloads,target=/opt/vcpkg/downloads \
    if [ "${KVAI_DOCKER_OPTIONAL_BACKENDS}" = "ON" ]; then \
        VCPKG_BINARY_SOURCES="clear;files,/opt/vcpkg-binary-cache,read;files,/root/.cache/vcpkg/archives,readwrite" \
        "${VCPKG_ROOT}/vcpkg" install \
            --triplet x64-linux \
            --x-manifest-root=/workspace \
            --x-install-root=/workspace/vcpkg_installed \
            --clean-after-build; \
    fi

COPY . .

RUN if [ "${KVAI_DOCKER_OPTIONAL_BACKENDS}" = "ON" ]; then \
        cmake -S . -B build/docker -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DKVAI_ENABLE_PACKAGING=ON \
            -DVCPKG_INSTALLED_DIR=/workspace/vcpkg_installed \
            -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake; \
    else \
        cmake -S . -B build/docker -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DKVAI_ENABLE_OPTIONAL_BACKENDS=OFF \
            -DKVAI_ENABLE_PACKAGING=ON; \
    fi && \
    cmake --build build/docker --parallel && \
    ctest --test-dir build/docker --output-on-failure && \
    cmake --install build/docker --prefix /opt/rangeveckv

FROM debian:13-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG DEBIAN_MIRROR=http://deb.debian.org/debian
ARG DEBIAN_SECURITY_MIRROR=http://deb.debian.org/debian-security

RUN sed -i "s|http://deb.debian.org/debian-security|${DEBIAN_SECURITY_MIRROR}|g; s|http://deb.debian.org/debian|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources && \
    apt-get update && apt-get install -y \
    ca-certificates \
    curl \
    libprotobuf32t64 \
    libgfortran5 \
    libgomp1 \
    libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/rangeveckv
COPY --from=build /opt/rangeveckv /opt/rangeveckv

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["/opt/rangeveckv/bin/kvai_server"]
CMD ["--config", "/opt/rangeveckv/share/rangeveckv/config/server.docker.yaml", "--serve"]

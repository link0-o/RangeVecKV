FROM debian:13 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG VCPKG_ROOT=/opt/vcpkg

RUN apt-get update && apt-get install -y \
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

RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" && \
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /workspace
COPY . .

RUN cmake -S . -B build/docker -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DKVAI_ENABLE_PACKAGING=ON \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake && \
    cmake --build build/docker --parallel && \
    ctest --test-dir build/docker --output-on-failure && \
    cmake --install build/docker --prefix /opt/rangeveckv

FROM debian:13-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    ca-certificates \
    curl \
    libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/rangeveckv
COPY --from=build /opt/rangeveckv /opt/rangeveckv

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["/opt/rangeveckv/bin/kvai_server"]
CMD ["--config", "/opt/rangeveckv/share/rangeveckv/config/server.yaml", "--serve"]
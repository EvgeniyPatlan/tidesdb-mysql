# TIDES builder image — Ubuntu 24.04 with MySQL 9.7 + TidesDB build deps.
#
# Usage:
#   docker build -t tides-builder /home/corvin/TIDES/docker
#   docker run --rm -v /home/corvin/TIDES:/work tides-builder /work/scripts/build-tidesdb.sh
#   docker run --rm -v /home/corvin/TIDES:/work tides-builder /work/scripts/build-plugin.sh
#
# The host repo is mounted at /work; all build outputs land under /work/<subdir>/build.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Core toolchain
    build-essential \
    cmake \
    ninja-build \
    bison \
    pkg-config \
    git \
    ca-certificates \
    # MySQL 9.7 native deps (from CLAUDE.md)
    libssl-dev \
    libncurses-dev \
    libreadline-dev \
    libudev-dev \
    libldap2-dev \
    libsasl2-dev \
    libkrb5-dev \
    libcurl4-openssl-dev \
    libtirpc-dev \
    # TidesDB compression deps
    libzstd-dev \
    liblz4-dev \
    libsnappy-dev \
    # Debugging
    gdb \
    procps \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work

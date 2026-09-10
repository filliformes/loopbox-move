FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build

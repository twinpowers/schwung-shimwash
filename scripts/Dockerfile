# Build environment for the Shimwash module.
# ubuntu:22.04 pinned deliberately: glibc 2.35, matching Ableton Move exactly.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev binutils file python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build

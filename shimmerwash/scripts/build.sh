#!/usr/bin/env bash
# Build Shimwash for the Ableton Move (aarch64) with Docker.
#
#   ./scripts/build.sh
#
# Runs every gate (see docker-build.sh) and leaves the installable module in
# dist/shimmerwash/ and dist/shimmerwash-module.tar.gz.
#
# The container is forced to linux/arm64: on an Apple-silicon Mac that is
# native and fast, and the binary the gates test IS the binary that ships.
# (On an Intel machine Docker emulates arm64; it works, it is just slower.)
set -euo pipefail
cd "$(dirname "$0")/.."

command -v docker >/dev/null || { echo "Docker is not installed or not on PATH." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "Docker is installed but not running. Open Docker Desktop and wait for it to settle." >&2; exit 1; }

docker build --platform linux/arm64 -t shimmerwash-builder -f scripts/Dockerfile scripts/
docker run --rm --platform linux/arm64 \
    -e PYTHONDONTWRITEBYTECODE=1 \
    -v "$PWD":/build -u "$(id -u):$(id -g)" -w /build \
    shimmerwash-builder ./scripts/docker-build.sh

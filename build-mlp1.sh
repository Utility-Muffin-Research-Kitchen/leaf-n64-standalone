#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER="${DOCKER:-docker}"
TOOLCHAIN_IMAGE="${TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"
MLP1_SRC="${MLP1_SRC:-$ROOT_DIR/workdir/mlp1/src}"
MLP1_BUILD_DIR="${MLP1_BUILD_DIR:-$ROOT_DIR/output/mlp1/build}"
BUILD_JOBS="${BUILD_JOBS:-}"
MLP1_BUILD_GLIDEN64="${MLP1_BUILD_GLIDEN64:-0}"

if ! "$DOCKER" image inspect "$TOOLCHAIN_IMAGE" >/dev/null 2>&1; then
    echo "missing Docker image: $TOOLCHAIN_IMAGE" >&2
    echo "build it with: make -C ../mlp1-toolchain image" >&2
    exit 1
fi

# Reuse the upstream source/patch inventory, but keep generated source trees in
# the MLP1 workdir instead of the repo root.
make -C "$ROOT_DIR" clone patch SRC="$MLP1_SRC"

"$DOCKER" run --rm \
    -v "$ROOT_DIR":/build \
    -w /build \
    -e MLP1_SRC=/build/workdir/mlp1/src \
    -e MLP1_BUILD_DIR=/build/output/mlp1/build \
    -e BUILD_JOBS="$BUILD_JOBS" \
    -e MLP1_BUILD_GLIDEN64="$MLP1_BUILD_GLIDEN64" \
    "$TOOLCHAIN_IMAGE" \
    bash /build/scripts/build-mlp1-in-docker.sh

find "$MLP1_BUILD_DIR" -maxdepth 3 -type f | sort

#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MOARVM_PREFIX=${MOARVM_PREFIX:-"$PROJECT_DIR/.local/moarvm"}
VERSION=2026.08
if [ -x "$MOARVM_PREFIX/bin/moar" ]; then
    "$MOARVM_PREFIX/bin/moar" --version
    exit 0
fi
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/spo-moarvm.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM
curl -fL "https://www.moarvm.org/releases/MoarVM-$VERSION.tar.gz" -o "$WORK_DIR/source.tar.gz"
EXPECTED_SHA256=805154e842baeb0a56194ed98c66a6fc94546a6dab8b3ffb982ebe97d7080a7a
ACTUAL_SHA256=$(shasum -a 256 "$WORK_DIR/source.tar.gz" | awk '{print $1}')
if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
    echo 'MoarVM source checksum mismatch' >&2
    exit 1
fi
mkdir "$WORK_DIR/source"
tar -xzf "$WORK_DIR/source.tar.gz" -C "$WORK_DIR/source" --strip-components=1
cd "$WORK_DIR/source"
perl Configure.pl --prefix="$MOARVM_PREFIX"
make -j "${JOBS:-4}"
make install
"$MOARVM_PREFIX/bin/moar" --version

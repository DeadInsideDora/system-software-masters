#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${MOAR:-}" ]; then
    exec "$MOAR" "$@"
fi
if [ -x "$PROJECT_DIR/.local/moarvm/bin/moar" ]; then
    exec "$PROJECT_DIR/.local/moarvm/bin/moar" "$@"
fi
if command -v moar >/dev/null 2>&1; then
    exec moar "$@"
fi
echo 'MoarVM not found. Run ./tools/install_moarvm.sh or set MOAR=/path/to/moar.' >&2
exit 127

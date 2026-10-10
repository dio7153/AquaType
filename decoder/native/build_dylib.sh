#!/usr/bin/env bash
# Build libaquadecoder.dylib, the decoder the engine links (C API: aquadecoder.h), from the five sources it is made of.
# MLX's headers and library come from the Python environment's mlx package, the same release the app ships.
# Usage: decoder/native/build_dylib.sh [--python PATH]      (default: .venv/bin/python, then python3)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; PY="${PYTHON:-}"
while [[ $# -gt 0 ]]; do case "$1" in --python) PY="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
if [[ -z "$PY" ]]; then if [[ -x "$ROOT/.venv/bin/python" ]]; then PY="$ROOT/.venv/bin/python"; else PY="$(command -v python3)"; fi; fi
MLXDIR="$("$PY" -c 'import mlx, os, sys; p = [x for x in sys.path if os.path.isdir(os.path.join(x, "mlx", "include"))]; print(os.path.join(p[0], "mlx"))')" \
  || { echo "no mlx package in $PY (pip install mlx)" >&2; exit 1; }
COMMIT="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY="$(git -C "$HERE" status --porcelain --untracked-files=no -- . 2>/dev/null | grep -q . && echo '-dirty' || true)"
VER="${COMMIT}${DIRTY}+$(date -u +%Y%m%d)"
"${CXX:-clang++}" -O3 -std=c++17 -fobjc-arc -fPIC -DDECODER_NATIVE_VERSION=\"$VER\" -DAQ_WITH_MLX -I "$MLXDIR/include" \
  -dynamiclib "$HERE/edit.cpp" "$HERE/enumerate.cpp" "$HERE/v2_model.cpp" "$HERE/v2_session.cpp" "$HERE/aquadecoder.cpp" \
  -L "$MLXDIR/lib" -lmlx -Wl,-rpath,"$MLXDIR/lib" -framework Metal -framework MetalPerformanceShaders -framework Foundation \
  -install_name @rpath/libaquadecoder.dylib -o "$HERE/libaquadecoder.dylib"
echo "built $HERE/libaquadecoder.dylib ($VER)"

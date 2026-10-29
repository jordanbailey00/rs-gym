#!/usr/bin/env bash
# Hooks RS-gym into a PufferLib 5.0 checkout as the env "rs_gym".
#
#   tools/puffer_overlay.sh /path/to/PufferLib
#
# 1. Builds build/puffer/librs_gym_puffer.a: the engine, content, rc-env and the
#    puf_* binding, compiled against that checkout's pufferenv.h.
# 2. Links ocean/rs_gym -> rc-env/puffer and config/rs_gym.ini into the checkout
#    (symlinks only; nothing in the checkout is overwritten).
# 3. Prints the PufferLib build command, which links the library through NVCC_EXTRA.
set -euo pipefail

PUFFER=${1:?usage: tools/puffer_overlay.sh /path/to/PufferLib}
PUFFER=$(cd "$PUFFER" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$PUFFER/src/pufferenv.h" ] || { echo "error: $PUFFER/src/pufferenv.h not found (PufferLib 5.0?)" >&2; exit 1; }
[ -f "$ROOT/data/regions/gym.cfg" ] || { echo "error: build the gym first: python3 tools/build_gym.py" >&2; exit 1; }

RAYLIB_INCLUDE=$(ls -d "$PUFFER"/raylib-*/include 2>/dev/null | head -1 || true)
cmake -S "$ROOT" -B "$ROOT/build/puffer" -DCMAKE_BUILD_TYPE=Release \
    -DRS_GYM_PUFFERLIB="$PUFFER" -DRS_GYM_RAYLIB_INCLUDE="${RAYLIB_INCLUDE:-}" >/dev/null
cmake --build "$ROOT/build/puffer" -j"$(nproc)" --target rs_gym_puffer

for link in "ocean/rs_gym:$ROOT/rc-env/puffer" "config/rs_gym.ini:$ROOT/rc-env/puffer/rs_gym.ini"; do
    dst="$PUFFER/${link%%:*}"
    src="${link#*:}"
    if [ -e "$dst" ] && [ ! -L "$dst" ]; then
        echo "error: $dst exists and is not a symlink; leaving it alone" >&2
        exit 1
    fi
    ln -sfn "$src" "$dst"
done

cat <<EOF
RS-gym is linked into $PUFFER as env "rs_gym". Build and train from there:

  cd $PUFFER
  export RUNEC_DATA_ROOT=$ROOT/data
  NVCC_EXTRA="-I$ROOT/rc-env -L$ROOT/build/puffer -lrs_gym_puffer -lz -lpthread" ./build.sh rs_gym

Then train with the checkout's usual trainer command for an env (config: config/rs_gym.ini).
EOF

#!/usr/bin/env bash
# Hooks RS-gym into a PufferLib 5.0 checkout as two envs: "rs_gym_compact" (the compact_osrs_v1
# mode, schema compact-v2) and "rs_gym" (the island mode's v1 interface).
#
#   tools/puffer_overlay.sh /path/to/PufferLib
#
# 1. Builds build/puffer/librs_gym_compact_puffer.a and librs_gym_puffer.a: the engine, content,
#    rc-env and each puf_* binding, compiled against that checkout's pufferenv.h.
# 2. Links ocean/<env> and config/<env>.ini into the checkout (symlinks only; nothing in the
#    checkout is overwritten).
# 3. Prints the PufferLib build commands, which link the libraries through NVCC_EXTRA.
set -euo pipefail

PUFFER=${1:?usage: tools/puffer_overlay.sh /path/to/PufferLib}
PUFFER=$(cd "$PUFFER" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$PUFFER/src/pufferenv.h" ] || { echo "error: $PUFFER/src/pufferenv.h not found (PufferLib 5.0?)" >&2; exit 1; }
[ -f "$ROOT/data/modes/compact_osrs_v1/gym.cfg" ] || {
    echo "error: build the mode first: python3 tools/build_mode.py compact_osrs_v1" >&2; exit 1; }

RAYLIB_INCLUDE=$(ls -d "$PUFFER"/raylib-*/include 2>/dev/null | head -1 || true)
cmake -S "$ROOT" -B "$ROOT/build/puffer" -DCMAKE_BUILD_TYPE=Release \
    -DRS_GYM_PUFFERLIB="$PUFFER" -DRS_GYM_RAYLIB_INCLUDE="${RAYLIB_INCLUDE:-}" >/dev/null
cmake --build "$ROOT/build/puffer" -j"$(nproc)" --target rs_gym_compact_puffer rs_gym_puffer

for link in "ocean/rs_gym_compact:$ROOT/rc-env/puffer/compact" \
            "config/rs_gym_compact.ini:$ROOT/rc-env/puffer/compact/rs_gym_compact.ini" \
            "ocean/rs_gym:$ROOT/rc-env/puffer" "config/rs_gym.ini:$ROOT/rc-env/puffer/rs_gym.ini"; do
    dst="$PUFFER/${link%%:*}"
    src="${link#*:}"
    if [ -e "$dst" ] && [ ! -L "$dst" ]; then
        echo "error: $dst exists and is not a symlink; leaving it alone" >&2
        exit 1
    fi
    ln -sfn "$src" "$dst"
done

cat <<EOF
RS-gym is linked into $PUFFER as envs "rs_gym_compact" (compact_osrs_v1) and "rs_gym" (island).
Build and train from there:

  cd $PUFFER
  export RUNEC_DATA_ROOT=$ROOT/data
  NVCC_EXTRA="-I$ROOT/rc-env -L$ROOT/build/puffer -lrs_gym_compact_puffer -lz -lpthread" ./build.sh rs_gym_compact
  NVCC_EXTRA="-I$ROOT/rc-env -L$ROOT/build/puffer -lrs_gym_puffer -lz -lpthread" ./build.sh rs_gym

Then train with the checkout's usual trainer command (config: config/rs_gym_compact.ini).
EOF

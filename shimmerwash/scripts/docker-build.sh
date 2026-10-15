#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 container. Do not run it on a host with a newer
# glibc: the artifact would not load on the Move (glibc 2.35).
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
ARCH_FLAGS=""
if [ "$(uname -m)" = "aarch64" ]; then
    ARCH_FLAGS="-march=armv8-a -mtune=cortex-a72"    # Move is a Cortex-A72
fi

# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of the clamps on the realtime path.
echo "=== build ==="
mkdir -p build
$CC -O3 -std=gnu11 -Wall -Wextra $ARCH_FLAGS -shared -fPIC -Isrc \
    -o build/shimmerwash.so src/shimmerwash.c -lm
ls -l build/shimmerwash.so
file build/shimmerwash.so

# The gates below exist because each failure mode is invisible at runtime: a
# stale module.json still loads (the host just never offers the knob), and a
# help.json in the wrong shape is silently dropped.
echo "=== module.json generated from the plugin's own chain_params ==="
python3 tools/gen_module_json.py build/shimmerwash.so
python3 tools/gen_module_json.py build/shimmerwash.so --check

echo "=== help.json is in the shape the device reads ==="
python3 tools/check_help.py

echo "=== contract + behavior gate (the real .so, loaded like the host does) ==="
python3 tools/check_contract.py build/shimmerwash.so src/module.json

echo "=== DSP behavior report (informational) ==="
$CC -O2 -Isrc -o build/dsp_suite tests/dsp_suite.c -lm
./build/dsp_suite || true

# A newer glibc symbol makes dlopen fail with an error the chain host swallows:
# you would see a dead slot and no message.
echo "=== glibc symbol check (must be <= 2.35) ==="
MAXV=$(objdump -T build/shimmerwash.so | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1 || true)
echo "  highest required: ${MAXV:-none}"
if [ -n "${MAXV:-}" ]; then
    HIGH=$(printf '%s\nGLIBC_2.35\n' "$MAXV" | sort -uV | tail -1)
    [ "$HIGH" = "GLIBC_2.35" ] || { echo "FAIL: needs $MAXV, Move has 2.35"; exit 1; }
fi
echo "  ok"

echo "=== package ==="
rm -rf dist/shimmerwash
mkdir -p dist/shimmerwash
cp build/shimmerwash.so dist/shimmerwash/
cp src/module.json src/help.json LICENSE dist/shimmerwash/
chmod 755 dist/shimmerwash/shimmerwash.so
chmod 644 dist/shimmerwash/module.json dist/shimmerwash/help.json dist/shimmerwash/LICENSE
(cd dist && tar -czf shimmerwash-module.tar.gz shimmerwash/)
ls -l dist/shimmerwash dist/shimmerwash-module.tar.gz
echo
echo "BUILD OK -> dist/shimmerwash/  and  dist/shimmerwash-module.tar.gz"

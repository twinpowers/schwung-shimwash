#!/usr/bin/env bash
# Install Shimwash on the Move SAFELY.
#
#   ./scripts/deploy.sh [host]            (default host: move.local)
#   ./scripts/deploy.sh move.local --reboot
#
# Never scp directly over a live .so. The Schwung host dlopen()s it, so
# overwriting the file in place changes the code pages of a running process,
# which can take the whole firmware down. Upload under a temporary name, then
# mv: rename(2) is atomic and leaves the old file intact for the running
# process. A reboot is then what makes the Move load the new build.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:-move.local}"
DEST="/data/UserData/schwung/modules/audio_fx/shimmerwash"
SRC="dist/shimmerwash"

for f in shimmerwash.so module.json help.json; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f - run ./scripts/build.sh first" >&2; exit 1; }
done

echo "==> $HOST:$DEST"
ssh "root@$HOST" "mkdir -p $DEST"
for f in shimmerwash.so module.json help.json; do
    scp -q "$SRC/$f" "root@$HOST:$DEST/$f.new"
done
ssh "root@$HOST" "cd $DEST && \
    mv -f shimmerwash.so.new shimmerwash.so && \
    mv -f module.json.new module.json && \
    mv -f help.json.new help.json && \
    chmod 755 shimmerwash.so && rm -f dsp.so && ls -l"

if [ "${2:-}" = "--reboot" ]; then
    echo "==> rebooting the Move"
    ssh "root@$HOST" reboot || true
else
    echo
    echo "Copied. Reboot the Move to load the new build:"
    echo "  ssh root@$HOST reboot"
fi

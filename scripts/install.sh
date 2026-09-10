#!/bin/bash
set -e
MODULE_ID="loopbox"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators"

if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh root@$MOVE_HOST "mkdir -p $DEST/$MODULE_ID"
scp -r "dist/$MODULE_ID/"* "root@$MOVE_HOST:$DEST/$MODULE_ID/"
ssh root@$MOVE_HOST "chown -R ableton:users $DEST/$MODULE_ID && chmod +x $DEST/$MODULE_ID/dsp.so"
echo "Done. Restart Move to load the module."

#!/usr/bin/env bash
# Deploy the shell to a connected Move over SSH, by ATOMIC RENAME.
# Writing straight over a mapped .so truncates it under the live mapping and
# crashes Move's audio process; staging + `mv` within /data is atomic, so a
# loaded instance keeps its old mapping until reloaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules/overtake/loopex

[ -f dist/loopex/dsp.so ] || { echo "no dist/loopex/dsp.so — run scripts/build.sh first"; exit 1; }

echo "staging to $MOVE_HOST ..."
scp dist/loopex/dsp.so dist/loopex/module.json dist/loopex/ui.js "$MOVE_HOST:/data/UserData/"

echo "atomic move into place ..."
ssh "$MOVE_HOST" "set -e
  D=$DEST; S=/data/UserData
  mkdir -p \"\$D\"
  mv -f \"\$S/dsp.so\"      \"\$D/dsp.so\"
  mv -f \"\$S/module.json\" \"\$D/module.json\"
  mv -f \"\$S/ui.js\"       \"\$D/ui.js\"
  echo 'installed:'; ls -l \"\$D\""

echo
echo "Done. On the Move: rescan modules (or Schwung Manager), then open LPX Shell"
echo "from the Overtake/Tools list. If it was already loaded, FULL-EXIT first"
echo "(Shift + Volume + Jog-click; a plain Back only suspends) so suspend_keeps_js"
echo "doesn't resume the old code."

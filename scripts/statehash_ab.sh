#!/bin/sh
# Statehash A/B between two kestrel64 builds: same ROMs, same caps, both RCP modes.
# A host-only change (refactor, PGO, code layout) must print SAME on every line.
#
#   sh scripts/statehash_ab.sh <exe A> <exe B> [rom=MAXINSN ...]
#
# Defaults to junkrunner64 (libdragon 3D homebrew: CPU+RSP+RDP+audio) at 400 M instructions.
# ROM paths are relative to KESTREL_TESTROMS (scripts/local.env) unless absolute.
# Rendezvous are made deterministic with KESTREL_SPLEAD=0 KESTREL_DPLOGLEAD=0, and each ROM
# runs with KESTREL_THREADS=0 (lockstep) and 1 (threaded).
# A game that writes its save file (EEPROM next to the ROM) boots differently the first time
# it runs on a machine; if only the very first line differs, run the script again.
set -u
cd "$(dirname "$0")/.."
. scripts/localenv.sh
A=$1; B=$2; shift 2
[ $# -eq 0 ] && set -- homebrew/junkrunner64.z64=400000000
rc=0
hash() { # hash <exe> <rom> <maxinsn> <threads>
  env KESTREL_LOADSTATE=0 KESTREL_THROTTLE=0 KESTREL_SPLEAD=0 KESTREL_DPLOGLEAD=0 \
      KESTREL_PRDP=0 KESTREL_THREADS=$4 KESTREL_MAXINSN=$3 \
      timeout 120 "$1" --run "$2" 2>&1 | tr -d '\0' \
    | grep -oE "\[statehash\] [0-9a-f]+" | tail -1 | cut -d' ' -f2
}
for spec in "$@"; do
  rom=${spec%=*}; insn=${spec##*=}
  case $rom in /*|[A-Za-z]:*) r=$rom ;; *) r="$KESTREL_TESTROMS/$rom" ;; esac
  for t in 0 1; do
    ha=$(hash "$A" "$r" "$insn" $t); hb=$(hash "$B" "$r" "$insn" $t)
    if [ -n "$ha" ] && [ "$ha" = "$hb" ]; then v=SAME; else v=DIFF; rc=1; fi
    echo "$v  $(basename "$rom") THREADS=$t  A=${ha:-none} B=${hb:-none}"
  done
done
exit $rc

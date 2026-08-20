#!/usr/bin/env bash
# Medida de velocidad SM64: N buffer swaps a reloj de pared. Un numero comparable
# entre builds (mismo ROM, mismo arranque). Borra el .eep antes: un save viejo
# cambia el camino de arranque y con el la carga de trabajo.
set -u
export PATH=/c/msys64/clang64/bin:$PATH
ROM=${KROM:-"/e/Claude/N64/test_roms/Super Mario 64 (USA).z64"}
SAVE="${ROM%.*}.eep"
FLIPS=${KFLIPS:-500}
EXE=${KEXE:-./build/kestrel64.exe}
TAG=${1:-run}
shift 2>/dev/null || true
rm -f "$SAVE"
s=$(date +%s%N)
env "$@" KESTREL_NOVIDEO=1 KESTREL_MAXFLIPS=$FLIPS KESTREL_HANGDOG=${KHANG:-400} \
    "$EXE" "$ROM" --run > /tmp/perf-$TAG.log 2>&1
e=$(date +%s%N)
ms=$(( (e-s)/1000000 ))
insn=$(grep -o '[0-9]*M insns' /tmp/perf-$TAG.log | head -1)
bad=$(grep -c 'rdp!\|hangdog' /tmp/perf-$TAG.log)
printf '%-16s %6dms  %7.2f swaps/s  %s  anom=%s\n' "$TAG" $ms \
  "$(awk "BEGIN{print $FLIPS*1000/$ms}")" "${insn:-?}" "$bad"
rm -f "$SAVE"

#!/bin/sh
# Tanda de pdbench.py sobre varias ROMs de PD (PD64_pending P6). Una fila markdown por ROM.
# Uso: sh scripts/pdbench.sh [--mode lockstep|threaded] [--stage villa] rom[=map] ...
#   rom=map  map explicito (p.ej. v2-00-base.z64 comparte map con v2-01)
#   sin map  pdbench.py busca pd.map junto a la ROM, luego <rom sin .z64>.map
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
HERE=$(cd "$(dirname "$0")" && pwd)
ARGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mode|--stage|--skip|--measure|--exe|--port|--prof) ARGS="$ARGS $1 $2"; shift 2 ;;
    *) break ;;
  esac
done
mkdir -p out
echo "| rom | md5 | stage | mode | fps | gclk (ms RDP invitado = % del frame) | invitado % RDP/RSP (sondeo)/CPU-ocio | px IM_RD | gclk_sync | gclk_tmem | tris | sync_pipe (sin prim / ni prim ni load) | sync_load (sin prim / ni prim ni load) | loads(TMEM igual) | wall_s |"
echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
for spec in "$@"; do
  rom=${spec%%=*}
  map=""
  [ "$rom" != "$spec" ] && map=${spec#*=}
  [ -z "$map" ] && [ -f "${rom%.z64}.map" ] && map=${rom%.z64}.map
  name=$(basename "$rom" .z64)
  # tope de pared por ROM: lockstep Villa ~110 s, threaded ~20 s (docs/baselines/timings.md)
  timeout 900 $PY "$HERE/pdbench.py" "$rom" ${map:+--map "$map"} $ARGS > "out/pdbench_$name.json" 2>&1
  tail -1 "out/pdbench_$name.json"
done

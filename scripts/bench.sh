#!/usr/bin/env bash
# Banco de pruebas SM64: corre la ROM N ops con FBDUMP y saca tiempo + md5 del framebuffer.
#
# GOTCHA: SM64 escribe su EEPROM (.eep) junto a la ROM. Si queda de una corrida anterior, el
# arranque toma otro camino y el md5 cambia SIN que haya cambiado el emulador. El script borra
# el save antes de CADA corrida: sin eso el oráculo "interp == JIT" no vale nada.
#
# uso: bash scripts/bench.sh <etiqueta>=<VAR=1 VAR2=1> ...
#   ej: bash scripts/bench.sh interp= jit=KESTREL_JIT=1 "link=KESTREL_JIT=1 KESTREL_JIT_LINK=1"
set -u
export PATH=/c/msys64/clang64/bin:$PATH

ROM=${KROM:-"/e/Claude/N64/test_roms/Super Mario 64 (USA).z64"}
SAVE="${ROM%.*}.eep"
OUT=${KOUT:-/tmp/kbench}
INSN=${KINSN:-300000000}
EXE=${KEXE:-./build/kestrel64.exe}
mkdir -p "$OUT"

for spec in "$@"; do
  tag=${spec%%=*}
  envs=${spec#*=}
  rm -f "$SAVE"
  s=$SECONDS
  env $envs KESTREL_MAXINSN=$INSN KESTREL_FBDUMP="$OUT/$tag.bmp" \
      timeout ${KTIMEOUT:-300} "$EXE" "$ROM" --run > "$OUT/$tag.log" 2>&1
  rc=$?
  md5=$(md5sum "$OUT/$tag.bmp" 2>/dev/null | cut -d' ' -f1)
  printf '%-24s rc=%d %4ds md5=%s\n' "$tag" $rc $((SECONDS-s)) "${md5:-NO-DUMP}"
done
rm -f "$SAVE"

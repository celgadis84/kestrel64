# Carga scripts/local.env (configuracion de la maquina, fuera del repo) sin pisar variables que
# ya vengan del entorno. Uso:  . "$(dirname "$0")/localenv.sh"  antes de un `cd`, o
# . scripts/localenv.sh  con la raiz del repo como directorio. Claves: scripts/local.env.example.
_le=scripts/local.env
[ -f "$_le" ] || _le="$(dirname "$0")/local.env"
if [ -f "$_le" ]; then
  while IFS= read -r _l || [ -n "$_l" ]; do
    _l=${_l%"$(printf '\r')"}
    case $_l in ''|\#*) continue ;; esac
    _k=${_l%%=*}; _v=${_l#*=}; _v=${_v#\"}; _v=${_v%\"}
    eval "[ -n \"\${$_k+x}\" ] || $_k=\$_v"
    export "$_k"
  done < "$_le"
fi
unset _le _l _k _v
PY=${PY:-python}
KESTREL_TESTROMS=${KESTREL_TESTROMS:-../test_roms}
# Ruta de una ROM de local.env: absoluta tal cual, relativa a KESTREL_TESTROMS. Vacia si falta.
romcfg() {
  case $1 in
    '') ;;
    /*|[A-Za-z]:*) printf '%s' "$1" ;;
    *) printf '%s/%s' "$KESTREL_TESTROMS" "$1" ;;
  esac
}

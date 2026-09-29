#!/bin/sh
# Mata SOLO los kestrel64*.exe que corren desde ESTE repo (arboles de build y dist/).
# Windows no deja reenlazar un .exe abierto, pero `taskkill //IM` mataba tambien las
# corridas de otra sesion que usa una copia fuera del repo (2026-09-29, PD-opt).
ROOT=$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)
ROOT=$(printf '%s' "$ROOT" | tr '/' '\\')
powershell -NoProfile -Command "Get-Process kestrel64,kestrel64-soft,kestrel64-gui -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$ROOT\\', [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force" >/dev/null 2>&1 || true

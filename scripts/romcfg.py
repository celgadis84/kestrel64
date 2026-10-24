"""Configuracion local de la maquina (ROMs de prueba, ares) para los scripts de python.

Lee scripts/local.env (ignorado por git; plantilla en scripts/local.env.example). Una variable
de entorno con el mismo nombre manda sobre el fichero. Las ROMs y las rutas de cada maquina
NO van en el repo.
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_FILE = ROOT / "scripts" / "local.env"


def _load():
    vals = {}
    if _FILE.exists():
        for line in _FILE.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip().strip('"')
    return vals


_VALS = _load()


def get(key, default=""):
    """Valor de `key`: entorno, luego local.env, luego `default`."""
    v = os.environ.get(key)
    if v is None:
        v = _VALS.get(key, "")
    return v or default


def testroms():
    """Carpeta de ROMs de prueba (KESTREL_TESTROMS; por defecto ../test_roms junto al repo)."""
    p = Path(get("KESTREL_TESTROMS", str(ROOT.parent / "test_roms")))
    return p if p.is_absolute() else (ROOT / p).resolve()


def rom(key):
    """Ruta de la ROM de `key` (ROM_SM64, ROM_DK64, ROM_PD) o None si no esta configurada."""
    v = get(key)
    if not v:
        return None
    p = Path(v)
    return p if p.is_absolute() else testroms() / p


def need(path, key):
    """Corta con un mensaje claro si una ROM obligatoria no esta configurada."""
    if not path:
        sys.exit(f"falta la ROM: pasa --rom o pon {key} en scripts/local.env "
                 f"(plantilla: scripts/local.env.example)")
    return path

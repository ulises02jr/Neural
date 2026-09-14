#!/usr/bin/env python3
"""
Backup diario de los datos CRÍTICOS de NeuralWorship a DigitalOcean Spaces.

Respalda:
  - usuarios.db  (cuentas, organizaciones, tokens, planes)   -> copia consistente (sqlite .backup)
  - config.json  (principal) y orgs/<id>/config.json de cada organización

NO respalda el audio (pistas/tonos/pads): eso ya vive en Spaces de forma durable.
NO incluye secrets.json (credenciales de servicios externos que se guardan aparte).

Sube a:   backups/AAAAMMDD/neural-backup-AAAAMMDD_HHMMSS.tar.gz
Retención: borra los backups con más de DIAS_RETENCION días.

Uso:  ./venv/bin/python backup_diario.py
"""
import sys
import tarfile
import sqlite3
import datetime
from pathlib import Path

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE))
import almacen  # noqa: E402  (reutiliza las credenciales de Spaces de secrets.json)

DIAS_RETENCION = 21
PREFIX = "backups/"


def _dump_db(db_path, dest):
    """Copia consistente de la base sqlite aunque el servidor este escribiendo."""
    src = sqlite3.connect(str(db_path))
    dst = sqlite3.connect(str(dest))
    try:
        with dst:
            src.backup(dst)
    finally:
        src.close()
        dst.close()


def main():
    if not almacen.habilitado():
        print("ERROR: Spaces no esta habilitado; no se puede respaldar.")
        return 1

    ts = datetime.datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    tmp = Path("/tmp/neural_backup_%s" % ts)
    tmp.mkdir(parents=True, exist_ok=True)

    # 1) Copia consistente de la base de datos
    db = BASE / "usuarios.db"
    if db.exists():
        _dump_db(db, tmp / "usuarios.db")

    # 2) Empaquetar db + config.json (principal y de cada organizacion)
    tar_path = tmp / ("neural-backup-%s.tar.gz" % ts)
    with tarfile.open(tar_path, "w:gz") as tar:
        if (tmp / "usuarios.db").exists():
            tar.add(tmp / "usuarios.db", arcname="usuarios.db")
        cfg = BASE / "config.json"
        if cfg.exists():
            tar.add(cfg, arcname="config.json")
        orgs_dir = BASE / "orgs"
        if orgs_dir.is_dir():
            for c in sorted(orgs_dir.glob("*/config.json")):
                tar.add(c, arcname="orgs/%s/config.json" % c.parent.name)

    size = tar_path.stat().st_size

    # 3) Subir a Spaces
    key = "%s%s/%s" % (PREFIX, ts[:8], tar_path.name)
    almacen.subir(key, tar_path, content_type="application/gzip")
    print("OK backup subido: %s (%.2f MB)" % (key, size / (1024 * 1024)))

    # 4) Retencion: borrar backups con mas de DIAS_RETENCION dias
    corte = (datetime.datetime.utcnow() - datetime.timedelta(days=DIAS_RETENCION)).strftime("%Y%m%d")
    borrados = 0
    try:
        for k in almacen.listar(PREFIX):
            partes = k.split("/")  # backups/AAAAMMDD/archivo
            if len(partes) >= 3 and partes[1].isdigit() and len(partes[1]) == 8 and partes[1] < corte:
                almacen.borrar(k)
                borrados += 1
    except Exception as e:
        print("Aviso: no se pudo aplicar retencion:", e)
    if borrados:
        print("Retencion: %d backup(s) viejo(s) borrado(s)." % borrados)

    # 5) Limpiar temporales
    try:
        for f in tmp.iterdir():
            f.unlink()
        tmp.rmdir()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

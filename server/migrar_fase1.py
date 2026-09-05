"""
Migración Fase 1 (multi-tenant): crea la organización #1 a partir de la
configuración actual (org_nombre + live_token de config.json) y asigna todos
los usuarios existentes a esa organización.

Idempotente y seguro: si YA existe alguna organización, no hace nada.

Uso:  python3 migrar_fase1.py
"""
import json
from pathlib import Path
import usuarios

CONFIG = Path(__file__).parent / "config.json"


def main():
    nombre = "Neural Worship"
    token = None
    try:
        cfg = json.load(open(CONFIG))
        nombre = (cfg.get("org_nombre") or nombre).strip()
        token = cfg.get("live_token") or None
    except Exception as e:
        print(f"(aviso) no se pudo leer config.json: {e}")

    usuarios.init_db()
    creada, org_id, msg = usuarios.asegurar_org_inicial(nombre=nombre, token=token)
    print(("✓ " if creada else "· ") + msg)

    # Verificación
    orgs = usuarios.listar_organizaciones()
    print("\n=== ORGANIZACIONES ===")
    for o in orgs:
        print(f"  #{o['id']} '{o['nombre']}' paquete={o['paquete']} dueno={o['owner_user_id']} "
              f"asientos={o['max_musicos']} gb={o['almacen_gb']} token={str(o['token'])[:8]}...")
    print("\n=== USUARIOS (org_id) ===")
    for u in usuarios.listar_usuarios():
        print(f"  id={u['id']} {u['email']} rol={u['rol']} estado={u['estado']} org_id={u.get('org_id')}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Guardia de disco: avisa al operador por email si el disco del servidor supera
un umbral. Tiene cooldown para no repetir el aviso cada rato.

Pensado como red de seguridad temporal hasta tener el bot de Telegram de alertas.
Uso (via cron cada 30 min):  ./venv/bin/python guardia_disco.py
"""
import sys
import json
import time
import shutil
from pathlib import Path

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE))
import emails as emails_module  # noqa: E402

UMBRAL_PCT = 85
OPERADOR_EMAIL = "andresescobar.ae41@gmail.com"
ESTADO = Path("/tmp/neural_guardia_disco.json")
COOLDOWN_H = 12


def main():
    du = shutil.disk_usage("/")
    pct = du.used / du.total * 100.0
    libre_gb = du.free / (1024 ** 3)
    if pct < UMBRAL_PCT:
        return 0
    # Cooldown: no repetir el aviso dentro de COOLDOWN_H horas
    try:
        last = json.loads(ESTADO.read_text()).get("last", 0)
    except Exception:
        last = 0
    if time.time() - last < COOLDOWN_H * 3600:
        return 0
    asunto = "Alerta: disco del servidor al %.0f%%" % pct
    html = ("<p>El disco del servidor NeuralWorship esta al <b>%.0f%%</b> "
            "(libre ~%.1f GB).</p><p>Libera espacio o subi el tamano del droplet "
            "antes de que se llene (los renders y subidas fallan con el disco lleno).</p>" % (pct, libre_gb))
    try:
        emails_module.enviar_email(OPERADOR_EMAIL, asunto, html)
    except Exception as e:
        print("no se pudo enviar alerta:", e)
        return 1
    try:
        ESTADO.write_text(json.dumps({"last": time.time(), "pct": pct}))
    except Exception:
        pass
    print("Alerta enviada: disco al %.0f%%" % pct)
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""
Integracion de pagos con Lemon Squeezy (Merchant of Record).

Responsabilidades de este modulo (lo minimo y estable):
  - Verificar la firma HMAC-SHA256 de los webhooks (seguridad).
  - Mapear la variante de producto comprada -> plan interno (basico/premium/ministerio).

Config en secrets.json (se llena cuando la cuenta este aprobada):
  lemonsqueezy_signing_secret : str   -> para validar los webhooks (OBLIGATORIO)
  lemonsqueezy_api_key        : str   -> opcional, para llamadas a la API
  lemonsqueezy_store_id       : str   -> opcional, para armar links de checkout
  lemonsqueezy_variants       : { "<variant_id>": "basico"|"premium"|"ministerio", ... }
"""
import hmac
import hashlib
import json
from pathlib import Path

_SECRETS = Path(__file__).parent / "secrets.json"
_cfg = None


def _load():
    global _cfg
    if _cfg is None:
        try:
            _cfg = json.loads(_SECRETS.read_text())
        except Exception:
            _cfg = {}
    return _cfg


def recargar():
    """Fuerza releer secrets.json (util tras editar la config)."""
    global _cfg
    _cfg = None
    return _load()


def habilitado():
    """True si ya hay signing secret configurado (si no, el webhook responde 503)."""
    return bool(_load().get("lemonsqueezy_signing_secret"))


def firma_valida(raw_body, signature_hex):
    """True si el HMAC-SHA256 del cuerpo crudo coincide con el header X-Signature."""
    secret = _load().get("lemonsqueezy_signing_secret") or ""
    if not secret or not signature_hex:
        return False
    esperado = hmac.new(secret.encode("utf-8"), raw_body, hashlib.sha256).hexdigest()
    try:
        return hmac.compare_digest(esperado, signature_hex.strip())
    except Exception:
        return False


def plan_de_variante(variant_id):
    """Plan interno (basico/premium/ministerio) para un variant_id de LS, o None."""
    if variant_id is None:
        return None
    m = _load().get("lemonsqueezy_variants") or {}
    return m.get(str(variant_id))


def tipo_de_addon(product_name, variant_name=None):
    """Detecta si el producto comprado es una AMPLIACION (add-on) y de qué tipo.
       Devuelve 'gb' (+50 GB de almacenamiento), 'seat' (+1 asiento de músico) o None."""
    txt = ((product_name or "") + " " + (variant_name or "")).lower()
    if not txt.strip():
        return None
    # Ojo: chequear add-ons ANTES que los planes (no confundir con Plus/Premium)
    if "gb" in txt or "almacen" in txt or "storage" in txt:
        return "gb"
    if "asiento" in txt or "músico" in txt or "musico" in txt or "seat" in txt or "asientos" in txt:
        return "seat"
    return None


def plan_de_nombre(product_name, variant_name=None):
    """Mapea por NOMBRE del producto/variante -> plan interno (no necesita IDs numéricos).
       Plus  -> premium (clave interna del plan de $10)
       Premium -> ministerio (clave interna del plan de $20)"""
    txt = ((product_name or "") + " " + (variant_name or "")).lower()
    if not txt.strip():
        return None
    if "premium" in txt or "premiun" in txt:
        return "ministerio"
    if "plus" in txt:
        return "premium"
    if "basic" in txt or "básico" in txt or "basico" in txt:
        return "basico"
    return None


def store_id():
    return _load().get("lemonsqueezy_store_id")


def api_key():
    return _load().get("lemonsqueezy_api_key")

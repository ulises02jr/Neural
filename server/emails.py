"""
Módulo para enviar emails desde el VPS usando la API de Resend.

La API key se lee de la variable de entorno RESEND_API_KEY o de secrets.json
(clave "resend_api_key"). El archivo secrets.json NO se sube a git.

Transporte: API HTTP de Resend (https://api.resend.com/emails) vía urllib
(librería estándar, sin dependencias externas, puerto 443).
"""
import os
import re
import json
import ssl
import urllib.request
import urllib.error
from pathlib import Path

ARCHIVO_SECRETS = Path(__file__).parent / "secrets.json"

# --- Marca NeuralWorship (fija) ---
REMITENTE_NOMBRE = "NeuralWorship"
REMITENTE_EMAIL = "noreply@neuralworship.com"
SOPORTE_EMAIL = "soporte@neuralworship.com"
URL_APP = "https://neuralworship.com"
ACENTO = "#9CA3AF"
API_URL = "https://api.resend.com/emails"


def _get_api_key():
    """Lee la API key de Resend de env var o de secrets.json."""
    key = os.getenv("RESEND_API_KEY")
    if key:
        return key.strip()
    if ARCHIVO_SECRETS.exists():
        try:
            with open(ARCHIVO_SECRETS) as f:
                data = json.load(f)
            return (data.get("resend_api_key") or "").strip() or None
        except Exception:
            return None
    return None


def email_configurado():
    """True si la API key de Resend está configurada."""
    return bool(_get_api_key())


def enviar_email(destino, asunto, cuerpo_html, cuerpo_texto=None):
    """Envía un email vía la API de Resend.

    Args:
        destino: email de destino (string)
        asunto: subject del email
        cuerpo_html: contenido HTML
        cuerpo_texto: versión texto plano (opcional; se genera si no se da)

    Returns:
        (ok: bool, mensaje: str)
    """
    api_key = _get_api_key()
    if not api_key:
        return False, "Resend no configurado (falta resend_api_key en secrets.json)"

    if cuerpo_texto is None:
        cuerpo_texto = re.sub(r"<[^>]+>", "", cuerpo_html)
        cuerpo_texto = re.sub(r"\n\s*\n\s*\n+", "\n\n", cuerpo_texto).strip()

    payload = {
        "from": f"{REMITENTE_NOMBRE} <{REMITENTE_EMAIL}>",
        "to": [destino],
        "subject": asunto,
        "html": cuerpo_html,
        "text": cuerpo_texto,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        API_URL,
        data=data,
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "User-Agent": "NeuralWorship-Mailer/1.0",
            "Accept": "application/json",
        },
    )
    try:
        ctx = ssl.create_default_context()
        with urllib.request.urlopen(req, timeout=30, context=ctx) as resp:
            body = resp.read().decode("utf-8", "replace")
        return True, f"Email enviado a {destino} ({body[:120]})"
    except urllib.error.HTTPError as e:
        detalle = e.read().decode("utf-8", "replace")
        return False, f"Error Resend HTTP {e.code}: {detalle[:200]}"
    except Exception as e:
        return False, f"Error enviando email: {type(e).__name__}: {e}"


def _enviar_email_background(destino, asunto, cuerpo_html):
    """Envía email en un thread separado para no bloquear la request HTTP."""
    import threading

    def _worker():
        ok, msg = enviar_email(destino, asunto, cuerpo_html)
        if not ok:
            print(f"⚠️  Email background falló: {msg}")
        else:
            print(f"✓ Email background enviado a {destino}")

    t = threading.Thread(target=_worker, daemon=True)
    t.start()


def _envoltura(titulo_seccion, contenido_html):
    """Plantilla base con la marca NeuralWorship."""
    return f"""
    <div style="font-family:Arial,Helvetica,sans-serif;max-width:540px;margin:0 auto;padding:24px;background:#f5f5f5">
      <div style="background:#0a0a0a;color:#fff;padding:26px 24px;border-radius:12px 12px 0 0;text-align:center">
        <div style="font-size:15px;color:{ACENTO};letter-spacing:3px;font-weight:700">NEURALWORSHIP</div>
        <div style="font-size:11px;color:#8a8a8a;margin-top:5px;letter-spacing:1px">{titulo_seccion}</div>
      </div>
      <div style="background:#fff;padding:32px 24px;border-radius:0 0 12px 12px">
        {contenido_html}
        <hr style="border:none;border-top:1px solid #eee;margin:24px 0">
        <p style="color:#aaa;font-size:11px;text-align:center;margin:0">
          Este es un correo automático de NeuralWorship. Por favor no respondas a este mensaje.<br>
          ¿Necesitás ayuda? Escribinos a <a href="mailto:{SOPORTE_EMAIL}" style="color:{ACENTO}">{SOPORTE_EMAIL}</a>.
        </p>
      </div>
    </div>
    """


def enviar_email_codigo_reset(destino, nombre, codigo, en_background=True):
    """Envía email con código de reset de contraseña.
    Si en_background=True, no bloquea (devuelve inmediatamente)."""
    asunto = "Código para restablecer tu contraseña — NeuralWorship"
    contenido = f"""
        <h2 style="margin:0 0 16px;color:#222">Hola {nombre},</h2>
        <p style="color:#444;line-height:1.6">Recibimos una solicitud para restablecer la contraseña de tu cuenta.</p>
        <p style="color:#444;line-height:1.6">Usá este código para crear una nueva contraseña:</p>
        <div style="text-align:center;margin:28px 0">
          <div style="display:inline-block;background:#0a0a0a;color:{ACENTO};font-size:32px;letter-spacing:8px;
                      font-weight:700;padding:18px 32px;border-radius:10px;font-family:monospace">
            {codigo}
          </div>
        </div>
        <p style="color:#888;font-size:13px;line-height:1.5">
          Este código expira en <strong>15 minutos</strong>.
          Si no solicitaste el cambio, podés ignorar este mensaje y tu contraseña seguirá igual.
        </p>
    """
    html = _envoltura("RESTABLECER CONTRASEÑA", contenido)
    if en_background:
        _enviar_email_background(destino, asunto, html)
        return True, "Email programado en background"
    return enviar_email(destino, asunto, html)


def enviar_email_bienvenida(destino, nombre, en_background=True):
    """Email que avisa que la cuenta fue aprobada.
    Si en_background=True, no bloquea."""
    asunto = "Tu cuenta fue aprobada — NeuralWorship"
    contenido = f"""
        <h2 style="margin:0 0 16px;color:#222">¡Bienvenido, {nombre}!</h2>
        <p style="color:#444;line-height:1.6">Tu cuenta fue aprobada y ya podés ingresar al sistema.</p>
        <div style="text-align:center;margin:28px 0">
          <a href="{URL_APP}/login"
             style="display:inline-block;background:{ACENTO};color:#000;padding:14px 32px;
                    border-radius:8px;text-decoration:none;font-weight:700">
            Ingresar a NeuralWorship
          </a>
        </div>
    """
    html = _envoltura("CUENTA APROBADA", contenido)
    if en_background:
        _enviar_email_background(destino, asunto, html)
        return True, "Email programado en background"
    return enviar_email(destino, asunto, html)


def enviar_email_invitacion(destino, org_nombre, link, en_background=True):
    """Email con el link de invitación para unirse a una organización."""
    asunto = f"Te invitaron a {org_nombre} — NeuralWorship"
    contenido = f"""
        <h2 style="margin:0 0 16px;color:#222">¡Te invitaron a {org_nombre}!</h2>
        <p style="color:#444;line-height:1.6">Fuiste invitado a unirte al equipo de música de
           <strong>{org_nombre}</strong> en NeuralWorship.</p>
        <p style="color:#444;line-height:1.6">Hacé clic en el botón para crear tu cuenta (queda lista al instante):</p>
        <div style="text-align:center;margin:28px 0">
          <a href="{link}"
             style="display:inline-block;background:{ACENTO};color:#000;padding:14px 32px;
                    border-radius:8px;text-decoration:none;font-weight:700">
            Aceptar invitación
          </a>
        </div>
        <p style="color:#888;font-size:13px;line-height:1.5">
          Si el botón no funciona, copiá este enlace:<br>
          <a href="{link}" style="color:{ACENTO};word-break:break-all">{link}</a>
        </p>
        <p style="color:#888;font-size:12px;line-height:1.5">
          Este enlace vence en 7 días. Si no esperabas esta invitación, podés ignorar este mensaje.
        </p>
    """
    html = _envoltura("INVITACIÓN", contenido)
    if en_background:
        _enviar_email_background(destino, asunto, html)
        return True, "Email programado en background"
    return enviar_email(destino, asunto, html)


if __name__ == "__main__":
    # Test rápido: python emails.py <email-destino>
    import sys
    if len(sys.argv) < 2:
        print("Uso: python emails.py <email-destino>")
        sys.exit(1)
    ok, msg = enviar_email(
        sys.argv[1],
        "Test de correo — NeuralWorship",
        _envoltura("PRUEBA", "<h2 style='color:#222'>Funciona ✓</h2>"
                   "<p style='color:#444'>El correo de NeuralWorship está configurado correctamente vía Resend.</p>"),
    )
    print(("✓ " if ok else "✗ ") + msg)

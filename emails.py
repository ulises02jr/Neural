"""
Módulo para enviar emails desde el VPS usando SMTP de Hostinger.

Las credenciales se leen de variables de entorno o de un archivo secrets.json
que NO debe subirse a git.

Variables de entorno (o keys en secrets.json):
  SMTP_HOST     - smtp.hostinger.com
  SMTP_PORT     - 465
  SMTP_USER     - noreply@miiglesiainternacional.org
  SMTP_PASSWORD - <password de la cuenta>
"""
import os
import json
import smtplib
import ssl
from email.message import EmailMessage
from pathlib import Path

ARCHIVO_SECRETS = Path(__file__).parent / "secrets.json"


def _get_config():
    """Lee config SMTP de env vars o de secrets.json."""
    # Primero env vars
    cfg = {
        "host": os.getenv("SMTP_HOST"),
        "port": os.getenv("SMTP_PORT"),
        "user": os.getenv("SMTP_USER"),
        "password": os.getenv("SMTP_PASSWORD"),
    }
    if all(cfg.values()):
        cfg["port"] = int(cfg["port"])
        return cfg
    # Fallback: secrets.json
    if ARCHIVO_SECRETS.exists():
        with open(ARCHIVO_SECRETS) as f:
            data = json.load(f)
            return {
                "host": data.get("smtp_host", "smtp.hostinger.com"),
                "port": int(data.get("smtp_port", 465)),
                "user": data.get("smtp_user"),
                "password": data.get("smtp_password"),
            }
    return None


def email_configurado():
    """True si las credenciales SMTP están configuradas."""
    cfg = _get_config()
    return cfg is not None and cfg.get("user") and cfg.get("password")


def _enviar_email_background(destino, asunto, cuerpo_html):
    """Envía email en un thread separado para no bloquear la request HTTP."""
    import threading
    def _worker():
        ok, msg = enviar_email(destino, asunto, cuerpo_html)
        if not ok:
            print(f"⚠️  Email background fallo: {msg}")
        else:
            print(f"✓ Email background enviado a {destino}")
    t = threading.Thread(target=_worker, daemon=True)
    t.start()


def enviar_email(destino, asunto, cuerpo_html, cuerpo_texto=None):
    """Envía un email vía SMTP de Hostinger.

    Args:
        destino: email de destino (string)
        asunto: subject del email
        cuerpo_html: contenido HTML
        cuerpo_texto: versión texto plano (opcional, se genera si no se da)

    Returns:
        (ok: bool, mensaje: str)
    """
    cfg = _get_config()
    if not cfg or not cfg.get("user") or not cfg.get("password"):
        return False, "SMTP no configurado (falta SMTP_USER o SMTP_PASSWORD)"

    msg = EmailMessage()
    msg["From"] = f"Mi Iglesia Internacional <{cfg['user']}>"
    msg["To"] = destino
    msg["Subject"] = asunto

    # Texto plano como fallback
    if cuerpo_texto is None:
        import re
        cuerpo_texto = re.sub(r"<[^>]+>", "", cuerpo_html).strip()
    msg.set_content(cuerpo_texto)
    msg.add_alternative(cuerpo_html, subtype="html")

    try:
        ctx = ssl.create_default_context()
        with smtplib.SMTP_SSL(cfg["host"], cfg["port"], context=ctx, timeout=30) as smtp:
            smtp.login(cfg["user"], cfg["password"])
            smtp.send_message(msg)
        return True, f"Email enviado a {destino}"
    except smtplib.SMTPAuthenticationError as e:
        return False, f"Error de autenticación SMTP: {e}"
    except Exception as e:
        return False, f"Error enviando email: {type(e).__name__}: {e}"


def enviar_email_codigo_reset(destino, nombre, codigo, en_background=True):
    """Envía email con código de reset de contraseña.
    Si en_background=True, no bloquea (devuelve inmediatamente)."""
    asunto = "Código para restablecer tu contraseña - Mi Iglesia Internacional"
    html = f"""
    <div style="font-family:Arial,sans-serif;max-width:540px;margin:0 auto;padding:24px;background:#f7f7f7">
      <div style="background:#0a0a0a;color:#fff;padding:24px;border-radius:12px 12px 0 0;text-align:center">
        <div style="font-size:13px;color:#C9A96E;letter-spacing:2px;font-weight:600">MI IGLESIA INTERNACIONAL</div>
        <div style="font-size:11px;color:#a3a3a3;margin-top:4px">CHARTS · MODO ENSAYO</div>
      </div>
      <div style="background:#fff;padding:32px 24px;border-radius:0 0 12px 12px">
        <h2 style="margin:0 0 16px;color:#222">Hola {nombre},</h2>
        <p style="color:#444;line-height:1.6">Recibimos una solicitud para restablecer la contraseña de tu cuenta.</p>
        <p style="color:#444;line-height:1.6">Usá este código para crear una nueva contraseña:</p>
        <div style="text-align:center;margin:28px 0">
          <div style="display:inline-block;background:#0a0a0a;color:#C9A96E;font-size:32px;letter-spacing:8px;
                      font-weight:700;padding:18px 32px;border-radius:10px;font-family:monospace">
            {codigo}
          </div>
        </div>
        <p style="color:#888;font-size:13px;line-height:1.5">
          Este código expira en <strong>15 minutos</strong>.
          Si no solicitaste el cambio, podés ignorar este mensaje y tu contraseña seguirá igual.
        </p>
        <hr style="border:none;border-top:1px solid #eee;margin:24px 0">
        <p style="color:#aaa;font-size:11px;text-align:center;margin:0">
          Este es un correo automático. No respondas a este mensaje.
        </p>
      </div>
    </div>
    """
    if en_background:
        _enviar_email_background(destino, asunto, html)
        return True, "Email programado en background"
    return enviar_email(destino, asunto, html)


def enviar_email_bienvenida(destino, nombre, en_background=True):
    """Email que avisa que la cuenta fue aprobada.
    Si en_background=True, no bloquea."""
    asunto = "Tu cuenta fue aprobada - Mi Iglesia Internacional"
    html = f"""
    <div style="font-family:Arial,sans-serif;max-width:540px;margin:0 auto;padding:24px;background:#f7f7f7">
      <div style="background:#0a0a0a;color:#fff;padding:24px;border-radius:12px 12px 0 0;text-align:center">
        <div style="font-size:13px;color:#C9A96E;letter-spacing:2px;font-weight:600">MI IGLESIA INTERNACIONAL</div>
        <div style="font-size:11px;color:#a3a3a3;margin-top:4px">CHARTS · MODO ENSAYO</div>
      </div>
      <div style="background:#fff;padding:32px 24px;border-radius:0 0 12px 12px">
        <h2 style="margin:0 0 16px;color:#222">¡Bienvenido, {nombre}!</h2>
        <p style="color:#444;line-height:1.6">Tu cuenta fue aprobada y ya podés ingresar al sistema.</p>
        <div style="text-align:center;margin:28px 0">
          <a href="https://miworship.miiglesiainternacional.org/login"
             style="display:inline-block;background:#C9A96E;color:#000;padding:14px 32px;
                    border-radius:8px;text-decoration:none;font-weight:600">
            Ingresar al sistema
          </a>
        </div>
        <p style="color:#888;font-size:13px;line-height:1.5">
          Si tenés dudas, escribí a <a href="mailto:contacto@miiglesiainternacional.org" style="color:#C9A96E">contacto@miiglesiainternacional.org</a>.
        </p>
      </div>
    </div>
    """
    if en_background:
        _enviar_email_background(destino, asunto, html)
        return True, "Email programado en background"
    return enviar_email(destino, asunto, html)


if __name__ == "__main__":
    # Test rápido
    import sys
    if len(sys.argv) < 2:
        print("Uso: python emails.py <email-destino>")
        sys.exit(1)
    ok, msg = enviar_email(
        sys.argv[1],
        "Test SMTP desde VPS Mi Iglesia",
        "<h2>Funciona ✓</h2><p>El SMTP está configurado correctamente.</p>"
    )
    print(("✓ " if ok else "✗ ") + msg)

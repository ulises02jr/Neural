"""
servidor_vps.py - Servidor Flask para modo ensayo (VPS público).

Diferente del sistema live:
- No usa MIDI ni Logic
- Cualquier músico puede ver cualquier canción cuando quiera
- Login con 2 passwords: uno para músicos (lectura), uno para admin (subir JSONs)
- Página principal: setlist destacado + biblioteca completa
- Página admin: subir JSONs, armar setlist, eliminar canciones

Endpoints:
  /                       Setlist + biblioteca (requiere login músicos)
  /cancion/<numero>       Ver una canción específica
  /login                  Login músicos
  /admin                  Panel admin (requiere login admin)
  /admin/login            Login admin
  /admin/subir            POST: subir un JSON nuevo
  /admin/eliminar/<n>     POST: eliminar canción
  /admin/setlist          POST: actualizar el setlist
  /api/cancion/<n>        JSON crudo de una canción (requiere login)
  /logout                 Cerrar sesión

Configuración: archivo config.json con:
  - password_musicos (sha256 hash)
  - password_admin (sha256 hash)
  - secret_key (cookie de sesión)
  - setlist (lista de números)
"""

import os
import json
import hashlib
import secrets
from functools import wraps
from pathlib import Path
from datetime import timedelta

from flask import (
    Flask, render_template, request, redirect, url_for,
    session, flash, jsonify, abort, send_file
)
from werkzeug.utils import secure_filename

from transposicion import transponer_cancion


# ───────────────────────── Configuración base ─────────────────────────
BASE_DIR = Path(__file__).resolve().parent
CARPETA_CANCIONES = BASE_DIR / "canciones"
ARCHIVO_CONFIG = BASE_DIR / "config.json"

CARPETA_CANCIONES.mkdir(exist_ok=True)


def hash_password(plain):
    return hashlib.sha256(plain.encode("utf-8")).hexdigest()


def cargar_config():
    """Carga config.json. Si no existe, lo crea con valores por defecto."""
    if not ARCHIVO_CONFIG.exists():
        # Primera ejecución: crear config con passwords por defecto
        default = {
            "password_musicos": hash_password("musicos2026"),
            "password_admin": hash_password("admin2026"),
            "secret_key": secrets.token_hex(32),
            "live_token": secrets.token_hex(16),
            "setlists": [],  # Multi-setlists: lista de {id, nombre, fecha, canciones}
            "live_activo": False,
            "mac_local_ip": None,
            "ultimo_heartbeat": None,
        }
        with open(ARCHIVO_CONFIG, "w", encoding="utf-8") as f:
            json.dump(default, f, indent=2)
        print("⚠️  config.json creado con passwords por defecto.")
        print("    Cámbialos editando el archivo.")
        return default
    with open(ARCHIVO_CONFIG, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    # Migración automática: agregar campos nuevos si la config es vieja
    cambios = False
    if "live_token" not in cfg:
        cfg["live_token"] = secrets.token_hex(16)
        cambios = True
    if "live_activo" not in cfg:
        cfg["live_activo"] = False
        cambios = True
    if "mac_local_ip" not in cfg:
        cfg["mac_local_ip"] = None
        cambios = True
    if "ultimo_heartbeat" not in cfg:
        cfg["ultimo_heartbeat"] = None
        cambios = True
    # Migración: setlist único → multi-setlists
    if "setlists" not in cfg:
        cfg["setlists"] = []
        # Si había un setlist viejo, convertirlo
        viejo = cfg.pop("setlist", None)
        if viejo:
            cfg["setlists"].append({
                "id": secrets.token_hex(6),
                "nombre": "Setlist migrado",
                "fecha": _hoy_iso(),
                "canciones": [
                    {"id": n, "tono": None} if isinstance(n, int)
                    else {"id": int(n.get("id", 0)), "tono": n.get("tono")}
                    for n in viejo if (isinstance(n, int) or isinstance(n, dict))
                ],
                "creado": _ahora_iso(),
            })
            print(f"✓ Setlist viejo migrado a multi-setlists")
        cambios = True
    # Limpiar setlists con fecha pasada (más de 1 día atrás)
    if "setlists" in cfg and cfg["setlists"]:
        hoy = _hoy_iso()
        antes = len(cfg["setlists"])
        cfg["setlists"] = [s for s in cfg["setlists"] if s.get("fecha", "9999") >= hoy]
        if len(cfg["setlists"]) < antes:
            cambios = True
            print(f"🗑  Eliminados {antes - len(cfg['setlists'])} setlist(s) con fecha pasada")
    if cambios:
        with open(ARCHIVO_CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    return cfg


def _hoy_iso():
    """Fecha de hoy en formato YYYY-MM-DD."""
    from datetime import date
    return date.today().isoformat()


def _ahora_iso():
    """Timestamp ISO."""
    from datetime import datetime
    return datetime.now().isoformat(timespec="seconds")


def guardar_config(cfg):
    with open(ARCHIVO_CONFIG, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)


# ───────────────────────── App Flask ─────────────────────────
# Cargamos config una vez para inicializar Flask
_config_inicial = cargar_config()

app = Flask(__name__)
app.secret_key = _config_inicial["secret_key"]
app.permanent_session_lifetime = timedelta(days=30)
app.config["MAX_CONTENT_LENGTH"] = 5 * 1024 * 1024  # 5 MB max por archivo


def get_config():
    """Lee config.json del disco en cada llamada.
    Esto garantiza que múltiples workers vean los cambios."""
    return cargar_config()


# ───────────────────────── Helpers ─────────────────────────
def cargar_biblioteca():
    """Lee todos los JSONs de canciones/ y devuelve dict numero → datos."""
    biblioteca = {}
    for archivo in CARPETA_CANCIONES.glob("*.json"):
        try:
            with open(archivo, "r", encoding="utf-8") as f:
                datos = json.load(f)
                if "numero" in datos:
                    biblioteca[datos["numero"]] = datos
        except Exception as e:
            print(f"⚠️  Error leyendo {archivo.name}: {e}")
    return biblioteca


def login_required(rol):
    """Decorador. rol='musico' o 'admin'."""
    def wrapper(fn):
        @wraps(fn)
        def decorated(*args, **kwargs):
            if session.get("rol") != rol:
                # Admin puede acceder a rutas de musico
                if rol == "musico" and session.get("rol") == "admin":
                    return fn(*args, **kwargs)
                return redirect(url_for("login" if rol == "musico" else "admin_login"))
            return fn(*args, **kwargs)
        return decorated
    return wrapper


# ───────────────────────── Rutas públicas (login) ─────────────────────────
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        password = request.form.get("password", "")
        cfg = get_config()
        if hash_password(password) == cfg["password_musicos"]:
            session.permanent = True
            session["rol"] = "musico"
            return redirect(url_for("principal"))
        flash("Contraseña incorrecta", "error")
    return render_template("login_musicos.html")


@app.route("/admin/login", methods=["GET", "POST"])
def admin_login():
    if request.method == "POST":
        password = request.form.get("password", "")
        cfg = get_config()
        if hash_password(password) == cfg["password_admin"]:
            session.permanent = True
            session["rol"] = "admin"
            return redirect(url_for("admin"))
        flash("Contraseña incorrecta", "error")
    return render_template("login_admin.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


# ───────────────────────── Rutas para músicos ─────────────────────────
@app.route("/")
@login_required("musico")
def principal():
    biblioteca = cargar_biblioteca()
    biblioteca_ordenada = sorted(biblioteca.values(), key=lambda c: c["numero"])
    cfg = get_config()

    # Ordenar setlists por fecha ASC (más próximo primero), luego por created DESC para mismo día
    setlists_raw = cfg.get("setlists", [])
    setlists_ordenados = sorted(
        setlists_raw,
        key=lambda s: (s.get("fecha", "9999"), s.get("creado", ""))
    )

    # Setlist seleccionado: ?setlist_id=xxx o el más próximo
    setlist_id = request.args.get("setlist_id")
    setlist_activo = None
    if setlist_id:
        setlist_activo = next((s for s in setlists_ordenados if s["id"] == setlist_id), None)
    if not setlist_activo and setlists_ordenados:
        setlist_activo = setlists_ordenados[0]  # el más próximo

    # Resolver canciones del setlist activo (con tono override)
    setlist_canciones = []
    if setlist_activo:
        for item in setlist_activo.get("canciones", []):
            num = item.get("id")
            if num in biblioteca:
                cancion = dict(biblioteca[num])
                # Aplicar tono override
                if item.get("tono"):
                    cancion["tono"] = item["tono"]
                cancion["_es_override"] = item.get("tono") and item["tono"] != biblioteca[num].get("tono")
                setlist_canciones.append(cancion)

    # Lista resumida de TODOS los setlists para el selector
    setlists_resumen = [
        {"id": s["id"], "nombre": s.get("nombre", "Sin nombre"), "fecha": s.get("fecha", "")}
        for s in setlists_ordenados
    ]

    # Estado del live
    live_activo = is_live_activo(cfg)

    return render_template(
        "principal.html",
        setlist=setlist_canciones,
        setlist_activo=setlist_activo,
        setlists_disponibles=setlists_resumen,
        biblioteca=biblioteca_ordenada,
        es_admin=(session.get("rol") == "admin"),
        live_activo=live_activo,
        live_ip=cfg.get("mac_local_ip") if live_activo else None,
    )


@app.route("/cancion/<int:numero>")
@login_required("musico")
def ver_cancion(numero):
    biblioteca = cargar_biblioteca()
    if numero not in biblioteca:
        abort(404)

    cancion = dict(biblioteca[numero])  # copia para no mutar la biblioteca
    cfg = get_config()

    # Si viene de un setlist específico, aplicar el tono override de ese setlist
    setlist_id = request.args.get("setlist_id")
    if setlist_id:
        sl = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
        if sl:
            item = next((c for c in sl.get("canciones", []) if c.get("id") == numero), None)
            if item and item.get("tono") and item["tono"] != cancion.get("tono"):
                # Calcular semitonos entre tono original y override
                from transposicion import NOTA_A_INDICE
                import re
                def raiz(t):
                    m = re.match(r'^([A-G][#b]?)', t or "")
                    return m.group(1) if m else None
                r_orig = raiz(cancion.get("tono", ""))
                r_dest = raiz(item["tono"])
                if r_orig and r_dest and r_orig in NOTA_A_INDICE and r_dest in NOTA_A_INDICE:
                    sem = (NOTA_A_INDICE[r_dest] - NOTA_A_INDICE[r_orig]) % 12
                    if sem != 0:
                        cancion = transponer_cancion(cancion, sem)

    # Soporte de transposición manual (botones +/-): ?t=N
    # Y origen: ?from=bib indica que se accedió desde biblioteca (se muestran botones +/-)
    try:
        semitonos = int(request.args.get("t", 0))
        if semitonos < -12 or semitonos > 12:
            semitonos = 0
    except (ValueError, TypeError):
        semitonos = 0

    if semitonos != 0:
        cancion = transponer_cancion(cancion, semitonos)

    # Permitir transposición solo si viene de biblioteca (no desde setlist)
    desde_biblioteca = request.args.get("from") == "bib"

    return render_template(
        "visor.html",
        cancion=cancion,
        es_admin=(session.get("rol") == "admin"),
        permitir_transposicion=desde_biblioteca,
        semitonos_actual=semitonos,
    )


@app.route("/api/cancion/<int:numero>")
@login_required("musico")
def api_cancion(numero):
    biblioteca = cargar_biblioteca()
    if numero not in biblioteca:
        return jsonify({"error": "No encontrada"}), 404
    return jsonify(biblioteca[numero])


# ───────────────────────── Rutas admin ─────────────────────────
@app.route("/admin")
@login_required("admin")
def admin():
    biblioteca = cargar_biblioteca()
    biblioteca_ordenada = sorted(biblioteca.values(), key=lambda c: c["numero"])
    cfg = get_config()
    live_activo = is_live_activo(cfg)
    # Hace cuántos segundos fue el último heartbeat
    ultimo = cfg.get("ultimo_heartbeat")
    if ultimo:
        import time as _t
        segundos_atras = int(_t.time() - ultimo)
    else:
        segundos_atras = None
    # Setlists ordenados por fecha ASC
    setlists = sorted(
        cfg.get("setlists", []),
        key=lambda s: (s.get("fecha", "9999"), s.get("creado", ""))
    )
    # Para cada setlist, agregar info de las canciones
    biblioteca_dict = {c["numero"]: c for c in biblioteca_ordenada}
    setlists_full = []
    for s in setlists:
        canciones = []
        for item in s.get("canciones", []):
            num = item.get("id")
            if num in biblioteca_dict:
                base = biblioteca_dict[num]
                tono_efectivo = item.get("tono") or base.get("tono", "")
                canciones.append({
                    "id": num,
                    "titulo": base["titulo"],
                    "artista": base.get("artista", ""),
                    "tono": tono_efectivo,
                    "tono_original": base.get("tono", ""),
                    "es_override": bool(item.get("tono") and item["tono"] != base.get("tono", "")),
                })
        setlists_full.append({
            **s,
            "canciones_resolved": canciones,
        })
    return render_template(
        "admin.html",
        biblioteca=biblioteca_ordenada,
        setlists=setlists_full,
        live_activo=live_activo,
        live_ip=cfg.get("mac_local_ip"),
        live_token=cfg.get("live_token", ""),
        ultimo_heartbeat_seg=segundos_atras,
        hoy=_hoy_iso(),
    )


@app.route("/admin/subir", methods=["POST"])
@login_required("admin")
def admin_subir():
    if "archivo" not in request.files:
        flash("No se seleccionó archivo", "error")
        return redirect(url_for("admin"))
    archivo = request.files["archivo"]
    if archivo.filename == "":
        flash("Archivo vacío", "error")
        return redirect(url_for("admin"))
    if not archivo.filename.endswith(".json"):
        flash("Solo se aceptan archivos .json", "error")
        return redirect(url_for("admin"))
    try:
        contenido = archivo.read()
        datos = json.loads(contenido.decode("utf-8"))
        if "numero" not in datos or "titulo" not in datos or "secciones" not in datos:
            flash("El JSON no tiene los campos requeridos (numero, titulo, secciones)", "error")
            return redirect(url_for("admin"))
    except json.JSONDecodeError:
        flash("Archivo no es JSON válido", "error")
        return redirect(url_for("admin"))
    nombre_seguro = secure_filename(archivo.filename)
    destino = CARPETA_CANCIONES / nombre_seguro
    with open(destino, "wb") as f:
        f.write(contenido)
    flash(f"✓ Canción '{datos['titulo']}' (#{datos['numero']}) subida correctamente", "success")
    return redirect(url_for("admin"))


@app.route("/admin/eliminar/<int:numero>", methods=["POST"])
@login_required("admin")
def admin_eliminar(numero):
    biblioteca = cargar_biblioteca()
    if numero not in biblioteca:
        flash("Canción no encontrada", "error")
        return redirect(url_for("admin"))
    for archivo in CARPETA_CANCIONES.glob("*.json"):
        try:
            with open(archivo, "r", encoding="utf-8") as f:
                datos = json.load(f)
            if datos.get("numero") == numero:
                archivo.unlink()
                flash(f"✓ Canción #{numero} eliminada", "success")
                # Removerla de TODOS los setlists si estaba
                cfg = get_config()
                for s in cfg.get("setlists", []):
                    s["canciones"] = [c for c in s.get("canciones", []) if c.get("id") != numero]
                guardar_config(cfg)
                break
        except Exception:
            continue
    return redirect(url_for("admin"))


# ───────── CRUD de setlists ─────────
@app.route("/admin/setlist/crear", methods=["POST"])
@login_required("admin")
def admin_setlist_crear():
    nombre = request.form.get("nombre", "").strip() or "Sin nombre"
    fecha = request.form.get("fecha", "").strip() or _hoy_iso()
    cfg = get_config()
    nuevo = {
        "id": secrets.token_hex(6),
        "nombre": nombre[:80],
        "fecha": fecha,
        "canciones": [],
        "creado": _ahora_iso(),
    }
    cfg.setdefault("setlists", []).append(nuevo)
    guardar_config(cfg)
    flash(f"✓ Setlist '{nombre}' creado", "success")
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/editar", methods=["POST"])
@login_required("admin")
def admin_setlist_editar(setlist_id):
    nombre = request.form.get("nombre", "").strip()
    fecha = request.form.get("fecha", "").strip()
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if not s:
        flash("Setlist no encontrado", "error")
        return redirect(url_for("admin"))
    if nombre:
        s["nombre"] = nombre[:80]
    if fecha:
        s["fecha"] = fecha
    guardar_config(cfg)
    flash("✓ Setlist actualizado", "success")
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/eliminar", methods=["POST"])
@login_required("admin")
def admin_setlist_eliminar(setlist_id):
    cfg = get_config()
    antes = len(cfg.get("setlists", []))
    cfg["setlists"] = [s for s in cfg.get("setlists", []) if s["id"] != setlist_id]
    if len(cfg["setlists"]) < antes:
        guardar_config(cfg)
        flash("✓ Setlist eliminado", "success")
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/agregar", methods=["POST"])
@login_required("admin")
def admin_setlist_agregar(setlist_id):
    numero = request.form.get("numero", "").strip()
    try:
        numero = int(numero)
    except ValueError:
        flash("Número inválido", "error")
        return redirect(url_for("admin"))
    biblioteca = cargar_biblioteca()
    if numero not in biblioteca:
        flash("Canción no encontrada", "error")
        return redirect(url_for("admin"))
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if not s:
        flash("Setlist no encontrado", "error")
        return redirect(url_for("admin"))
    # Evitar duplicados
    if any(c.get("id") == numero for c in s.get("canciones", [])):
        flash("La canción ya está en el setlist", "error")
        return redirect(url_for("admin"))
    s.setdefault("canciones", []).append({"id": numero, "tono": None})
    guardar_config(cfg)
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/quitar/<int:numero>", methods=["POST"])
@login_required("admin")
def admin_setlist_quitar(setlist_id, numero):
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if s:
        s["canciones"] = [c for c in s.get("canciones", []) if c.get("id") != numero]
        guardar_config(cfg)
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/mover", methods=["POST"])
@login_required("admin")
def admin_setlist_mover(setlist_id):
    numero = int(request.form.get("numero", 0))
    direccion = request.form.get("direccion", "")
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if not s:
        return redirect(url_for("admin"))
    canciones = s.get("canciones", [])
    idx = next((i for i, c in enumerate(canciones) if c.get("id") == numero), -1)
    if idx < 0:
        return redirect(url_for("admin"))
    if direccion == "arriba" and idx > 0:
        canciones[idx-1], canciones[idx] = canciones[idx], canciones[idx-1]
    elif direccion == "abajo" and idx < len(canciones) - 1:
        canciones[idx+1], canciones[idx] = canciones[idx], canciones[idx+1]
    guardar_config(cfg)
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/transponer/<int:numero>", methods=["POST"])
@login_required("admin")
def admin_setlist_transponer(setlist_id, numero):
    """Sube/baja N semitonos el tono de una canción en un setlist."""
    delta = int(request.form.get("delta", 0))
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if not s:
        return redirect(url_for("admin"))
    item = next((c for c in s.get("canciones", []) if c.get("id") == numero), None)
    if not item:
        return redirect(url_for("admin"))
    biblioteca = cargar_biblioteca()
    base = biblioteca.get(numero, {})
    tono_actual = item.get("tono") or base.get("tono", "")
    if not tono_actual:
        return redirect(url_for("admin"))
    # Calcular nuevo tono (smart: bemoles/sostenidos)
    from transposicion import transponer_acorde, usar_sostenidos
    tono_sost = transponer_acorde(tono_actual, delta, usar_sost=True)
    usar_sost = usar_sostenidos(tono_sost)
    nuevo_tono = transponer_acorde(tono_actual, delta, usar_sost=usar_sost)
    # Si vuelve al original, limpiar override
    if nuevo_tono == base.get("tono", ""):
        item["tono"] = None
    else:
        item["tono"] = nuevo_tono
    guardar_config(cfg)
    return redirect(url_for("admin"))


@app.route("/admin/setlist/<setlist_id>/resetear/<int:numero>", methods=["POST"])
@login_required("admin")
def admin_setlist_resetear_tono(setlist_id, numero):
    cfg = get_config()
    s = next((s for s in cfg.get("setlists", []) if s["id"] == setlist_id), None)
    if s:
        item = next((c for c in s.get("canciones", []) if c.get("id") == numero), None)
        if item:
            item["tono"] = None
            guardar_config(cfg)
    return redirect(url_for("admin"))


@app.route("/admin/cambiar_password", methods=["POST"])
@login_required("admin")
def admin_cambiar_password():
    cual = request.form.get("cual", "")
    nueva = request.form.get("nueva", "")
    if not nueva or len(nueva) < 6:
        flash("Contraseña debe tener al menos 6 caracteres", "error")
        return redirect(url_for("admin"))
    cfg = get_config()
    if cual == "musicos":
        cfg["password_musicos"] = hash_password(nueva)
        flash("✓ Password de músicos actualizado", "success")
    elif cual == "admin":
        cfg["password_admin"] = hash_password(nueva)
        flash("✓ Password de admin actualizado", "success")
    else:
        flash("Tipo de password inválido", "error")
        return redirect(url_for("admin"))
    guardar_config(cfg)
    return redirect(url_for("admin"))


# ───────────────────────── Live (Mac local) ─────────────────────────
# Timeout: si pasaron más de 90 segundos sin heartbeat, el live se considera apagado
HEARTBEAT_TIMEOUT_SEG = 90


def is_live_activo(cfg=None):
    """True si el Mac mandó heartbeat recientemente."""
    if cfg is None:
        cfg = get_config()
    if not cfg.get("live_activo"):
        return False
    ultimo = cfg.get("ultimo_heartbeat")
    if not ultimo:
        return False
    import time as _t
    return (_t.time() - ultimo) < HEARTBEAT_TIMEOUT_SEG


@app.route("/api/live_ping", methods=["POST"])
def api_live_ping():
    """El puente.py del Mac llama esto cada 30s para avisar que está vivo."""
    data = request.get_json(silent=True) or {}
    cfg = get_config()
    # Validar token
    if data.get("token") != cfg.get("live_token"):
        return jsonify({"ok": False, "error": "invalid token"}), 403
    ip = data.get("ip", "").strip()
    if not ip:
        return jsonify({"ok": False, "error": "missing ip"}), 400
    accion = data.get("accion", "ping")  # "ping" o "bye"
    import time as _t
    if accion == "bye":
        cfg["live_activo"] = False
        cfg["ultimo_heartbeat"] = None
        print(f"📴 Mac local desconectado (bye explícito)")
    else:
        cfg["live_activo"] = True
        cfg["mac_local_ip"] = ip
        cfg["ultimo_heartbeat"] = _t.time()
    guardar_config(cfg)
    return jsonify({"ok": True})


@app.route("/api/live_status")
def api_live_status():
    """El frontend lo consulta para saber si hay live activo."""
    cfg = get_config()
    activo = is_live_activo(cfg)
    return jsonify({
        "activo": activo,
        "ip": cfg.get("mac_local_ip") if activo else None,
    })


@app.route("/admin/live_off", methods=["POST"])
@login_required("admin")
def admin_live_off():
    """Forzar apagado del live desde el panel admin."""
    cfg = get_config()
    cfg["live_activo"] = False
    cfg["ultimo_heartbeat"] = None
    guardar_config(cfg)
    flash("✓ Live forzado a OFF", "success")
    return redirect(url_for("admin"))


# ───────────────────────── Main ─────────────────────────
if __name__ == "__main__":
    # En desarrollo local (no en VPS), correr en puerto 5051
    # En VPS, esto lo manejará gunicorn o systemd
    app.run(host="127.0.0.1", port=5051, debug=False)

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
import re
import subprocess
import shutil
import threading
import logging
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
import usuarios
import emails as emails_module


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

# Inicializar base de datos de usuarios (crea tablas si no existen)
usuarios.init_db()

app = Flask(__name__)
app.secret_key = _config_inicial["secret_key"]
app.permanent_session_lifetime = timedelta(days=30)
app.config["MAX_CONTENT_LENGTH"] = 200 * 1024 * 1024  # 5 MB max por archivo


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
    """Decorador. rol='musico' o 'admin'.
    Compatible con:
    - Sistema viejo: session['rol'] = 'musico' o 'admin' (password compartido)
    - Sistema nuevo: session['user_id'] + session['rol'] + session['nombre']
    Para 'musico', admin también puede acceder.
    """
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


def get_usuario_actual():
    """Devuelve dict del usuario logueado o None.
    Si es login viejo (password compartido), devuelve un dict mínimo.
    """
    uid = session.get("user_id")
    if uid:
        u = usuarios.buscar_por_id(uid)
        if u:
            return u
    # Fallback: login con password compartido (admin de emergencia)
    if session.get("rol") == "admin" and session.get("nombre") == "Admin (Emergencia)":
        return {"nombre": "Admin", "apellido": "(Emergencia)", "email": "—", "rol": "admin", "id": None}
    return None


# ───────────────────────── Rutas públicas (login) ─────────────────────────
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        email = request.form.get("email", "").strip().lower()
        password = request.form.get("password", "")
        if not email or not password:
            flash("Completá email y contraseña", "error")
            return render_template("login_musicos.html")
        # Intentar autenticar con sistema de usuarios
        u = usuarios.autenticar(email, password)
        if u:
            # Si es admin, redirigir al login de admin (separación de pantallas)
            if u["rol"] == "admin":
                flash("Sos administrador. Usá el acceso de administrador.", "error")
                return redirect(url_for("admin_login"))
            session.permanent = True
            session.clear()
            session["user_id"] = u["id"]
            session["rol"] = u["rol"]
            session["nombre"] = u["nombre"]
            return redirect(url_for("principal"))
        # Verificar si el usuario existe pero está pendiente
        existente = usuarios.buscar_por_email(email)
        if existente and existente["estado"] == "pendiente":
            flash("Tu cuenta está pendiente de aprobación", "error")
        elif existente and existente["estado"] == "rechazado":
            flash("Tu cuenta no fue aprobada. Contactá al administrador.", "error")
        else:
            flash("Email o contraseña incorrectos", "error")
    return render_template("login_musicos.html")


@app.route("/registro", methods=["GET", "POST"])
def registro():
    if request.method == "POST":
        nombre = request.form.get("nombre", "").strip()
        apellido = request.form.get("apellido", "").strip()
        email = request.form.get("email", "").strip().lower()
        password = request.form.get("password", "")
        password2 = request.form.get("password2", "")
        if password != password2:
            flash("Las contraseñas no coinciden", "error")
            return render_template("registro.html",
                                   nombre=nombre, apellido=apellido, email=email)
        ok, resultado = usuarios.crear_usuario(nombre, apellido, email, password, rol="musico")
        if ok:
            flash("✓ Cuenta creada. Esperá la aprobación del administrador para poder ingresar.", "success")
            return redirect(url_for("login"))
        else:
            flash(resultado, "error")
            return render_template("registro.html",
                                   nombre=nombre, apellido=apellido, email=email)
    return render_template("registro.html")


@app.route("/admin/login", methods=["GET", "POST"])
def admin_login():
    """Login para admins. Soporta:
    - Email + password (sistema nuevo)
    - Password de emergencia (fallback para no quedar bloqueado)
    """
    if request.method == "POST":
        email = request.form.get("email", "").strip().lower()
        password = request.form.get("password", "")

        # Si dejan email vacío → intentar password de emergencia
        if not email and password:
            cfg = get_config()
            if hash_password(password) == cfg.get("password_admin", ""):
                session.permanent = True
                session.clear()
                session["rol"] = "admin"
                session["nombre"] = "Admin (Emergencia)"
                flash("⚠️ Entraste con password de emergencia. Iniciá sesión con tu cuenta personal cuando puedas.", "success")
                return redirect(url_for("admin"))
            flash("Password de emergencia incorrecto", "error")
            return render_template("login_admin.html")

        # Login normal con email
        u = usuarios.autenticar(email, password)
        if u and u["rol"] == "admin":
            session.permanent = True
            session.clear()
            session["user_id"] = u["id"]
            session["rol"] = "admin"
            session["nombre"] = u["nombre"]
            return redirect(url_for("admin"))
        flash("Email o contraseña incorrectos (o no sos admin)", "error")
    return render_template("login_admin.html")


@app.route("/olvide_password", methods=["GET", "POST"])
def olvide_password():
    """Solicitar código de reset por email."""
    if request.method == "POST":
        email = request.form.get("email", "").strip().lower()
        if not email:
            flash("Ingresá tu email", "error")
            return render_template("olvide_password.html")
        u = usuarios.buscar_por_email(email)
        # Por seguridad: siempre damos el mismo mensaje aunque no exista
        if u and u["estado"] == "activo":
            codigo = usuarios.crear_codigo_reset(u["id"])
            nombre_completo = f"{u['nombre']} {u['apellido']}"
            ok, _ = emails_module.enviar_email_codigo_reset(u["email"], nombre_completo, codigo)
            if not ok:
                print(f"⚠️  No se pudo enviar email a {email}")
        flash("Si el email existe en nuestro sistema, te enviamos un código de reset.", "success")
        return redirect(url_for("reset_password", email=email))
    return render_template("olvide_password.html")


@app.route("/reset_password", methods=["GET", "POST"])
def reset_password():
    """Ingresar código + nueva contraseña."""
    email_pre = request.args.get("email", "")
    if request.method == "POST":
        email = request.form.get("email", "").strip().lower()
        codigo = request.form.get("codigo", "").strip()
        nueva = request.form.get("password", "")
        nueva2 = request.form.get("password2", "")
        if nueva != nueva2:
            flash("Las contraseñas no coinciden", "error")
            return render_template("reset_password.html", email=email, codigo=codigo)
        ok, msg = usuarios.usar_codigo_y_cambiar_password(email, codigo, nueva)
        if ok:
            flash("✓ Contraseña cambiada. Ya podés ingresar.", "success")
            return redirect(url_for("login"))
        flash(msg, "error")
        return render_template("reset_password.html", email=email, codigo=codigo)
    return render_template("reset_password.html", email=email_pre, codigo="")


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
        usuario=get_usuario_actual(),
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

    tono_original = cancion.get("tono", "C")

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
        tono_original=tono_original,
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
        usuarios_pendientes=usuarios.listar_usuarios(estado="pendiente"),
        usuarios_activos=usuarios.listar_usuarios(estado="activo"),
        usuario_actual=get_usuario_actual(),
        email_configurado=emails_module.email_configurado(),
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


# ───────────────────────── Gestión de usuarios (admin) ─────────────────────────
@app.route("/admin/usuario/<int:user_id>/aprobar", methods=["POST"])
@login_required("admin")
def admin_usuario_aprobar(user_id):
    u = usuarios.buscar_por_id(user_id)
    if not u:
        flash("Usuario no encontrado", "error")
        return redirect(url_for("admin"))
    if usuarios.aprobar_usuario(user_id):
        # Mandar email de bienvenida
        nombre_completo = f"{u['nombre']} {u['apellido']}"
        ok, _ = emails_module.enviar_email_bienvenida(u["email"], nombre_completo)
        if ok:
            flash(f"✓ {nombre_completo} aprobado y notificado por email", "success")
        else:
            flash(f"✓ {nombre_completo} aprobado (no se pudo mandar el email)", "success")
    else:
        flash("No se pudo aprobar (¿ya estaba aprobado?)", "error")
    return redirect(url_for("admin"))


@app.route("/admin/usuario/<int:user_id>/eliminar", methods=["POST"])
@login_required("admin")
def admin_usuario_eliminar(user_id):
    u = usuarios.buscar_por_id(user_id)
    if not u:
        flash("Usuario no encontrado", "error")
        return redirect(url_for("admin"))
    # No permitir borrar el último admin activo
    if u["rol"] == "admin" and u["estado"] == "activo" and usuarios.contar_admins_activos() <= 1:
        flash("No podés borrar el último admin activo", "error")
        return redirect(url_for("admin"))
    # No permitir auto-eliminación
    if session.get("user_id") == user_id:
        flash("No podés borrar tu propia cuenta", "error")
        return redirect(url_for("admin"))
    if usuarios.eliminar_usuario(user_id):
        flash(f"✓ Usuario {u['nombre']} {u['apellido']} eliminado", "success")
    return redirect(url_for("admin"))


@app.route("/admin/usuario/<int:user_id>/reset_password", methods=["POST"])
@login_required("admin")
def admin_usuario_reset_password(user_id):
    """Admin manda manualmente un código de reset (por si el usuario no recibe el email original)."""
    u = usuarios.buscar_por_id(user_id)
    if not u:
        flash("Usuario no encontrado", "error")
        return redirect(url_for("admin"))
    codigo = usuarios.crear_codigo_reset(user_id)
    nombre_completo = f"{u['nombre']} {u['apellido']}"
    ok, msg = emails_module.enviar_email_codigo_reset(u["email"], nombre_completo, codigo)
    if ok:
        flash(f"✓ Código de reset enviado a {u['email']}", "success")
    else:
        flash(f"⚠️ No se pudo mandar email: {msg}. Código: {codigo}", "error")
    return redirect(url_for("admin"))


@app.route("/admin/crear_admin", methods=["POST"])
@login_required("admin")
def admin_crear_admin():
    """Crear un nuevo admin desde el panel (solo otros admins pueden)."""
    nombre = request.form.get("nombre", "").strip()
    apellido = request.form.get("apellido", "").strip()
    email = request.form.get("email", "").strip().lower()
    password = request.form.get("password", "")
    ok, resultado = usuarios.crear_usuario(nombre, apellido, email, password, rol="admin", estado="activo")
    if ok:
        flash(f"✓ Admin {nombre} {apellido} creado", "success")
    else:
        flash(f"Error: {resultado}", "error")
    return redirect(url_for("admin"))


# ───────────────────────── Main ─────────────────────────
# ---- Reproductor de practica (stems / modo ensayo) ----
CARPETA_PISTAS = BASE_DIR / "pistas"
CARPETA_PISTAS.mkdir(exist_ok=True)
_EXT_AUDIO_ENSAYO = (".mp3", ".m4a", ".ogg", ".wav")
FAMILIAS_FIJAS = {"Percusión", "Guía", "Click"}


def _carpeta_tono(numero, n):
    if n == 0:
        return CARPETA_PISTAS / str(numero)
    return CARPETA_PISTAS / str(numero) / ("tono_" + str(n))


def _stems_originales(numero):
    d = CARPETA_PISTAS / str(numero)
    if not d.is_dir():
        return []
    return sorted([f.name for f in d.iterdir() if f.is_file() and f.suffix.lower() in _EXT_AUDIO_ENSAYO])


def _tono_listo(numero, n):
    if n == 0:
        return len(_stems_originales(numero)) > 0
    d = _carpeta_tono(numero, n)
    if not d.is_dir() or (d / ".lock").exists():
        return False
    orig = _stems_originales(numero)
    if not orig:
        return False
    hechos = set(f.name for f in d.iterdir() if f.is_file() and f.suffix.lower() in _EXT_AUDIO_ENSAYO)
    return all(o in hechos for o in orig)


def _render_tono(numero, n):
    d = _carpeta_tono(numero, n)
    d.mkdir(parents=True, exist_ok=True)
    lock = d / ".lock"
    orig = _stems_originales(numero)
    ratio = 2 ** (n / 12.0)
    try:
        lock.write_text("0/" + str(len(orig)))
        hechos = 0
        fam_map = _leer_familias(numero)
        for nombre in orig:
            entrada = CARPETA_PISTAS / str(numero) / nombre
            salida = d / nombre
            if not salida.exists():
                fam = fam_map.get(nombre) or _familia_auto(Path(nombre).stem)
                if fam in FAMILIAS_FIJAS:
                    shutil.copy2(str(entrada), str(salida))
                else:
                    subprocess.run(
                        ["/usr/bin/nice", "-n", "19", "/usr/bin/ffmpeg", "-y", "-i", str(entrada),
                         "-af", "rubberband=pitch=" + repr(ratio), "-b:a", "192k", str(salida)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
            hechos += 1
            try:
                lock.write_text(str(hechos) + "/" + str(len(orig)))
            except Exception:
                pass
    except Exception as e:
        logging.error("render tono %s/%s: %s", numero, n, e)
    finally:
        try:
            lock.unlink()
        except Exception:
            pass


@app.route("/api/pistas/<int:numero>")
@login_required("musico")
def api_pistas(numero):
    try:
        n = int(request.args.get("t", "0"))
    except ValueError:
        n = 0
    listo = _tono_listo(numero, n)
    stems = []
    fam_guardadas = _leer_familias(numero)
    if listo:
        for f in sorted(_carpeta_tono(numero, n).iterdir()):
            if f.is_file() and f.suffix.lower() in _EXT_AUDIO_ENSAYO:
                stems.append({"name": f.stem, "file": f.name,
                              "familia": fam_guardadas.get(f.name) or _familia_auto(f.stem)})
    return jsonify({"numero": numero, "tono": n, "listo": listo,
                    "hay_pistas": len(_stems_originales(numero)) > 0,
                    "stems": stems, "secciones": _leer_secciones(numero)})


@app.route("/api/pistas/<int:numero>/render/<n>", methods=["POST"])
@login_required("musico")
def api_render(numero, n):
    try:
        n = int(n)
    except ValueError:
        return jsonify({"error": "tono invalido"}), 400
    if _tono_listo(numero, n):
        return jsonify({"listo": True})
    if not _stems_originales(numero):
        return jsonify({"listo": False, "error": "sin pistas"})
    d = _carpeta_tono(numero, n)
    if not (d.exists() and (d / ".lock").exists()):
        threading.Thread(target=_render_tono, args=(numero, n), daemon=True).start()
    return jsonify({"listo": False, "estado": "procesando"})


@app.route("/api/pistas/<int:numero>/render/<n>/estado")
@login_required("musico")
def api_render_estado(numero, n):
    try:
        n = int(n)
    except ValueError:
        return jsonify({"listo": False})
    if _tono_listo(numero, n):
        return jsonify({"listo": True})
    prog = ""
    lock = _carpeta_tono(numero, n) / ".lock"
    if lock.exists():
        try:
            prog = lock.read_text().strip()
        except Exception:
            prog = ""
    return jsonify({"listo": False, "progreso": prog})


@app.route("/pista/<int:numero>/<path:archivo>")
@login_required("musico")
def servir_pista(numero, archivo):
    try:
        n = int(request.args.get("t", "0"))
    except ValueError:
        n = 0
    base = _carpeta_tono(numero, n).resolve()
    ruta = (base / archivo).resolve()
    try:
        dentro = os.path.commonpath([str(base), str(ruta)]) == str(base)
    except ValueError:
        dentro = False
    if not dentro or not ruta.is_file():
        abort(404)
    return send_file(str(ruta))


# ---- Admin: gestion de pistas de ensayo ----
@app.route("/admin/pistas")
@login_required("admin")
def admin_pistas():
    biblioteca = cargar_biblioteca()
    songs = []
    for numero in sorted(biblioteca.keys()):
        c = biblioteca[numero]
        carpeta = CARPETA_PISTAS / str(numero)
        n = 0
        if carpeta.is_dir():
            n = sum(1 for f in carpeta.iterdir() if f.is_file() and f.suffix.lower() in _EXT_AUDIO_ENSAYO)
        songs.append({"numero": numero, "titulo": c.get("titulo", ""), "artista": c.get("artista", ""), "n_pistas": n})
    return render_template("admin_pistas.html", songs=songs)


@app.route("/admin/pistas/subir", methods=["POST"])
@login_required("admin")
def admin_pistas_subir():
    numero = request.form.get("numero", "").strip()
    if not numero.isdigit():
        flash("Elegi una cancion valida", "error")
        return redirect(url_for("admin_pistas"))
    archivos = request.files.getlist("pistas")
    carpeta = CARPETA_PISTAS / numero
    carpeta.mkdir(exist_ok=True)
    guardadas = 0
    for a in archivos:
        if not a or a.filename == "":
            continue
        ext = os.path.splitext(a.filename)[1].lower()
        if ext not in _EXT_AUDIO_ENSAYO:
            continue
        nombre = secure_filename(a.filename)
        if not nombre:
            continue
        a.save(str(carpeta / nombre))
        guardadas += 1
    if guardadas:
        flash("OK: " + str(guardadas) + " pista(s) subida(s) a la cancion #" + numero, "success")
    else:
        flash("No se subio ninguna pista (revisa el formato: mp3/m4a/ogg/wav)", "error")
    return redirect(url_for("admin_pistas"))


@app.route("/admin/pistas/eliminar/<int:numero>", methods=["POST"])
@login_required("admin")
def admin_pistas_eliminar(numero):
    carpeta = CARPETA_PISTAS / str(numero)
    borradas = 0
    if carpeta.is_dir():
        for f in list(carpeta.iterdir()):
            if f.is_file() and f.suffix.lower() in _EXT_AUDIO_ENSAYO:
                try:
                    f.unlink()
                    borradas += 1
                except Exception:
                    pass
    flash("OK: " + str(borradas) + " pista(s) eliminada(s) de #" + str(numero), "success")
    return redirect(url_for("admin_pistas"))


# ---- Mapa de secciones (etapa 2) ----
def _archivo_secciones(numero):
    return CARPETA_PISTAS / str(numero) / "secciones.json"


def _leer_secciones(numero):
    f = _archivo_secciones(numero)
    if f.is_file():
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            return []
    return []


def _parse_tiempo(txt):
    txt = txt.strip().replace(",", ".")
    partes = txt.split(":")
    try:
        nums = [float(p) for p in partes]
    except ValueError:
        return None
    if len(nums) == 4:
        h, mi, se, ms = nums; return h*3600 + mi*60 + se + ms/1000.0
    if len(nums) == 3:
        h, mi, se = nums; return h*3600 + mi*60 + se
    if len(nums) == 2:
        mi, se = nums; return mi*60 + se
    if len(nums) == 1:
        return nums[0]
    return None


def _fmt_tiempo(secs):
    ms = int(round((secs - int(secs)) * 1000))
    t = int(secs); h = t // 3600; mi = (t % 3600) // 60; se = t % 60
    return "%02d:%02d:%02d:%03d" % (h, mi, se, ms)


@app.route("/admin/pistas/<int:numero>/secciones", methods=["GET", "POST"])
@login_required("admin")
def admin_secciones(numero):
    biblioteca = cargar_biblioteca()
    cancion = biblioteca.get(numero)
    if not cancion:
        flash("Cancion no encontrada", "error")
        return redirect(url_for("admin_pistas"))
    secs_chart = cancion.get("secciones", [])
    if request.method == "POST":
        guardadas = []
        for i, sec in enumerate(secs_chart):
            tiempo = request.form.get("t_" + str(i), "").strip()
            if not tiempo:
                continue
            val = _parse_tiempo(tiempo)
            if val is None:
                continue
            guardadas.append({"i": i, "nombre": sec.get("tipo", "Seccion"), "t": round(val, 3)})
        guardadas.sort(key=lambda x: x["t"])
        carpeta = CARPETA_PISTAS / str(numero)
        carpeta.mkdir(exist_ok=True)
        _archivo_secciones(numero).write_text(
            json.dumps(guardadas, ensure_ascii=False, indent=2), encoding="utf-8")
        if request.form.get("wizard"):
            flash("Cancion lista: chart, pistas y tiempos cuadrados", "success")
            return redirect(url_for("admin"))
        flash("OK: " + str(len(guardadas)) + " seccion(es) con tiempo guardada(s)", "success")
        return redirect(url_for("admin_secciones", numero=numero))
    guardadas = {sec["i"]: sec["t"] for sec in _leer_secciones(numero) if "i" in sec}
    filas = []
    for i, sec in enumerate(secs_chart):
        filas.append({"i": i, "tipo": sec.get("tipo", "Seccion"), "nota": sec.get("nota", ""),
                      "t": _fmt_tiempo(guardadas[i]) if i in guardadas else ""})
    titulo = cancion.get("titulo", "Cancion #" + str(numero))
    return render_template("admin_secciones.html", numero=numero, titulo=titulo, filas=filas,
                           wizard=bool(request.args.get("wizard")))


@app.route("/admin/nueva", methods=["GET", "POST"])
@login_required("admin")
def admin_nueva():
    if request.method == "POST":
        archivo = request.files.get("archivo")
        if not archivo or archivo.filename == "":
            flash("Selecciona el archivo JSON", "error")
            return redirect(url_for("admin_nueva"))
        if not archivo.filename.endswith(".json"):
            flash("Solo se aceptan archivos .json", "error")
            return redirect(url_for("admin_nueva"))
        try:
            contenido = archivo.read()
            datos = json.loads(contenido.decode("utf-8"))
            if "numero" not in datos or "titulo" not in datos or "secciones" not in datos:
                flash("El JSON no tiene los campos requeridos (numero, titulo, secciones)", "error")
                return redirect(url_for("admin_nueva"))
        except Exception:
            flash("Archivo no es JSON valido", "error")
            return redirect(url_for("admin_nueva"))
        (CARPETA_CANCIONES / secure_filename(archivo.filename)).write_bytes(contenido)
        flash("Chart cargado: " + str(datos["titulo"]), "success")
        return redirect(url_for("admin_nueva_pistas", numero=int(datos["numero"])))
    return render_template("admin_nueva.html")


@app.route("/admin/nueva/<int:numero>/pistas", methods=["GET", "POST"])
@login_required("admin")
def admin_nueva_pistas(numero):
    biblioteca = cargar_biblioteca()
    cancion = biblioteca.get(numero)
    titulo = cancion.get("titulo", "Cancion #" + str(numero)) if cancion else "Cancion #" + str(numero)
    if request.method == "POST":
        carpeta = CARPETA_PISTAS / str(numero)
        carpeta.mkdir(exist_ok=True)
        guardadas = 0
        for a in request.files.getlist("pistas"):
            if not a or a.filename == "":
                continue
            if os.path.splitext(a.filename)[1].lower() not in _EXT_AUDIO_ENSAYO:
                continue
            nombre = secure_filename(a.filename)
            if not nombre:
                continue
            a.save(str(carpeta / nombre))
            guardadas += 1
        flash("OK: " + str(guardadas) + " pista(s) subida(s)", "success")
        return redirect(url_for("admin_secciones", numero=numero, wizard=1))
    return render_template("admin_nueva_pistas.html", numero=numero, titulo=titulo)


FAMILIAS = ["Voces", "Guitarras", "Teclados", "Cuerdas", "Metales", "Bajo",
            "Percusión", "Guía", "Música original", "Click", "Otros"]


def _familia_auto(nombre):
    n = " " + re.sub(r"[_\-.]+", " ", nombre.lower()) + " "
    if re.search(r"click|metr", n):
        return "Click"
    if re.search(r" voz | voces| vocal| coro| lead| choir| vox ", n):
        return "Voces"
    if re.search(r"guitarra|guit|gtr|ac.stic|el.ctric| ag | eg | ga | ge | g\d", n):
        return "Guitarras"
    if re.search(r"teclado| tecla|keys|piano| pad|synth| sint|organo| k\d", n):
        return "Teclados"
    if re.search(r"string|cuerda|viol|cello", n):
        return "Cuerdas"
    if re.search(r"trompeta|trumpet|sax|trombon|brass|metal", n):
        return "Metales"
    if re.search(r" bajo|bass", n):
        return "Bajo"
    if re.search(r"bater|drum|percu| perc|kick|snare| hat| tom|platillo|cymbal|shaker|conga|tambor|loop", n):
        return "Percusión"
    if re.search(r"guide|gu.a| cue |click.?guide", n):
        return "Guía"
    if re.search(r"original|mezcla|master|banda| full | todo ", n):
        return "Música original"
    return "Otros"


def _leer_familias(numero):
    f = CARPETA_PISTAS / str(numero) / "familias.json"
    if f.is_file():
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


@app.route("/admin/pistas/<int:numero>/editar")
@login_required("admin")
def admin_editar(numero):
    biblioteca = cargar_biblioteca()
    cancion = biblioteca.get(numero)
    if not cancion:
        flash("Cancion no encontrada", "error")
        return redirect(url_for("admin_pistas"))
    fam_guardadas = _leer_familias(numero)
    lista = []
    for f in _stems_originales(numero):
        base = f.rsplit(".", 1)[0]
        lista.append({"file": f, "name": base, "familia": fam_guardadas.get(f) or _familia_auto(base)})
    tonos = [{"n": n, "listo": _tono_listo(numero, n)} for n in [-5, -4, -3, -2, -1, 1, 2, 3, 4, 5]]
    return render_template("admin_editar.html", numero=numero, titulo=cancion.get("titulo", ""),
                           stems=lista, familias=FAMILIAS, tonos=tonos,
                           n_secciones=len(_leer_secciones(numero)))


@app.route("/admin/pistas/<int:numero>/familias", methods=["POST"])
@login_required("admin")
def admin_familias(numero):
    fam = {}
    for f in _stems_originales(numero):
        v = request.form.get("fam_" + f, "").strip()
        if v:
            fam[f] = v
    (CARPETA_PISTAS / str(numero) / "familias.json").write_text(
        json.dumps(fam, ensure_ascii=False, indent=2), encoding="utf-8")
    flash("Familias guardadas", "success")
    return redirect(url_for("admin_editar", numero=numero))


@app.route("/admin/pistas/<int:numero>/agregar", methods=["POST"])
@login_required("admin")
def admin_editar_subir(numero):
    carpeta = CARPETA_PISTAS / str(numero)
    carpeta.mkdir(exist_ok=True)
    guardadas = 0
    for a in request.files.getlist("pistas"):
        if not a or a.filename == "":
            continue
        if os.path.splitext(a.filename)[1].lower() not in _EXT_AUDIO_ENSAYO:
            continue
        nombre = secure_filename(a.filename)
        if not nombre:
            continue
        a.save(str(carpeta / nombre))
        guardadas += 1
    flash("OK: " + str(guardadas) + " pista(s) agregada(s)", "success")
    return redirect(url_for("admin_editar", numero=numero))


@app.route("/admin/pistas/<int:numero>/borrar_una", methods=["POST"])
@login_required("admin")
def admin_pista_borrar_una(numero):
    archivo = secure_filename(request.form.get("archivo", ""))
    if archivo:
        ruta = CARPETA_PISTAS / str(numero) / archivo
        if ruta.is_file():
            try:
                ruta.unlink()
                flash("Pista eliminada: " + archivo, "success")
            except Exception:
                flash("No se pudo eliminar", "error")
    return redirect(url_for("admin_editar", numero=numero))


@app.route("/admin/pistas/<int:numero>/pregenerar", methods=["POST"])
@login_required("admin")
def admin_pregenerar(numero):
    try:
        n = int(request.form.get("tono", "0"))
    except ValueError:
        n = 0
    if n == 0 or not _stems_originales(numero):
        flash("Tono invalido o sin pistas", "error")
        return redirect(url_for("admin_editar", numero=numero))
    if _tono_listo(numero, n):
        flash("El tono " + str(n) + " ya estaba listo", "success")
    else:
        d = _carpeta_tono(numero, n)
        if not (d.exists() and (d / ".lock").exists()):
            threading.Thread(target=_render_tono, args=(numero, n), daemon=True).start()
        flash("Generando el tono " + ("+" if n > 0 else "") + str(n) +
              " en segundo plano. Puede tardar unos minutos; refresca para ver cuando este listo.", "success")
    return redirect(url_for("admin_editar", numero=numero))


if __name__ == "__main__":
    # En desarrollo local (no en VPS), correr en puerto 5051
    # En VPS, esto lo manejará gunicorn o systemd
    app.run(host="127.0.0.1", port=5051, debug=False)

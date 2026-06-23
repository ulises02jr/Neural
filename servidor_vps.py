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
        # IMPORTANTE: el admin debe cambiarlos después
        default = {
            "password_musicos": hash_password("musicos2026"),
            "password_admin": hash_password("admin2026"),
            "secret_key": secrets.token_hex(32),
            "setlist": [],
        }
        with open(ARCHIVO_CONFIG, "w", encoding="utf-8") as f:
            json.dump(default, f, indent=2)
        print("⚠️  config.json creado con passwords por defecto.")
        print("    Cámbialos editando el archivo.")
        return default
    with open(ARCHIVO_CONFIG, "r", encoding="utf-8") as f:
        return json.load(f)


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
    # Setlist: lista de objetos canción en el orden del setlist
    setlist_canciones = []
    cfg = get_config()
    for num in cfg.get("setlist", []):
        if num in biblioteca:
            setlist_canciones.append(biblioteca[num])
    # Biblioteca completa ordenada por número
    biblioteca_ordenada = sorted(biblioteca.values(), key=lambda c: c["numero"])
    return render_template(
        "principal.html",
        setlist=setlist_canciones,
        biblioteca=biblioteca_ordenada,
        es_admin=(session.get("rol") == "admin"),
    )


@app.route("/cancion/<int:numero>")
@login_required("musico")
def ver_cancion(numero):
    biblioteca = cargar_biblioteca()
    if numero not in biblioteca:
        abort(404)
    
    # Soporte de transposición: ?t=N donde N es semitonos (-12 a +12)
    # Y origen: ?from=bib indica que se accedió desde biblioteca (se muestran botones +/-)
    cancion = biblioteca[numero]
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
    return render_template(
        "admin.html",
        biblioteca=biblioteca_ordenada,
        setlist=get_config().get("setlist", []),
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
    # Validar que es un JSON válido y tiene los campos requeridos
    try:
        contenido = archivo.read()
        datos = json.loads(contenido.decode("utf-8"))
        if "numero" not in datos or "titulo" not in datos or "secciones" not in datos:
            flash("El JSON no tiene los campos requeridos (numero, titulo, secciones)", "error")
            return redirect(url_for("admin"))
    except json.JSONDecodeError:
        flash("Archivo no es JSON válido", "error")
        return redirect(url_for("admin"))
    # Guardar
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
    # Buscar el archivo .json correspondiente
    for archivo in CARPETA_CANCIONES.glob("*.json"):
        try:
            with open(archivo, "r", encoding="utf-8") as f:
                datos = json.load(f)
            if datos.get("numero") == numero:
                archivo.unlink()
                flash(f"✓ Canción #{numero} eliminada", "success")
                # Removerla del setlist si estaba
                cfg = get_config()
                if numero in cfg.get("setlist", []):
                    cfg["setlist"].remove(numero)
                    guardar_config(config)
                break
        except Exception:
            continue
    return redirect(url_for("admin"))


@app.route("/admin/setlist", methods=["POST"])
@login_required("admin")
def admin_setlist():
    # Recibe la lista de números en orden (form field "setlist", coma-separado)
    setlist_str = request.form.get("setlist", "")
    try:
        nuevo_setlist = [int(x.strip()) for x in setlist_str.split(",") if x.strip()]
        cfg = get_config()
        cfg["setlist"] = nuevo_setlist
        guardar_config(cfg)
        flash("✓ Setlist actualizado", "success")
    except ValueError:
        flash("Lista de números inválida", "error")
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


# ───────────────────────── Main ─────────────────────────
if __name__ == "__main__":
    # En desarrollo local (no en VPS), correr en puerto 5051
    # En VPS, esto lo manejará gunicorn o systemd
    app.run(host="127.0.0.1", port=5051, debug=False)

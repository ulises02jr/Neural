"""
Gestión de usuarios del sistema.
Usa SQLite para almacenar usuarios, sesiones y códigos de reset.

Schema:
  usuarios:
    id INTEGER PRIMARY KEY
    nombre TEXT
    apellido TEXT
    email TEXT UNIQUE
    password_hash TEXT
    rol TEXT ('musico' | 'admin')
    estado TEXT ('pendiente' | 'activo' | 'rechazado')
    creado_en TEXT (ISO datetime)
    aprobado_en TEXT (NULL si no aprobado)

  reset_codigos:
    id INTEGER PRIMARY KEY
    user_id INTEGER (FK usuarios.id)
    codigo TEXT (6 dígitos)
    expira_en TEXT (ISO datetime)
    usado INTEGER (0 o 1)
"""
import sqlite3
import hashlib
import secrets
import random
from datetime import datetime, timedelta
from pathlib import Path
from werkzeug.security import generate_password_hash, check_password_hash

ARCHIVO_DB = Path(__file__).parent / "usuarios.db"
CODIGO_RESET_DURACION_MIN = 15


def _conexion():
    conn = sqlite3.connect(ARCHIVO_DB)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    """Crea las tablas si no existen."""
    with _conexion() as conn:
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS usuarios (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                nombre TEXT NOT NULL,
                apellido TEXT NOT NULL,
                email TEXT UNIQUE NOT NULL,
                password_hash TEXT NOT NULL,
                rol TEXT NOT NULL DEFAULT 'musico',
                estado TEXT NOT NULL DEFAULT 'pendiente',
                creado_en TEXT NOT NULL,
                aprobado_en TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_usuarios_email ON usuarios(email);
            CREATE INDEX IF NOT EXISTS idx_usuarios_estado ON usuarios(estado);

            CREATE TABLE IF NOT EXISTS reset_codigos (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id INTEGER NOT NULL,
                codigo TEXT NOT NULL,
                expira_en TEXT NOT NULL,
                usado INTEGER NOT NULL DEFAULT 0,
                FOREIGN KEY (user_id) REFERENCES usuarios(id)
            );
            CREATE INDEX IF NOT EXISTS idx_reset_codigo ON reset_codigos(codigo);

            CREATE TABLE IF NOT EXISTS login_intentos (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ip TEXT NOT NULL,
                ts TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_login_intentos ON login_intentos(ip, ts);
        """)
        try:
            conn.execute("ALTER TABLE usuarios ADD COLUMN acento TEXT")
        except Exception:
            pass  # ya existe
        try:
            conn.execute("ALTER TABLE usuarios ADD COLUMN prefs TEXT")
        except Exception:
            pass  # ya existe


def hash_password(password):
    """Hash seguro con PBKDF2-SHA256 (werkzeug)."""
    return generate_password_hash(password, method="pbkdf2:sha256")


def _es_hash_antiguo(h):
    """True si es el formato viejo salt$sha256 (sin prefijo de metodo werkzeug)."""
    return bool(h) and "$" in h and not h.startswith(("pbkdf2:", "scrypt:", "argon2"))


def verificar_password(password, password_hash):
    if not password_hash:
        return False
    if _es_hash_antiguo(password_hash):
        salt, h = password_hash.split("$", 1)
        return hashlib.sha256((salt + password).encode()).hexdigest() == h
    try:
        return check_password_hash(password_hash, password)
    except Exception:
        return False


def necesita_rehash(password_hash):
    return _es_hash_antiguo(password_hash)


# Rate limiting de intentos de login
LOGIN_MAX_INTENTOS = 8
LOGIN_VENTANA_MIN = 10


def registrar_intento(ip):
    try:
        with _conexion() as conn:
            conn.execute("INSERT INTO login_intentos (ip, ts) VALUES (?, ?)", (ip or "?", _ahora_iso()))
    except Exception:
        pass


def login_bloqueado(ip):
    try:
        limite = (datetime.utcnow() - timedelta(minutes=LOGIN_VENTANA_MIN)).isoformat(timespec="seconds")
        with _conexion() as conn:
            n = conn.execute("SELECT COUNT(*) FROM login_intentos WHERE ip = ? AND ts > ?", (ip or "?", limite)).fetchone()[0]
        return n >= LOGIN_MAX_INTENTOS
    except Exception:
        return False


def limpiar_intentos(ip):
    try:
        with _conexion() as conn:
            conn.execute("DELETE FROM login_intentos WHERE ip = ?", (ip or "?",))
    except Exception:
        pass


def _ahora_iso():
    return datetime.utcnow().isoformat(timespec="seconds")


def crear_usuario(nombre, apellido, email, password, rol="musico", estado="pendiente"):
    """Crea un usuario nuevo. Devuelve (ok, user_id_o_error)."""
    email = email.strip().lower()
    if not nombre or not apellido or not email or not password:
        return False, "Todos los campos son obligatorios"
    if "@" not in email or "." not in email:
        return False, "Email inválido"
    if len(password) < 6:
        return False, "La contraseña debe tener al menos 6 caracteres"
    try:
        with _conexion() as conn:
            cur = conn.execute(
                "INSERT INTO usuarios (nombre, apellido, email, password_hash, rol, estado, creado_en, aprobado_en) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    nombre.strip(), apellido.strip(), email,
                    hash_password(password), rol, estado,
                    _ahora_iso(),
                    _ahora_iso() if estado == "activo" else None,
                ),
            )
            return True, cur.lastrowid
    except sqlite3.IntegrityError:
        return False, "Ya existe una cuenta con ese email"
    except Exception as e:
        return False, f"Error: {e}"


def buscar_por_email(email):
    """Devuelve dict del usuario o None."""
    email = email.strip().lower()
    with _conexion() as conn:
        row = conn.execute("SELECT * FROM usuarios WHERE email = ?", (email,)).fetchone()
        return dict(row) if row else None


def obtener_acento(user_id):
    """Color de acento guardado del usuario (o None)."""
    if not user_id:
        return None
    try:
        with _conexion() as conn:
            row = conn.execute("SELECT acento FROM usuarios WHERE id = ?", (user_id,)).fetchone()
            return (row["acento"] if row else None) or None
    except Exception:
        return None


def guardar_acento(user_id, color):
    """Guarda (o limpia) el color de acento del usuario. Solo acepta #rrggbb."""
    if not user_id:
        return False
    import re as _re
    val = None
    if color:
        color = str(color).strip()
        if _re.match(r"^#[0-9a-fA-F]{6}$", color):
            val = color.upper()
        else:
            return False
    try:
        with _conexion() as conn:
            conn.execute("UPDATE usuarios SET acento = ? WHERE id = ?", (val, user_id))
        return True
    except Exception:
        return False


def obtener_prefs(user_id):
    """Preferencias de vista del visor (JSON string) del usuario, o '{}'."""
    if not user_id:
        return "{}"
    try:
        with _conexion() as conn:
            row = conn.execute("SELECT prefs FROM usuarios WHERE id = ?", (user_id,)).fetchone()
            v = (row["prefs"] if row else None)
            return v if v else "{}"
    except Exception:
        return "{}"


def guardar_prefs(user_id, data):
    """Guarda preferencias de vista del visor (dict) con whitelist + validacion."""
    if not user_id or not isinstance(data, dict):
        return False
    import json as _json, re as _re
    out = {}
    tema = str(data.get("tema", "")).strip()
    if tema in ("oscuro", "claro"):
        out["tema"] = tema
    modo = str(data.get("modo", "")).strip()
    if modo in ("ambos", "acordes", "letra"):
        out["modo"] = modo
    grosor = str(data.get("grosor", "")).strip()
    if grosor in ("fino", "normal", "grueso"):
        out["grosor"] = grosor
    tam = str(data.get("tam", "")).strip()
    if tam in ("xs", "s", "m", "l", "xl"):
        out["tam"] = tam
    color = str(data.get("color", "")).strip()
    if color == "":
        out["color"] = ""
    elif _re.match(r"^#[0-9a-fA-F]{6}$", color):
        out["color"] = color.upper()
    try:
        with _conexion() as conn:
            conn.execute("UPDATE usuarios SET prefs = ? WHERE id = ?", (_json.dumps(out), user_id))
        return True
    except Exception:
        return False


def buscar_por_id(user_id):
    with _conexion() as conn:
        row = conn.execute("SELECT * FROM usuarios WHERE id = ?", (user_id,)).fetchone()
        return dict(row) if row else None


def listar_usuarios(estado=None):
    """Lista todos los usuarios o filtra por estado."""
    with _conexion() as conn:
        if estado:
            rows = conn.execute(
                "SELECT * FROM usuarios WHERE estado = ? ORDER BY creado_en DESC", (estado,)
            ).fetchall()
        else:
            rows = conn.execute("SELECT * FROM usuarios ORDER BY creado_en DESC").fetchall()
        return [dict(r) for r in rows]


def listar_perfiles():
    """Perfiles (id, nombre, acento, prefs) de usuarios activos, para el visor local de NeuralPlay."""
    try:
        with _conexion() as conn:
            rows = conn.execute(
                "SELECT id, nombre, apellido, acento, prefs FROM usuarios WHERE estado = 'activo' ORDER BY nombre COLLATE NOCASE"
            ).fetchall()
        out = []
        for r in rows:
            nom = (r["nombre"] or "").strip()
            ap = (r["apellido"] or "").strip()
            full = (nom + " " + ap).strip() or nom or ("usuario " + str(r["id"]))
            out.append({"id": r["id"], "nombre": full, "acento": r["acento"] or "", "prefs": r["prefs"] or "{}"})
        return out
    except Exception:
        return []


def aprobar_usuario(user_id):
    with _conexion() as conn:
        cur = conn.execute(
            "UPDATE usuarios SET estado = 'activo', aprobado_en = ? WHERE id = ? AND estado = 'pendiente'",
            (_ahora_iso(), user_id),
        )
        return cur.rowcount > 0


def eliminar_usuario(user_id):
    with _conexion() as conn:
        conn.execute("DELETE FROM reset_codigos WHERE user_id = ?", (user_id,))
        cur = conn.execute("DELETE FROM usuarios WHERE id = ?", (user_id,))
        return cur.rowcount > 0


def autenticar(email, password):
    """Login: valida email + password. Devuelve user dict o None."""
    u = buscar_por_email(email)
    if not u:
        return None
    if u["estado"] != "activo":
        return None
    if not verificar_password(password, u["password_hash"]):
        return None
    if necesita_rehash(u["password_hash"]):
        try:
            nuevo = hash_password(password)
            with _conexion() as conn:
                conn.execute("UPDATE usuarios SET password_hash = ? WHERE id = ?", (nuevo, u["id"]))
        except Exception:
            pass
    return u


def crear_codigo_reset(user_id):
    """Crea un código de 6 dígitos para reset de password. Invalida los anteriores."""
    codigo = f"{random.randint(0, 999999):06d}"
    expira = (datetime.utcnow() + timedelta(minutes=CODIGO_RESET_DURACION_MIN)).isoformat(timespec="seconds")
    with _conexion() as conn:
        # Invalidar códigos anteriores no usados
        conn.execute("UPDATE reset_codigos SET usado = 1 WHERE user_id = ? AND usado = 0", (user_id,))
        conn.execute(
            "INSERT INTO reset_codigos (user_id, codigo, expira_en, usado) VALUES (?, ?, ?, 0)",
            (user_id, codigo, expira),
        )
    return codigo


def validar_codigo_reset(email, codigo):
    """Verifica que el código sea válido para ese email. Devuelve user_id o None."""
    u = buscar_por_email(email)
    if not u:
        return None
    with _conexion() as conn:
        row = conn.execute(
            "SELECT * FROM reset_codigos WHERE user_id = ? AND codigo = ? AND usado = 0",
            (u["id"], codigo.strip()),
        ).fetchone()
        if not row:
            return None
        # Verificar que no haya expirado
        expira = datetime.fromisoformat(row["expira_en"])
        if datetime.utcnow() > expira:
            return None
        return u["id"]


def usar_codigo_y_cambiar_password(email, codigo, nueva_password):
    """Marca el código como usado y cambia la password."""
    if len(nueva_password) < 6:
        return False, "La contraseña debe tener al menos 6 caracteres"
    user_id = validar_codigo_reset(email, codigo)
    if not user_id:
        return False, "Código inválido o expirado"
    with _conexion() as conn:
        conn.execute(
            "UPDATE usuarios SET password_hash = ? WHERE id = ?",
            (hash_password(nueva_password), user_id),
        )
        conn.execute(
            "UPDATE reset_codigos SET usado = 1 WHERE codigo = ? AND user_id = ?",
            (codigo.strip(), user_id),
        )
    return True, "Contraseña cambiada correctamente"


def cambiar_password_directo(user_id, nueva_password):
    """Cambia directamente la contraseña (hash) de una cuenta. Devuelve (ok, msg)."""
    if len(nueva_password) < 6:
        return False, "La contraseña debe tener al menos 6 caracteres"
    with _conexion() as conn:
        cur = conn.execute(
            "UPDATE usuarios SET password_hash = ? WHERE id = ?",
            (hash_password(nueva_password), user_id),
        )
        if cur.rowcount == 0:
            return False, "Usuario no encontrado"
    return True, "Contraseña actualizada"


def actualizar_perfil(user_id, nombre, apellido, email):
    """Actualiza nombre, apellido y email de una cuenta. Devuelve (ok, msg)."""
    nombre = (nombre or "").strip()
    apellido = (apellido or "").strip()
    email = (email or "").strip().lower()
    if not nombre or not apellido or not email:
        return False, "Nombre, apellido y email son obligatorios"
    if "@" not in email or "." not in email:
        return False, "Email inválido"
    with _conexion() as conn:
        row = conn.execute("SELECT id FROM usuarios WHERE email = ? AND id != ?", (email, user_id)).fetchone()
        if row:
            return False, "Ya existe otra cuenta con ese email"
        cur = conn.execute(
            "UPDATE usuarios SET nombre = ?, apellido = ?, email = ? WHERE id = ?",
            (nombre, apellido, email, user_id),
        )
        if cur.rowcount == 0:
            return False, "Usuario no encontrado"
    return True, "Perfil actualizado"


def contar_admins_activos():
    """Cuántos admins activos hay (para evitar quedar sin admins)."""
    with _conexion() as conn:
        row = conn.execute(
            "SELECT COUNT(*) AS n FROM usuarios WHERE rol = 'admin' AND estado = 'activo'"
        ).fetchone()
        return row["n"]


if __name__ == "__main__":
    init_db()
    print(f"✓ Base de datos inicializada en {ARCHIVO_DB}")

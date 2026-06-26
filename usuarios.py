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
        """)


def hash_password(password):
    """Hash de password con SHA-256 + salt."""
    salt = secrets.token_hex(16)
    h = hashlib.sha256((salt + password).encode()).hexdigest()
    return f"{salt}${h}"


def verificar_password(password, password_hash):
    if "$" not in password_hash:
        return False
    salt, h = password_hash.split("$", 1)
    return hashlib.sha256((salt + password).encode()).hexdigest() == h


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

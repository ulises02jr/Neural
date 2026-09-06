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


# Código de unión de organización (corto, sin caracteres ambiguos)
_COD_ALFA = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def _codigo_libre(conn):
    """Genera un código de organización de 6 caracteres que no exista aún."""
    for _ in range(50):
        c = "".join(secrets.choice(_COD_ALFA) for _ in range(6))
        if not conn.execute("SELECT 1 FROM organizations WHERE codigo = ?", (c,)).fetchone():
            return c
    return "".join(secrets.choice(_COD_ALFA) for _ in range(8))


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

            CREATE TABLE IF NOT EXISTS organizations (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                nombre TEXT NOT NULL,
                owner_user_id INTEGER,
                token TEXT UNIQUE,
                paquete TEXT NOT NULL DEFAULT 'basico',
                max_musicos INTEGER NOT NULL DEFAULT 3,
                almacen_gb INTEGER NOT NULL DEFAULT 20,
                estado_suscripcion TEXT NOT NULL DEFAULT 'activa',
                creado_en TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_org_token ON organizations(token);

            CREATE TABLE IF NOT EXISTS invitaciones (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                org_id INTEGER NOT NULL,
                email TEXT NOT NULL,
                token TEXT UNIQUE NOT NULL,
                estado TEXT NOT NULL DEFAULT 'pendiente',
                creado_en TEXT NOT NULL,
                expira_en TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_invit_token ON invitaciones(token);
            CREATE INDEX IF NOT EXISTS idx_invit_org ON invitaciones(org_id);
        """)
        try:
            conn.execute("ALTER TABLE usuarios ADD COLUMN acento TEXT")
        except Exception:
            pass  # ya existe
        try:
            conn.execute("ALTER TABLE usuarios ADD COLUMN prefs TEXT")
        except Exception:
            pass  # ya existe
        # Multi-tenant: cada usuario pertenece a una organización.
        try:
            conn.execute("ALTER TABLE usuarios ADD COLUMN org_id INTEGER")
        except Exception:
            pass  # ya existe
        try:
            conn.execute("CREATE INDEX IF NOT EXISTS idx_usuarios_org ON usuarios(org_id)")
        except Exception:
            pass
        # Código de unión por organización
        try:
            conn.execute("ALTER TABLE organizations ADD COLUMN codigo TEXT")
        except Exception:
            pass  # ya existe
        try:
            faltantes = conn.execute(
                "SELECT id FROM organizations WHERE codigo IS NULL OR codigo = ''"
            ).fetchall()
            for r in faltantes:
                conn.execute("UPDATE organizations SET codigo = ? WHERE id = ?",
                             (_codigo_libre(conn), r["id"]))
        except Exception:
            pass


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


def crear_usuario(nombre, apellido, email, password, rol="musico", estado="pendiente", org_id=None):
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
                "INSERT INTO usuarios (nombre, apellido, email, password_hash, rol, estado, creado_en, aprobado_en, org_id) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    nombre.strip(), apellido.strip(), email,
                    hash_password(password), rol, estado,
                    _ahora_iso(),
                    _ahora_iso() if estado == "activo" else None,
                    org_id,
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


def listar_usuarios(estado=None, org_id=None):
    """Lista usuarios; filtra opcionalmente por estado y/o organización."""
    where, params = [], []
    if estado:
        where.append("estado = ?")
        params.append(estado)
    if org_id:
        where.append("org_id = ?")
        params.append(org_id)
    sql = "SELECT * FROM usuarios"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY creado_en DESC"
    with _conexion() as conn:
        rows = conn.execute(sql, params).fetchall()
        return [dict(r) for r in rows]


def listar_perfiles(org_id=None):
    """Perfiles (id, nombre, acento, prefs) de usuarios activos de una organización,
    para el visor local de NeuralPlay ('¿Quién sos?')."""
    try:
        with _conexion() as conn:
            if org_id:
                rows = conn.execute(
                    "SELECT id, nombre, apellido, acento, prefs FROM usuarios WHERE estado = 'activo' AND org_id = ? ORDER BY nombre COLLATE NOCASE",
                    (org_id,),
                ).fetchall()
            else:
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


def aprobar_usuario(user_id, org_id=None):
    with _conexion() as conn:
        if org_id:
            cur = conn.execute(
                "UPDATE usuarios SET estado = 'activo', aprobado_en = ? WHERE id = ? AND estado = 'pendiente' AND org_id = ?",
                (_ahora_iso(), user_id, org_id),
            )
        else:
            cur = conn.execute(
                "UPDATE usuarios SET estado = 'activo', aprobado_en = ? WHERE id = ? AND estado = 'pendiente'",
                (_ahora_iso(), user_id),
            )
        return cur.rowcount > 0


def eliminar_usuario(user_id, org_id=None):
    with _conexion() as conn:
        conn.execute("DELETE FROM reset_codigos WHERE user_id = ?", (user_id,))
        if org_id:
            cur = conn.execute("DELETE FROM usuarios WHERE id = ? AND org_id = ?", (user_id, org_id))
        else:
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


# ---------------------------------------------------------------------------
# Multi-tenant: organizaciones
# ---------------------------------------------------------------------------

def crear_organizacion(nombre, owner_user_id=None, token=None, paquete="basico",
                       max_musicos=3, almacen_gb=20, estado="activa"):
    """Crea una organización. Devuelve (ok, org_id_o_error).
    Si no se pasa token, se genera uno seguro."""
    nombre = (nombre or "").strip() or "Organización"
    if not token:
        token = secrets.token_hex(16)
    try:
        with _conexion() as conn:
            codigo = _codigo_libre(conn)
            cur = conn.execute(
                "INSERT INTO organizations (nombre, owner_user_id, token, codigo, paquete, max_musicos, almacen_gb, estado_suscripcion, creado_en) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (nombre, owner_user_id, token, codigo, paquete, int(max_musicos), int(almacen_gb), estado, _ahora_iso()),
            )
            return True, cur.lastrowid
    except sqlite3.IntegrityError as e:
        return False, f"Token duplicado o error de integridad: {e}"
    except Exception as e:
        return False, f"Error: {e}"


def obtener_organizacion(org_id):
    """Devuelve dict de la organización o None."""
    if not org_id:
        return None
    with _conexion() as conn:
        row = conn.execute("SELECT * FROM organizations WHERE id = ?", (org_id,)).fetchone()
        return dict(row) if row else None


def obtener_org_por_token(token):
    """Devuelve la organización dueña de ese token, o None."""
    if not token:
        return None
    with _conexion() as conn:
        row = conn.execute("SELECT * FROM organizations WHERE token = ?", (token,)).fetchone()
        return dict(row) if row else None


def buscar_org_por_codigo(codigo):
    """Devuelve la organización por su código de unión (case-insensitive), o None."""
    if not codigo:
        return None
    codigo = str(codigo).strip().upper()
    with _conexion() as conn:
        row = conn.execute("SELECT * FROM organizations WHERE UPPER(codigo) = ?", (codigo,)).fetchone()
        return dict(row) if row else None


def contar_musicos_activos(org_id):
    """Cuántos músicos activos tiene una organización (para el límite de asientos)."""
    if not org_id:
        return 0
    with _conexion() as conn:
        return conn.execute(
            "SELECT COUNT(*) FROM usuarios WHERE rol = 'musico' AND estado = 'activo' AND org_id = ?",
            (org_id,),
        ).fetchone()[0]


def contar_usuarios(org_id):
    """Total de usuarios de una organización (admins + músicos)."""
    if not org_id:
        return 0
    with _conexion() as conn:
        return conn.execute("SELECT COUNT(*) FROM usuarios WHERE org_id = ?", (org_id,)).fetchone()[0]


def actualizar_organizacion(org_id, paquete=None, max_musicos=None, almacen_gb=None, estado_suscripcion=None):
    """Actualiza campos de una organización (para el súper-admin)."""
    sets, params = [], []
    if paquete is not None:
        sets.append("paquete = ?"); params.append(paquete)
    if max_musicos is not None:
        sets.append("max_musicos = ?"); params.append(int(max_musicos))
    if almacen_gb is not None:
        sets.append("almacen_gb = ?"); params.append(int(almacen_gb))
    if estado_suscripcion is not None:
        sets.append("estado_suscripcion = ?"); params.append(estado_suscripcion)
    if not sets:
        return False
    params.append(org_id)
    with _conexion() as conn:
        cur = conn.execute("UPDATE organizations SET " + ", ".join(sets) + " WHERE id = ?", params)
        return cur.rowcount > 0


def org_de_usuario(user_id):
    """Devuelve el org_id del usuario (o None)."""
    if not user_id:
        return None
    try:
        with _conexion() as conn:
            row = conn.execute("SELECT org_id FROM usuarios WHERE id = ?", (user_id,)).fetchone()
            return (row["org_id"] if row else None)
    except Exception:
        return None


def listar_organizaciones():
    with _conexion() as conn:
        rows = conn.execute("SELECT * FROM organizations ORDER BY id").fetchall()
        return [dict(r) for r in rows]


def asignar_org(user_id, org_id):
    """Asigna un usuario a una organización."""
    with _conexion() as conn:
        cur = conn.execute("UPDATE usuarios SET org_id = ? WHERE id = ?", (org_id, user_id))
        return cur.rowcount > 0


def asegurar_org_inicial(nombre="Neural Worship", token=None, paquete="ministerio",
                         max_musicos=10, almacen_gb=100):
    """Migración one-time (idempotente): si NO existe ninguna organización, crea la #1
    con el admin activo más antiguo como dueño y asigna org_id a todos los usuarios sin org.
    Si ya hay organizaciones, no hace nada.
    Devuelve (creada: bool, org_id_o_None, msg)."""
    with _conexion() as conn:
        n = conn.execute("SELECT COUNT(*) FROM organizations").fetchone()[0]
    if n > 0:
        return False, None, "Ya existen organizaciones; no se hace nada."
    # Dueño = primer admin activo (por id más bajo)
    with _conexion() as conn:
        row = conn.execute(
            "SELECT id FROM usuarios WHERE rol='admin' AND estado='activo' ORDER BY id LIMIT 1"
        ).fetchone()
    owner_id = row["id"] if row else None
    ok, res = crear_organizacion(nombre, owner_user_id=owner_id, token=token,
                                 paquete=paquete, max_musicos=max_musicos,
                                 almacen_gb=almacen_gb, estado="activa")
    if not ok:
        return False, None, f"No se pudo crear la organización: {res}"
    org_id = res
    with _conexion() as conn:
        cur = conn.execute("UPDATE usuarios SET org_id = ? WHERE org_id IS NULL", (org_id,))
        migrados = cur.rowcount
    return True, org_id, f"Organización #{org_id} '{nombre}' creada (dueño user_id={owner_id}); {migrados} usuario(s) migrado(s)."


def crear_org_con_dueno(org_nombre, nombre, apellido, email, password,
                        paquete="basico", max_musicos=3, almacen_gb=20):
    """Crea una organización nueva + su usuario admin dueño (activo), atómicamente.
    Devuelve (ok, dict_con_datos | mensaje_error).
    dict = {org_id, user_id, token, nombre_org}."""
    email = (email or "").strip().lower()
    if not org_nombre or not (org_nombre or "").strip():
        return False, "El nombre de la organización es obligatorio"
    # Validar email libre ANTES de crear la organización
    if buscar_por_email(email):
        return False, "Ya existe una cuenta con ese email"
    ok, org_id = crear_organizacion(org_nombre, paquete=paquete,
                                    max_musicos=max_musicos, almacen_gb=almacen_gb,
                                    estado="prueba")
    if not ok:
        return False, org_id  # mensaje de error
    ok2, res = crear_usuario(nombre, apellido, email, password, rol="admin", estado="activo")
    if not ok2:
        # Rollback: borrar la organización recién creada
        try:
            with _conexion() as conn:
                conn.execute("DELETE FROM organizations WHERE id = ?", (org_id,))
        except Exception:
            pass
        return False, res
    user_id = res
    with _conexion() as conn:
        conn.execute("UPDATE usuarios SET org_id = ? WHERE id = ?", (org_id, user_id))
        conn.execute("UPDATE organizations SET owner_user_id = ? WHERE id = ?", (user_id, org_id))
    org = obtener_organizacion(org_id)
    return True, {"org_id": org_id, "user_id": user_id,
                  "token": org["token"], "nombre_org": org_nombre}


# ---------------------------------------------------------------------------
# Invitaciones de músicos (por correo)
# ---------------------------------------------------------------------------

def crear_invitacion(org_id, email, dias=7):
    """Crea una invitación de músico para un email. Devuelve (ok, token | error)."""
    email = (email or "").strip().lower()
    if not email or "@" not in email or "." not in email:
        return False, "Email inválido"
    if buscar_por_email(email):
        return False, "Ya existe una cuenta con ese email"
    token = secrets.token_urlsafe(24)
    expira = (datetime.utcnow() + timedelta(days=int(dias))).isoformat(timespec="seconds")
    try:
        with _conexion() as conn:
            conn.execute(
                "UPDATE invitaciones SET estado='cancelada' WHERE org_id=? AND email=? AND estado='pendiente'",
                (org_id, email),
            )
            conn.execute(
                "INSERT INTO invitaciones (org_id, email, token, estado, creado_en, expira_en) "
                "VALUES (?, ?, ?, 'pendiente', ?, ?)",
                (org_id, email, token, _ahora_iso(), expira),
            )
        return True, token
    except Exception as e:
        return False, f"Error: {e}"


def obtener_invitacion(token):
    """Devuelve la invitación pendiente y vigente por token, o None."""
    if not token:
        return None
    with _conexion() as conn:
        row = conn.execute(
            "SELECT * FROM invitaciones WHERE token = ? AND estado = 'pendiente'", (token,)
        ).fetchone()
    if not row:
        return None
    inv = dict(row)
    if inv.get("expira_en"):
        try:
            if datetime.utcnow() > datetime.fromisoformat(inv["expira_en"]):
                return None
        except Exception:
            pass
    return inv


def marcar_invitacion_usada(token):
    with _conexion() as conn:
        conn.execute("UPDATE invitaciones SET estado = 'usada' WHERE token = ?", (token,))


def listar_invitaciones(org_id, estado="pendiente"):
    with _conexion() as conn:
        rows = conn.execute(
            "SELECT * FROM invitaciones WHERE org_id = ? AND estado = ? ORDER BY creado_en DESC",
            (org_id, estado),
        ).fetchall()
        return [dict(r) for r in rows]


def eliminar_invitacion(inv_id, org_id):
    with _conexion() as conn:
        cur = conn.execute("DELETE FROM invitaciones WHERE id = ? AND org_id = ?", (inv_id, org_id))
        return cur.rowcount > 0


if __name__ == "__main__":
    init_db()
    print(f"✓ Base de datos inicializada en {ARCHIVO_DB}")

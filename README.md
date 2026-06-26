# MI Worship - Charts (VPS)

Sistema web para que los músicos de Mi Iglesia Internacional accedan a los charts (cifrados con acordes) de canciones de adoración. Cuentas individuales con email y contraseña, múltiples setlists, transposición inteligente y conexión al sistema local del culto en vivo.

**Producción:** https://miworship.miiglesiainternacional.org

---

## Características

### Para los músicos
- **Registro propio** con nombre, apellido, email y contraseña
- **Login individual** con email + contraseña
- **Recuperación de contraseña** vía código enviado por email
- **Tabs Live / Biblioteca** con buscador instantáneo por título y artista
- **Múltiples setlists** con nombre y fecha (ej: "Domingo mañana", "Jueves jóvenes")
- **Selector de setlist** cuando hay varios disponibles, default al más próximo
- **Tonos del setlist** aplicados automáticamente al abrir cada canción
- **Transposición libre** (+/- semitonos) desde la biblioteca, no afecta setlist
- **Vista limpia** del chart con acordes, secciones y timeline navegable
- **Tamaño de letra ajustable** (3 tamaños)
- **Saludo personalizado** "Bienvenido, [Nombre]" en la página principal
- **Banner "EN VIVO"** cuando el culto está activo, con redirección al sistema local

### Para el administrador
- Subida de canciones JSON
- **Gestión de usuarios:**
  - Aprobar / rechazar registros pendientes
  - Enviar código de reset manualmente
  - Eliminar usuarios
  - Crear admins adicionales
  - Protección anti-bloqueo (no se puede borrar el último admin)
- **Creación de múltiples setlists** con nombre y fecha personalizables
- **Transposición por canción** dentro del setlist con botones +/-
- Edición de nombre/fecha de cada setlist
- Eliminación automática de setlists con fecha pasada
- **Acceso de emergencia** con password compartido si pierde su cuenta
- Estado del live (conexión con Mac local)

### Para el live (culto)
- Banner "🔴 EN VIVO" cuando el sistema local (Mac con `puente.py`) está activo
- Redirige a los músicos al sistema local de la iglesia
- Heartbeat automático cada 30s desde el Mac

---

## Stack técnico

- **Backend:** Flask + Gunicorn
- **Reverse proxy:** Nginx
- **HTTPS:** Let's Encrypt
- **Hosting:** DigitalOcean (1 vCPU, 1GB RAM)
- **Service manager:** systemd
- **Persistencia:**
  - `config.json` para passwords compartidos, setlists, estado del live
  - `usuarios.db` (SQLite) para cuentas de usuario y códigos de reset
  - `secrets.json` para credenciales SMTP (no incluido en git)
- **Email:** SMTP de Hostinger desde `noreply@miiglesiainternacional.org`

## Estructura del proyecto

```
charts-vps/
├── servidor_vps.py        # Servidor Flask principal
├── transposicion.py       # Motor de transposición (bemoles/sostenidos smart)
├── usuarios.py            # Gestión de usuarios (SQLite + helpers)
├── emails.py              # Envío de emails vía SMTP Hostinger (HTML templates)
├── templates/
│   ├── login_musicos.html # Login con email + password
│   ├── login_admin.html   # Login admin (con fallback de emergencia)
│   ├── registro.html      # Registro de nuevos usuarios
│   ├── olvide_password.html # Solicitar código de reset
│   ├── reset_password.html  # Ingresar código + nueva contraseña
│   ├── principal.html     # Vista de los músicos
│   ├── visor.html         # Vista de canción
│   └── admin.html         # Panel de administración
├── static/
│   └── logo.png
├── canciones/             # JSONs de canciones (NO en git)
├── config.json            # Config global (NO en git)
├── usuarios.db            # SQLite con usuarios (NO en git)
├── secrets.json           # Credenciales SMTP (NO en git)
└── requirements.txt
```

## Configuración

### `config.json`
Se crea automáticamente al primer arranque:

```json
{
  "password_musicos": "<hash>",
  "password_admin": "<hash>",
  "secret_key": "<random>",
  "live_token": "<random>",
  "setlists": [
    {
      "id": "<random>",
      "nombre": "Domingo mañana",
      "fecha": "2026-06-28",
      "canciones": [{"id": 1, "tono": "Bb"}],
      "creado": "2026-06-25T10:30:00"
    }
  ],
  "live_activo": false,
  "mac_local_ip": null,
  "ultimo_heartbeat": null
}
```

### `secrets.json` (NO en git)
Credenciales SMTP para envío de emails:

```json
{
  "smtp_host": "smtp.hostinger.com",
  "smtp_port": 465,
  "smtp_user": "noreply@miiglesiainternacional.org",
  "smtp_password": "<password>"
}
```

### Base de datos de usuarios

SQLite (`usuarios.db`) con 2 tablas:

```sql
usuarios (id, nombre, apellido, email UNIQUE, password_hash, rol, estado, creado_en, aprobado_en)
reset_codigos (id, user_id, codigo, expira_en, usado)
```

- **Roles:** `musico` | `admin`
- **Estados:** `pendiente` | `activo` | `rechazado`
- **Códigos de reset:** 6 dígitos, expiran en 15 minutos

## Flujo de registro

```
Usuario nuevo
    │
    ▼
Crea cuenta en /registro  ──►  Estado: pendiente
    │
    ▼
Admin entra al panel       ──►  Aprueba o rechaza
    │
    ▼
Si aprueba: estado = activo + email automático de bienvenida
    │
    ▼
Usuario puede ingresar en /login con email + password
```

## Flujo de recuperación de contraseña

```
Usuario olvidó password
    │
    ▼
Click "¿Olvidaste tu contraseña?"  ──►  Ingresa email
    │
    ▼
Sistema genera código de 6 dígitos y lo envía por email
    │
    ▼ (respuesta inmediata, email en background)
Usuario va a /reset_password con email + código
    │
    ▼
Ingresa nueva contraseña → cambio confirmado
```

**Nota técnica:** El envío SMTP tarda ~30s debido a rate limiting de DigitalOcean en puerto saliente. Por eso se manda en thread background para no bloquear la UI.

## Formato de canciones JSON

```json
{
  "numero": 1,
  "titulo": "Tus Cuerdas de Amor",
  "artista": "Lowsan Melgar",
  "tono": "F",
  "tempo": 67,
  "compas": "4/4",
  "secciones": [
    {
      "tipo": "intro",
      "nombre": "Intro",
      "prog": ["Dm", "Bb", "F", "Dm", "Bb", "F"]
    },
    {
      "tipo": "verso",
      "nombre": "Verso 1",
      "lines": [
        [["F", "Soy tuyo y tú eres mía"]]
      ]
    }
  ]
}
```

## Deploy

```bash
# En local
git add . && git commit -m "..." && git push origin main

# En el VPS
ssh charts@64.227.10.28
cd ~/charts_app && git pull
sudo systemctl restart charts
```

## Integración con sistema local

Este VPS funciona como complemento del [sistema local Playback-Secuencias](https://github.com/ulises02jr/Playback-Secuencias) que corre en el Mac de la iglesia durante el culto.

## Endpoints

### Públicos
- `GET /login` - Login músicos (email + password)
- `POST /login` - Procesar login
- `GET /registro` - Pantalla de registro
- `POST /registro` - Crear cuenta (queda pendiente)
- `GET /olvide_password` - Solicitar código de reset
- `POST /olvide_password` - Enviar código por email
- `GET /reset_password` - Ingresar código + nueva password
- `POST /reset_password` - Cambiar contraseña
- `GET /admin/login` - Login admin (acepta password de emergencia con email vacío)
- `GET /api/live_status` - Estado del live (público)

### Músicos (requieren rol musico)
- `GET /` - Página principal con tabs Live/Biblioteca
- `GET /cancion/<n>` - Ver canción (acepta `?setlist_id=X&t=N&from=bib`)

### Admin (requieren rol admin)
- `GET /admin` - Panel completo
- `POST /admin/subir` - Subir JSON de canción
- `POST /admin/eliminar/<n>` - Eliminar canción
- **Usuarios:**
  - `POST /admin/usuario/<id>/aprobar` - Aprobar usuario pendiente
  - `POST /admin/usuario/<id>/eliminar` - Eliminar usuario
  - `POST /admin/usuario/<id>/reset_password` - Mandar código de reset manual
  - `POST /admin/crear_admin` - Crear nuevo admin
- **Setlists:**
  - `POST /admin/setlist/crear` - Crear setlist
  - `POST /admin/setlist/<id>/editar` - Editar nombre/fecha
  - `POST /admin/setlist/<id>/eliminar` - Eliminar setlist
  - `POST /admin/setlist/<id>/agregar` - Agregar canción
  - `POST /admin/setlist/<id>/quitar/<n>` - Quitar canción
  - `POST /admin/setlist/<id>/mover` - Reordenar
  - `POST /admin/setlist/<id>/transponer/<n>` - Subir/bajar 1 semitono
  - `POST /admin/setlist/<id>/resetear/<n>` - Volver al tono original
- **Otros:**
  - `POST /admin/cambiar_password` - Cambiar contraseña de emergencia
  - `POST /admin/live_off` - Forzar apagado del live

### Live (desde Mac local)
- `POST /api/live_ping` - Heartbeat (requiere token)

## Motor de transposición

El módulo `transposicion.py` decide automáticamente entre **bemoles y sostenidos** según el tono destino:

- **Sostenidos** para: G, D, A, E, B, F#, C# (y sus menores)
- **Bemoles** para: F, Bb, Eb, Ab, Db, Gb, C

Maneja correctamente:
- Acordes con modificadores: `m`, `m7`, `sus4`, `maj7`, `dim`, etc.
- Acordes con bajo: `C/E`, `D/F#`
- Strings con múltiples acordes y espacios

## Sistema de emails

`emails.py` envía emails HTML profesionales con la identidad visual de la iglesia:

- **Código de reset:** Diseño oscuro con código gigante en dorado
- **Bienvenida:** Cuando el admin aprueba una cuenta, link directo al login

**Modo background:** Los envíos se hacen en threads separados para no bloquear la UI cuando hay rate limiting del proveedor.

## Seguridad

- **Hashes:** SHA-256 con salt único por usuario
- **Sesiones:** Flask sessions con secret_key aleatorio
- **HTTPS:** Forzado en producción vía Let's Encrypt
- **Tokens de reset:** 6 dígitos numéricos, expiran en 15 minutos, se invalidan al usar
- **Códigos viejos:** Se invalidan al generar uno nuevo
- **Protección anti-bloqueo:** No se puede eliminar el último admin activo
- **Password de emergencia:** Disponible si el admin pierde su cuenta personal
- **secrets.json:** Excluido de git, permisos 600

## Licencia

Uso privado de Mi Iglesia Internacional.

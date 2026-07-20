# Servidor MI Worship (Flask)

Servidor web que corre en el VPS. Maneja la biblioteca de canciones, los setlists,
los usuarios, la generación de tonos/letras y la API que consumen NeuralPlay,
NeuralWorship y la web del músico.

## Archivos

| Archivo | Qué hace |
|---|---|
| `servidor_vps.py` | App Flask principal: rutas, biblioteca, setlists, API. |
| `transposicion.py` | Transposición de acordes y tonos. |
| `usuarios.py` | Registro, login, aprobación y hashing de usuarios. |
| `emails.py` | Envío de correos (bienvenida, reset de password). |
| `templates/` | Vistas HTML (admin, visor, principal, login, etc.). |
| `static/` | CSS/JS (incluye `ensayo.js`/`ensayo.css` del reproductor de ensayo) y logo. |
| `requirements.txt` | Dependencias Python. |

## Datos de runtime (NO versionados)

Estos existen en el servidor pero **nunca** se suben a Git (ver `.gitignore`):

- `config.json` — configuración (setlists, org, live_token, IP local). **Secreto.**
- `secrets.json` — llaves de la app. **Secreto.**
- `usuarios.db` — base de datos de usuarios. **Secreto.**
- `pistas/` — audio (stems) y caché de tonos renderizados.
- `canciones/*.json` — charts de la biblioteca (datos).
- `static/portadas/` — portadas subidas.
- `descargas/`, `backups_canciones/` — artefactos de runtime.

Para configurar un servidor nuevo, copiá `config.example.json` → `config.json`
y `secrets.example.json` → `secrets.json` y llená los valores.

## Infraestructura

- **Dominio:** miworship.miiglesiainternacional.org
- **Servicio:** `charts.service` (systemd) → gunicorn en `127.0.0.1:5051` (2 workers)
- **Frente:** Nginx + Let's Encrypt (HTTPS). DNS en Cloudflare.
- **Python:** venv en el servidor (`venv/`, no versionado).

## API para las apps (con token)

Todas bajo `/api/live/*`, autenticadas con `Authorization: Bearer <live_token>`
(o `?token=`). Ver `docs/AUDIT.md` para el inventario completo de rutas.

| Ruta | Devuelve |
|---|---|
| `GET /api/live/setlists` | Repertorios + índice de canciones. |
| `GET /api/live/pistas/<n>` | Stems + tiempos de sección de una canción a un tono. |
| `GET /api/live/pista/<n>/<archivo>` | Descarga de un stem. |
| `GET /api/live/midi/<n>` | Cajas MIDI + notas (en segundos). |
| `GET /api/live/chart/<n>` | Chart (secciones con acordes+letra) para los músicos. |
| `POST /api/live_ping` | Heartbeat del puente (enciende el live). |
| `GET /api/live_status` | Estado del live (público). |

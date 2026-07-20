# Auditoría del servidor — MI Worship

Revisión del servidor Flask (`server/servidor_vps.py` y módulos) con foco en
seguridad, orden de rutas, lógica y calidad, pensando en un producto de nivel
industrial. Fecha: 2026-07-20.

## Resumen ejecutivo

**Estado general: sólido.** El servidor está bien organizado, sin secretos versionados,
con autenticación por rol y por token, protección contra path traversal y hashing
moderno de contraseñas. Se listan abajo hallazgos menores y recomendaciones para
endurecerlo de cara a la venta.

## Seguridad ✅

- **Sin secretos en el repo ni en la historia.** `.gitignore` excluye `config.json`,
  `secrets.json`, `usuarios.db`, `pistas/`, `*.bak`, `__pycache__`. Verificado con
  `git ls-files` y `git log` sobre esos paths: cero coincidencias.
- **Contraseñas:** hashing pbkdf2 (migrable), rate-limit de login, CSRF con token,
  hardening de cookies (commits recientes de seguridad).
- **Descarga de stems:** valida path traversal con `os.path.commonpath` antes de
  `send_file` (bien).
- **API de apps:** protegida por token (`_token_ok()` compara contra `live_token`).
- **`/api/live_status`** es público a propósito (solo informa si hay live + IP local).

## Rutas — inventario (61 total, sin duplicados)

Agrupadas por nivel de acceso:

- **Público (6):** `/login`, `/registro`, `/admin/login`, `/olvide_password`,
  `/reset_password`, `/logout`.
- **Músico / sesión (6):** `/`, `/cancion/<n>`, `/api/sync/biblioteca`,
  `/api/cancion/<n>`, `/api/version`, `/api/pistas/<n>` (+ render/estado, servir_pista).
- **API live / token (7):** `/api/live/setlists`, `/api/live/pistas/<n>`,
  `/api/live/pista/<n>/<archivo>`, `/api/live/midi/<n>`, `/api/live/chart/<n>`,
  `POST /api/live_ping`, `GET /api/live_status`.
- **Admin / sesión (~40):** gestión de biblioteca, setlists, pistas, secciones,
  MIDI, letras, cifrado, usuarios, organización y perfil.

## Calidad del código

| Métrica | Resultado |
|---|---|
| Rutas duplicadas | 0 |
| `except:` desnudos | 0 (todos son `except Exception` o específicos) |
| `except Exception` | 37 |
| `print(...)` | 8 (van al journal de systemd) |
| `TODO/FIXME/HACK` | 4 |

## Hallazgos y recomendaciones (priorizadas)

### Media
1. **Un solo archivo de ~90 KB / 61 rutas.** Funciona, pero para escalar de forma
   industrial conviene separar por **Flask Blueprints** (auth, admin, api_live,
   pistas). Mejora mantenibilidad y pruebas.
2. **Archivos `.bak` en el directorio de producción.** Hay copias `servidor_vps.py.bak.*`
   en la carpeta viva. Están ignoradas por git (bien), pero conviene moverlas a una
   carpeta `backups/` fuera del árbol servido para no ensuciar producción.
3. **Rate-limiting en la API con token.** El login ya tiene rate-limit; agregar un
   límite básico a `/api/live/*` y `/api/live_ping` reduce abuso si el token se filtra.

### Baja
4. **`except Exception` amplios (37).** Revisar que ninguno silencie errores que
   deberían registrarse; loguear con contexto donde aplique.
5. **`print()` → logging.** Migrar los 8 `print` a `logging` con niveles (info/warning/
   error) para mejor observabilidad en producción.
6. **Resolver los 4 TODO/FIXME** pendientes en el código.
7. **Endpoint de salud** (`GET /healthz`) para monitoreo externo (uptime) sin sesión.
8. **Pruebas automatizadas** mínimas para las rutas críticas (login, API live, setlists).

### Observación (no es problema)
- El chart de los músicos se sirve en el **tono base** de la canción. Si se usan tonos
  transpuestos por setlist, el chart del músico no se transpone aún. Mejora futura.

## Conclusión

Nada crítico. La base es limpia y segura. Las recomendaciones son de robustez y
mantenibilidad para el crecimiento comercial: Blueprints, logging, rate-limit de la
API y pruebas.

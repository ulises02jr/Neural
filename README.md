# MI Worship - Charts (VPS)

Sistema web para que los músicos de Mi Iglesia Internacional accedan a los charts (cifrados con acordes) de canciones de adoración. Modo ensayo accesible desde cualquier dispositivo, con soporte de múltiples setlists y transposición inteligente.

**Producción:** https://miworship.miiglesiainternacional.org

---

## Características

### Para los músicos
- **Tabs Live / Biblioteca** con buscador instantáneo por título y artista
- **Múltiples setlists** con nombre y fecha (ej: "Domingo mañana", "Jueves jóvenes")
- **Selector de setlist** cuando hay varios disponibles
- **Tonos del setlist** aplicados automáticamente al abrir cada canción
- **Transposición libre** (+/- semitonos) desde la biblioteca (no afecta setlist)
- **Vista limpia** del chart con acordes, secciones y timeline navegable
- **Tamaño de letra ajustable** (3 tamaños)

### Para el administrador
- Subida de canciones JSON
- **Creación de múltiples setlists** con nombre y fecha personalizables
- **Transposición por canción** dentro del setlist con botones +/-
- Edición de nombre/fecha de cada setlist
- Eliminación automática de setlists con fecha pasada
- Cambio de passwords (músicos y admin)
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
- **Persistencia:** archivos JSON (sin base de datos)

## Estructura del proyecto

```
charts-vps/
├── servidor_vps.py        # Servidor Flask principal
├── transposicion.py       # Motor de transposición (bemoles/sostenidos smart)
├── templates/
│   ├── login_musicos.html
│   ├── login_admin.html
│   ├── principal.html     # Vista de los músicos (tabs + setlists)
│   ├── visor.html         # Vista de canción
│   └── admin.html         # Panel de administración
├── static/
│   └── logo.png
├── canciones/             # JSONs de canciones (NO en git)
├── config.json            # Config + passwords + setlists (NO en git)
└── requirements.txt
```

## Configuración

El archivo `config.json` se crea automáticamente al primer arranque con passwords por defecto que deben cambiarse desde el panel admin.

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
      "canciones": [
        {"id": 1, "tono": "Bb"}
      ],
      "creado": "2026-06-25T10:30:00"
    }
  ],
  "live_activo": false,
  "mac_local_ip": null,
  "ultimo_heartbeat": null
}
```

## Formato de canciones JSON

Cada canción es un archivo `cancion_NNN_titulo_TONO.json` con esta estructura:

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

Este VPS funciona como complemento del [sistema local Playback-Secuencias](https://github.com/ulises02jr/Playback-Secuencias) que corre en el Mac de la iglesia durante el culto:

1. El Mac arranca `puente.py` (sistema MIDI live con Logic Pro)
2. `puente.py` envía heartbeat cada 30s al VPS (`POST /api/live_ping`)
3. El VPS muestra banner "EN VIVO" a los músicos
4. Los músicos hacen click → redirige al Mac local (red WiFi de la iglesia)

## Endpoints principales

### Públicos
- `GET /login` - Login músicos
- `GET /admin/login` - Login admin
- `GET /api/live_status` - Estado del live (público)

### Músicos
- `GET /` - Página principal con tabs Live/Biblioteca
- `GET /cancion/<n>` - Ver canción (acepta `?setlist_id=X&t=N&from=bib`)

### Admin
- `GET /admin` - Panel completo
- `POST /admin/subir` - Subir JSON de canción
- `POST /admin/eliminar/<n>` - Eliminar canción
- `POST /admin/setlist/crear` - Crear setlist
- `POST /admin/setlist/<id>/editar` - Editar nombre/fecha
- `POST /admin/setlist/<id>/eliminar` - Eliminar setlist
- `POST /admin/setlist/<id>/agregar` - Agregar canción
- `POST /admin/setlist/<id>/quitar/<n>` - Quitar canción
- `POST /admin/setlist/<id>/mover` - Reordenar
- `POST /admin/setlist/<id>/transponer/<n>` - Subir/bajar 1 semitono
- `POST /admin/setlist/<id>/resetear/<n>` - Volver al tono original
- `POST /admin/cambiar_password` - Cambiar contraseña
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

## Licencia

Uso privado de Mi Iglesia Internacional.

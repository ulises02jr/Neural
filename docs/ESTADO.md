# Estado del proyecto — Neural (memoria)

> Última actualización: 2026-08-04. Snapshot de dónde está el ecosistema, qué se hizo y qué sigue.

## Ecosistema (3 apps)
- **NeuralPlay** — reproductor multipista nativo macOS (C++/JUCE 8, un solo `Main.cpp` ~6.8k líneas). App en desarrollo activo. Proyecto **CMake**, target `NeuralPlaySpike` (PRODUCT_NAME `NeuralPlay`).
- **Server** — Flask en VPS `root@64.227.10.28`, en `/home/charts/charts_app` (usuario `charts`), gunicorn `127.0.0.1:5051`, nginx + Let's Encrypt. Panel admin + charts + API `/api/live/...`.
- **NeuralSync** — app puente (Python/rumps) para músicos que tocan desde su propio DAW; sincroniza la sesión en vivo con los charts.

Dominio: `miworship.miiglesiainternacional.org`. Repo: `github.com/ulises02jr/Neural` (monorepo: `NeuralPlay/`, `NeuralSync/`, `server/`, `docs/`).

## Git / despliegue (IMPORTANTE)
- El **Mac** commitea local pero **NO tiene push** (llave sin acceso). Código autoritativo de **NeuralPlay** = el del Mac.
- El **VPS** (`/home/charts/Neural`, dueño **root**) es el que **pushea** a GitHub con el alias SSH `github-charts`. Código autoritativo del **server** = `charts_app` (producción).
- Flujo del server: `respaldar.sh` hace rsync `charts_app -> server/` (excluye datos/secretos/`pistas`/`pads`/portadas), luego `git add/commit/push`.
- Flujo de NeuralPlay: `scp Main.cpp` del Mac al VPS repo, y commit/push desde el VPS.
- Nunca subir: `pistas/`, `pads/`, `static/portadas`, `usuarios.db`, `secrets.json`, `config.json`.
- Nota: Mac y VPS tienen historiales divergentes (mismo contenido, distinto hash). Alinear con pull cuando convenga.

## NeuralPlay — funciones ya hechas
Motor de audio: mezcla por-track con ganancias atómicas, ruteo por familia (hasta 32 salidas), grilla de beats (`currentBeatGrid`), tempo dinámico por medley (`currentBpm()`), MIDI out por cajas, sección/mapa de canciones, descarga en 2 fases con barra por track.

1. **Pre-roll por sección** (menú): al dar Play al inicio de una sección, retrocede 1 compás y sube los faders hasta el downbeat (Click/Guía a nivel). `storage.json`.
2. **Puntos de inicio/fin por canción** (in/out): min:seg opcional en la ventana de tono. `inout.json`.
3. **Sección de click al final**: 2 compases DESPUÉS del final con **metrónomo sintetizado** al Click; loop ∞, `+`/`-` en Editar. `clicksec.json`.
4. **Mapping de teclado**: barra en Editar; teclas a transporte, mute/solo de tracks y buses, bloques de canción. `keymap.json`.
5+6. **MIDI IN + MIDI learn**: botón "MIDI Mapping"; aprende acciones + **faders continuos** por CC (incluye master y el fader del Pad). `midimap.json`.
- **Tope de faders** a 0 dB. Menú **Master/Buses por canción** (mute general por FAMILIA). **Aviso de cambios sin guardar** (punto rojo en Repertorios).

## Guardado / sincronización de mezcla
- La mezcla por canción vive **en memoria**; se persiste al servidor **SOLO al tocar "Guardar"** en Repertorios. Opciones del menú autoguardan local en `storage.json`. Otro músico ve los cambios solo tras Guardar.

## Pendiente conocido (bug menor)
- **Snap del playhead a la barra de click**: implementado pero no funciona; pausado.

## Roadmap y plataformas

### Plataformas por app (DECISIÓN)
Se elige por **criticidad de timing**, no por abarcar (como Playback de MultiTracks / Prime de Loop Community, Apple-only a propósito).
- **NeuralCharts** → **TODAS** (iPhone, iPad, macOS, Windows, Android) vía **PWA** (solo visor + ensayo, no crítico → barato y de alcance enorme).
- **NeuralPlay** → **Mac + iPad** (motor en vivo, timing crítico).
- **NeuralSync** → **Mac + Windows** (favor al músico que produce en su DAW sobre PC).
- Costos (verificado 2026-08): Apple Developer $99/año; Google Play $25 único; JUCE Personal gratis / Indie ~$40/año al vender; firma Windows opcional ~$100-400/año. El costo real es TIEMPO + hardware de prueba.

### Hecho
1-6 (arriba) + **#7 Pads ambientales** (servidor + NeuralPlay, completo — ver abajo). También: 32 salidas + Dante.

### Bloque a terminar (orden por esfuerzo)
- **NeuralCharts como PWA** (todas las plataformas) — barato, alcance enorme.
- **NeuralSync para Windows** (cambiar rumps por lib multiplataforma + PyInstaller).
- **SMPTE / LTC** (arrancar por MTC vía MIDI).
- **NeuralPlay para iPad** (rediseño touch).

### Después (deferido)
Transiciones entre canciones (motor nuevo; diseño en `docs/DISENO_16_17.md`); editar arreglo + reordenar secciones (no-lineal, mismo motor); control remoto a distancia; editar notas/cues MIDI; automatización de faders; crear/compartir arreglos + notificaciones; asientos/licencias; redundancia PlayAUDIO 12.

## Pads ambientales — feature completa (2026-08-04)

### Servidor (panel admin — EN PRODUCCIÓN y respaldado en el repo)
- Biblioteca GLOBAL de pads por tono. Se sube UN pad base y el servidor genera los 12 tonos por pitch-shift (ffmpeg + rubberband), cacheados en `pads/<id>/pad_<idx>.wav` (~14 MB c/u, ~170 MB por pad). `pads/packs.json` indexa {id, nombre, artista, base_root, base_idx, ext, portada, portada_ts}.
- Acceso: **Biblioteca** tiene pestañas **Canciones / Pads**; botón "＋ Nuevo Pad (Ambiente)". Ventana **"Ambient Pads"**: crear (nombre, artista, tono base, portada, audio base) con **barra de subida** (XHR + token `_csrf`), lista con progreso de generación, **editar** (icono 3 puntos → nombre/artista/tono/portada/reemplazar audio) y **pre-escucha por tono** (cifrado americano C/C#/D…).
- Rutas admin: `/admin/pads` (+ `crear`, `<id>/eliminar`, `<id>/regenerar`, `<id>/estado`, `<id>/editar`, `<id>/audio/<idx>`). API NeuralPlay: `/api/live/pads` (incluye `portada` y `base_idx`) y `/api/live/pad/<id>/<idx>` (auth por token). Portadas en `static/portadas/pad_<id>.<ext>`.
- `respaldar.sh` excluye `pads/`; `server/.gitignore` incluye `pads/`. Portadas también fuera del repo.

### NeuralPlay (Mac, `Main.cpp`)
- **Motor de audio del pad**: voz en loop (`AudioFormatReaderSource` looping + Buffering + Resampling) con crossfade ~3s; se mezcla SIEMPRE (aunque no haya canción o esté en pausa), a canales 0/1, `padGain` (fader con suavizado por muestra = anti-zipper) × master + soft-clip; lock aparte (`padLock`).
- **Estado global** en `storage.json`: `padPack`, `padGainDb`, `padMode` (Auto/Manual), `padManual`. Catálogo desde `/api/live/pads` al arrancar; descarga/caché de tonos bajo demanda + precarga de los 12 con progreso.
- **Botón cabecera "PAD"**: toggle; en Auto sigue la raíz del tono de la canción y se re-afina al cambiar.
- **Vista de faders del Pad (faderView 3)**: barra de vidrio con portada (redondeada, dentro) + nombre → tap abre menú de pads del Admin (marca los no descargados); almohada **"Link"** (Auto) + **12 tonos** grandes; indicador **"Descargando N/12" / "12 tonos listos"**; **fader propio** (asignable por MIDI, `midimap.json` clave `padfader`).
- **Pad Player por canción** (edición de la canción, junto a inicio/fin; `padplayer.json`): **"Pad al iniciar"** (entra ~3s y baja ~3s; si ya sonaba, solo baja) y **"Pad al finalizar"** (entra en los **últimos 3 compases** — con tempo/compás — y se **sostiene con latch** aunque la canción vuelva al inicio automáticamente). Solo dispara **reproduciendo**; volver a darle **Play** (flanco) corta el outro y ejecuta el intro; el **toggle manual** libera la automatización y ésta **se re-arma** al salir de la zona.
- **Build**: `cmake --build build --target NeuralPlaySpike` (cmake vía pip). Artefacto: `build/NeuralPlaySpike_artefacts/Release/NeuralPlay.app`.

## Negocio
Se ofrece como servicio (suscripción a iglesias, mercado worship en español). El valor real está en usuarios + ingresos + marca, no en el código (replicable). Meta: volverse el "Playback del mundo hispano de alabanza".

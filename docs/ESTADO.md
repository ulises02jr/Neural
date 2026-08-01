# Estado del proyecto — Neural (memoria)

> Última actualización: 2026-08-01. Snapshot de dónde está el ecosistema, qué se hizo y qué sigue.

## Ecosistema (3 apps)
- **NeuralPlay** — reproductor multipista nativo macOS (C++/JUCE 8, un solo `Main.cpp` ~5.9k líneas). App en desarrollo activo.
- **Server** — Flask en VPS `root@64.227.10.28`, en `/home/charts/charts_app` (usuario `charts`), gunicorn `127.0.0.1:5051`, nginx + Let's Encrypt. Panel admin + charts + API `/api/live/...`.
- **NeuralSync** — app puente (Python/rumps) para músicos que tocan desde su propio DAW; sincroniza la sesión en vivo con los charts.

Dominio: `miworship.miiglesiainternacional.org`. Repo: `github.com/ulises02jr/Neural` (monorepo: `NeuralPlay/`, `NeuralSync/`, `server/`, `docs/`).

## Git / despliegue (IMPORTANTE)
- El **Mac** commitea local pero **NO tiene push** (llave sin acceso).
- El **VPS** (`/home/charts/Neural`, dueño **root**) es el que **pushea** a GitHub con el alias SSH `github-charts` (llave de deploy en root).
- Flujo para subir cambios del Mac: `scp` del archivo al VPS -> en el VPS (como root) `git add/commit/push`.
- `respaldar.sh` (en `/home/charts/Neural`, corre como root) respalda SOLO el código del server (`charts_app` -> `server/`), nunca datos ni secretos.
- Nunca subir: `pistas/`, `usuarios.db`, `secrets.json`, `config.json`.
- Nota: Mac y VPS tienen historiales divergentes (mismo contenido, distinto hash). Alinear con pull cuando convenga.

## NeuralPlay — funciones ya hechas
Motor de audio: mezcla por-track con ganancias atómicas, ruteo por familia (hasta 32 salidas), grilla de beats (`currentBeatGrid`), tempo dinámico por medley (`currentBpm()`), MIDI out por cajas, sección/mapa de canciones, descarga en 2 fases con barra por track.

Bloque de mejoras 2026-08-01 (commit `bef500a` en GitHub / `4619586` en Mac):
1. **Pre-roll por sección** (menú, opcional): al dar Play justo al inicio de una sección, retrocede 1 compás y sube los faders hasta el downbeat. Click y Guía se quedan a nivel para oír el conteo. Persistente (`storage.json`).
2. **Puntos de inicio/fin por canción** (in/out): en la ventana de tono, min:seg opcional. Persistente (`inout.json`).
3. **Sección de click al final**: bloque de 2 compases DESPUÉS del final, con **metrónomo sintetizado** ruteado a la salida del Click (el click grabado suele acabar antes). Loop infinito reactivable sin saltos, ticks por beat, `+`/`-` en modo Editar. Sin loop, al terminar vuelve al inicio. Persistente (`clicksec.json`).
4. **Mapping de teclado**: barra desplegable en Editar (entre mapa y faders). Asigna teclas a transporte, Pad/Buses/MIDI/faders, **mute y solo de tracks y de buses**, y bloques de canción. Re-teclar desasigna, sin duplicados, badges translúcidos con la tecla en la esquina. Persistente (`keymap.json`).
- **Tope de faders** en el volumen original (0 dB, sin boost) en tracks, buses y master.
- Menú **"Master por canción"** y **"Buses y mute por canción"**: independiente por canción o general para todo el setlist. El mute general es por **FAMILIA (bus)** (estable entre canciones) con regla de dos capas (canal suelto vs familia bus-bloqueada). Persistente (`storage.json`).
- **Aviso de cambios sin guardar**: puntito rojo en el botón Repertorios + "Cambios sin guardar" en el picker; se apaga al Guardar; no se activa con cambios programáticos ni con el Fade.

## Guardado / sincronización de mezcla
- La mezcla por canción (faders, mutes, solos, buses, master) vive **en memoria** durante la sesión (se recuerda al cambiar de canción). Se persiste **al servidor SOLO al tocar "Guardar"** en Repertorios (`saveRepertoireMixes` -> `/api/live/setlist/<id>/mix`).
- Las opciones del menú (pre-roll, master/buses generales, mezcla general) **autoguardan localmente** en `storage.json` (preferencias del dispositivo, no viajan a la nube).
- Otro músico de la organización ve los cambios de mezcla **solo después de que alguien dio Guardar**.

## Pendiente conocido (bug menor)
- **Snap del playhead a la barra de click** al soltar el arrastre en el mapa: implementado pero no funciona en la práctica; quedó pausado para retomar después.

## Roadmap (19 ítems, orden de dificultad)
Hechos: **1** (pre-roll), **2** (in/out), **3** (click final), **4** (mapping teclado), **5+6** (MIDI IN + MIDI learn). También (de sesiones previas): 32 salidas y Dante (vía Dante Virtual Soundcard).

MIDI (5+6) — detalle: entrada MIDI abierta a todos los puertos (`MidiInputCallback`), botón "MIDI Mapping" en la barra de Editar junto a "Mapping de teclado" (conviven). Aprende las mismas acciones que el teclado (transporte, mute/solo de tracks y buses, canciones, Pad/Buses/MIDI/faders) por Note o CC-botón, MÁS **faders continuos** por CC (perilla/fader del controlador -> dB, click en el fader en pantalla para armarlo). Re-aprender desasigna, sin duplicados. Persistente en `midimap.json`. Play/Inicio/Fade se habilitan en modo mapping para poder armarlos.

Pendientes, del más fácil:
7. Pads ambientales (loop por tono + crossfade) — alto valor musical. **<- siguiente recomendado**.
8. NeuralCharts (empaquetar panel del músico como app).
9. SMPTE/LTC (arrancar por MTC vía MIDI).
10. Control remoto a distancia de NeuralPlay (server embebido).
11. NeuralPlay para Windows.
12. Editar notas/cues MIDI (mini piano-roll).
13. Automatización de faders (se cuelga del motor de 16/17).
14. Crear/compartir arreglos + notificaciones.
15. Asientos para músicos (licencias/cuentas).
16. Transiciones entre canciones (crossfade/gapless/auto-advance) — motor nuevo.
17. Editar arreglo + reordenar secciones (no-lineal) — mismo motor que 16.
18. NeuralPlay para iPad/iPhone.
19. Redundancia con PlayAUDIO 12 (sincronía real entre dos máquinas).

Notas de arquitectura: 16+17 (+13) son el mismo motor multi-región con curvas — diseñarlo una vez. 5+6 van pegados. 8+14 comparten infra de cuentas/notificaciones.

## Negocio
Se ofrece como servicio (suscripción a iglesias, mercado worship en español). El valor real está en usuarios + ingresos + marca, no en el código (replicable). Meta: volverse el "Playback del mundo hispano de alabanza".

# NeuralWorship (el puente)

App de escritorio que conecta un **DAW** (Logic, etc.) con los músicos. Es la
alternativa a NeuralPlay para equipos que prefieren manejar la reproducción desde
su propio DAW en vez del reproductor integrado.

## Cómo funciona (misma lógica que el puente de NeuralPlay)

1. El DAW manda MIDI por un bus **IAC** (canal 16 por defecto): una nota por sección,
   donde `nota = 36 + índice_de_sección` (`NOTA_BASE_SECCION = 36`).
2. NeuralWorship escucha ese MIDI, actualiza la sección actual y **transmite el estado
   a los músicos** por WebSocket, sirviendo la página en el puerto **5050**.
3. Manda el heartbeat al VPS (`/api/live_ping`) para encender el "live", y los músicos
   entran a `http://<ip-de-la-mac>:5050`.

Config típica (en `~/Library/Application Support/NeuralWorship/config.json`, no versionado):

```json
{
  "vps_url": "https://miworship.miiglesiainternacional.org",
  "token": "<live_token>",
  "midi_channel": 16,
  "iac_port": "Driver IAC To Secuencias Live",
  "puerto": 5050
}
```

## ⏳ Código pendiente de importar

El código fuente de esta app está hoy en el repo **`Logic-Secuencias`** y en el disco
`Software_Live`. **Falta migrarlo a esta carpeta.** Para hacerlo se necesita una de:

- Un ZIP del código fuente, **o**
- Montar el disco Crucial X10 (`/MI Worship/Software_Live`), **o**
- Una deploy key agregada al repo `Logic-Secuencias` (la llave del VPS solo alcanza `Mi-Worship`).

> Requisitos: `flask`, `flask-sock`, `mido`, `python-rtmidi`, `certifi`, `rumps`.

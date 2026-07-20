# NeuralPlay

Reproductor multitrack en vivo para macOS (JUCE / C++). Es la app "todo integrado":
baja el repertorio del servidor, reproduce los stems, mezcla, muestra el mapping de
la canción, dispara MIDI en sincronía y hace de **puente** para los músicos — sin
depender de un DAW externo.

## Estructura

```
NeuralPlay/
├── Source/
│   └── Main.cpp        Toda la app (UI, audio, MIDI, servidor de músicos).
├── CMakeLists.txt      Build (JUCE como subdirectorio).
├── AppIcon.png         Ícono.
└── README.md
```

> `JUCE/`, `build/` y `stems/` **no** se versionan (ver `.gitignore`).

## Compilar

Requiere CMake y JUCE 8. Desde la carpeta `NeuralPlay/`:

```bash
git clone https://github.com/juce-framework/JUCE.git   # una sola vez
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

El `.app` queda en `build/NeuralPlaySpike_artefacts/Release/NeuralPlay.app`.

## Configuración

La app lee `serverUrl` y `token` de su `config.json` en
`~/Library/Application Support/NeuralPlay/config.json` (no versionado).

## Funciones principales

- **Repertorio** desde el servidor (setlists, stems, portadas, tiempos de sección).
- **Reproductor multitrack** con mezclador (pistas, buses por familia, master), solos,
  fades y mapping con forma de onda.
- **MIDI**: cajas de notas por canción, salida por puerto/canal configurable,
  marcadores de color en el mapping; motor de disparo de baja latencia.
- **Sincronizar** (menú ☰): levanta un servidor local en `:5050` + heartbeat al VPS.
  Los músicos entran a `http://<ip-de-la-mac>:5050` y su chart se mueve solo,
  **sección por sección**, según avanza la canción.

## Cómo funciona el sync de músicos

NeuralPlay sirve tres endpoints HTTP en `:5050`:

- `GET /` → la página del músico (chart con acordes+letra).
- `GET /song` → el chart de la canción actual (lo baja de `/api/live/chart/<n>`).
- `GET /state` → `{ idx (sección), ver (versión de canción), playing }`.

La página hace *polling* a `/state` cada 400 ms y hace auto-scroll a la sección activa.
NeuralPlay calcula la sección desde el playhead (60 Hz) y la publica en variables
atómicas, para nunca trabar el audio ni la interfaz.

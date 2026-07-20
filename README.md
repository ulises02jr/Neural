# MI Worship

Ecosistema de **MI Worship** — plataforma de multitracks, charts (acordes + letra),
setlists y reproducción en vivo para el ministerio de medios de Mi Iglesia Internacional.

Este repositorio es el **monorepo** de todo el ecosistema. Cada pieza vive en su
propia carpeta, con su propio código y documentación.

## Estructura

```
Mi-Worship/
├── server/          Servidor web (Flask) — corre en el VPS
│                    Biblioteca, setlists, usuarios, API para las apps y la web del músico.
│
├── NeuralPlay/      App de escritorio (macOS · JUCE/C++) — el REPRODUCTOR
│                    Multitrack en vivo, mezclador, mapping, MIDI y puente de
│                    sincronización integrado. Todo en una sola app.
│
├── NeuralWorship/   App de escritorio — el PUENTE (para quien usa su propio DAW)
│                    Recibe MIDI del DAW (Logic, etc.) y sincroniza a los músicos.
│
├── docs/            Documentación transversal (arquitectura, auditoría, deploy).
└── README.md        Este archivo.
```

## Cómo encaja todo (visión de 30 segundos)

1. El **servidor** guarda la biblioteca (canciones, secciones, tiempos, MIDI, portadas)
   y los setlists. Expone una API con token para las apps y una web para los músicos.
2. **NeuralPlay** baja el repertorio del servidor, reproduce los multitracks en vivo y,
   al activar *Sincronizar*, levanta un servidor local (puerto 5050) al que los músicos
   se conectan: el chart de cada músico se mueve solo, sección por sección, según va la canción.
3. **NeuralWorship** hace lo mismo que el puente de NeuralPlay, pero para equipos que
   prefieren manejar la reproducción desde su propio DAW: el DAW manda MIDI (bus IAC,
   canal 16, una nota por sección) y NeuralWorship transmite la sección a los músicos.

La lógica de sincronización es **por sección** (no por milisegundo): liviana y estable en red.

## Respaldo (importante)

El servidor de producción se **edita en el VPS**; GitHub se usa como **respaldo**.
Para respaldar, ver [`docs/DEPLOY.md`](docs/DEPLOY.md) y el script `respaldar.sh`.

> ⚠️ Nunca se versionan: `secrets.json`, `config.json`, `usuarios.db`, `pistas/`,
> ni datos de runtime. Ver `.gitignore`.

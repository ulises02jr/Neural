# Diseño — #16 Transiciones + #17 Arreglo no-lineal (motor de reproducción)

> Documento de arquitectura (en papel, sin código todavía). Objetivo: dejar claro QUÉ construimos,
> CÓMO se conecta con el motor actual **sin romperlo**, y un plan por fases con victorias verificables.

## 1. Qué queremos lograr
- **#16 Transiciones entre canciones:** que una canción pase a la siguiente **sin cortar** — con *crossfade* (se cruzan volúmenes), *gapless* (pegada, sin silencio) o *auto-advance* (al terminar, entra sola la siguiente).
- **#17 Arreglo no-lineal:** reproducir las secciones en un **orden distinto al del archivo** (ej. Intro → V1 → Coro → V1 otra vez → Puente → Coro), **saltar**, **repetir**, **eliminar** o **duplicar** secciones, y **guardar** ese arreglo. Todo alineado al beat para que no brinque feo.

Ambos comparten el mismo motor nuevo. El **#13 (automatización de faders)** se cuelga de aquí también.

## 2. El motor de hoy (punto de partida)
En `getNextAudioBlock` hay **una sola voz implícita**: un arreglo de `resamplers` (los stems de la canción actual), un `positionOut` (posición en samples), y se mezcla track por track con `busGain`, luego `masterGain` + `softClip`. `loadSong()` limpia la voz anterior y arma la nueva → por eso al cambiar de canción hay **cortecito** y **no hay solape**. Las **secciones** (`sectionTimes`/`sectionNames`) son solo **marcas** sobre una línea recta de 0 al final; la grilla de beats (`currentBeatGrid`) ya nos da los puntos para alinear saltos.

**Sirve perfecto para reproducción lineal de una canción.** Lo que falta son dos capacidades: (a) tocar regiones en cualquier orden, (b) tener dos canciones sonando a la vez un instante para el crossfade.

## 3. Conceptos e ideas nuevas (modelo de datos)
- **Región (`Region`):** un tramo reproducible = `{ canción, inicioSample, finSample, (tempo/tono heredados) }`. Una sección del arreglo es una región. La canción entera, tal cual hoy, es "una región que va de 0 al final".
- **Arreglo (`Arrangement`):** **lista ordenada de regiones** de una canción. Reproducirlo = tocar región tras región en ese orden (esto ES el #17). Si el arreglo no existe, se usa el arreglo trivial `[canción entera]` → comportamiento idéntico al de hoy.
- **Voz (`PlaybackVoice`):** un conjunto de stems + posición + **envolvente de volumen** (fade in/out). Hoy hay **1 voz**; para crossfade entre canciones necesitamos **2 voces** (A y B) que puedan sonar a la vez unos segundos.
- **Transición (`Transition`):** entre dos canciones del setlist = `{ tipo: corte | gapless | crossfade, duración (segundos o compases), curva }`.
- **PlaybackEngine:** capa que mezcla las voces activas (máx. 2) con su envolvente y entrega al master. `getNextAudioBlock` pasa a sumar voces en vez de una sola.

## 4. Audio: cómo suena
- **No-lineal dentro de una canción (barato):** los stems ya están cargados; saltar de una sección a otra es solo **mover el read position** de los `resamplers` (como un `seek`), **alineado al downbeat** para que empate. Cero solape, cero costo extra. Esto cubre la mayor parte del #17.
- **Crossfade entre canciones (costoso):** la **voz B** (siguiente canción) se **precarga** (stems listos, `resamplers` preparados) **antes** del punto de transición; durante la transición ambas voces suenan y sus envolventes se cruzan (A baja, B sube) en la duración elegida. Al terminar, se libera la voz A. Como son canciones distintas (otro tempo/tono), **no se sincroniza tempo** — solo se cruzan volúmenes (que es como lo hace Playback/Ableton en un crossfade simple).
- **Gapless / auto-advance:** caso especial del anterior con duración 0 o muy corta; la clave es la **precarga a tiempo** para que no haya silencio.

## 5. Convivencia: no romper lo que ya sirve
- El **modo lineal de hoy es el default**. Si una canción no tiene arreglo ni transición definida, el motor nuevo se comporta **exactamente igual** (una región = canción entera, una voz).
- Se implementa como **capa encima**, no como reemplazo. Bandera interna para activar el camino nuevo.
- **Respaldos y verificación** en cada fase: la reproducción normal de tus servicios no se puede degradar. Cada fase se prueba en vivo antes de seguir.

## 6. Persistencia (JSON, por canción / repertorio)
- `arrangement` por canción: `[{ secciónId/inicio, fin, repeticiones }...]` → el orden no-lineal.
- `transitions` entre canciones del setlist: `{ deId, aId, tipo, duración, curva }`.
- Se guardan junto a la mezcla (mismo flujo de "Guardar" al servidor) para que viajen al equipo.

## 7. UI (pantallas nuevas)
- **Editor de arreglo (#17):** sobre el mapa/secciones — arrastrar bloques para **reordenar**, botones **duplicar / eliminar**, marcar **repetir/loop**. Vista previa del orden resultante.
- **Transiciones (#16):** entre tarjetas de canción del setlist, un control de **tipo + duración** (y opción "auto-advance" global).

## 8. Riesgos y mitigaciones
- **CPU/RAM:** 2 voces = doble decodificación de stems un instante. Mitigar: la voz B solo vive durante la transición; precargar con margen; limitar a 2 voces.
- **Glitch en el punto de cruce:** aplicar envolventes suaves (equal-power crossfade) y alinear saltos al buffer/beat.
- **Precarga a destiempo (silencio):** disparar la precarga de B con anticipación configurable.
- **Romper lo actual:** fases pequeñas + verificación en vivo + respaldo antes de cada fase.

## 9. Plan por fases (cada una deja algo verificable)
- **Fase 0 — Refactor invisible:** introducir el concepto "voz = región" con una sola región (= canción entera). **Nada cambia** para el usuario; solo preparamos el terreno. *Verificable: todo suena igual que hoy.*
- **Fase 1 — No-lineal en UNA canción (#17 núcleo):** saltos gapless entre secciones + reordenar/repetir, reusando los stems cargados. Bajo riesgo, alto valor. *Verificable: tocás V1 → Coro → V1 sin cortes.*
- **Fase 2 — Segunda voz + auto-advance/gapless entre canciones (#16 base):** precarga de la siguiente + pegada sin silencio. *Verificable: al terminar una canción entra sola la siguiente, sin hueco.*
- **Fase 3 — Crossfade configurable + editor de arreglo (UI de #16 y #17) + persistencia.** *Verificable: crossfade de 4s entre dos canciones; arreglo guardado que viaja al equipo.*
- **Fase 4 — #13 Automatización de faders:** curvas de volumen por región, aplicadas sin clicks. Se apoya en el motor ya hecho.

## 10. Dónde toca el código actual (mapa de impacto)
- `getNextAudioBlock` → sumar voces con envolvente (hoy una sola).
- `resamplers` / `loadSong` / `clearSong` → pasar a "cargar/preparar una voz"; permitir 2 voces.
- `positionOut` / `seekTo` → por voz; saltos alineados a `currentBeatGrid`.
- `sectionTimes` / `sectionNames` → base de las regiones del arreglo.
- Mapa (`drawMap`) y tarjetas de canción → UI de arreglo y transiciones.

## 11. Decisiones de producto (para Andrés, antes de codear)
1. **Transiciones entre canciones:** ¿automáticas (auto-advance de todo el setlist) o manuales (vos disparás el pase)?
2. **Duración del crossfade:** ¿por **tiempo** (segundos) o por **compases**?
3. **Arreglo no-lineal:** ¿se **guarda** un orden fijo por canción, o preferís un **modo en vivo** donde saltás secciones a mano (con los botones/MIDI que ya mapeamos)? ¿O ambos?
4. **Alcance de la Fase 1:** ¿arrancamos solo con **saltar/repetir secciones en vivo** (lo más útil y barato) y dejamos el editor con guardado para la Fase 3?

## 12. Tamaño estimado (referencia honesta)
- Fase 0–1: **medio** (motor interno + saltos). 1–2 sesiones.
- Fase 2: **medio-alto** (segunda voz + precarga). 1–2 sesiones.
- Fase 3: **alto** (UI editor + crossfade + persistencia). 2–3 sesiones.
- Fase 4 (#13): **medio**. 1–2 sesiones.
Total realista: **un proyecto de varias sesiones**, pero cada fase deja algo usable y no rompe lo anterior.

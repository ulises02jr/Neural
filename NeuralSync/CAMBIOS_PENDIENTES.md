# NeuralWorship — Cambios del panel

> El usuario final es una iglesia; el lenguaje debe ser simple, sin jerga técnica.

## HECHOS (commit af09b01)

- [x] **1.** Quitar la jerga "VPS": la etiqueta ahora dice **"Conexión"**.
- [x] **2.** MIDI sin "Logic": la etiqueta ahora dice solo **"MIDI"** (genérico para
  cualquier programa: Logic, Reaper, Cubase, etc.).
- [x] **3.** Quitado el contador **"Biblioteca: N"**.
- [x] **4.** El repertorio ahora se muestra como **lista de canciones** (títulos +
  tono) del setlist activo que trae del VPS, con la que va sonando resaltada.

## PENDIENTE

### 5. Acceso de músicos por la web pública, NO por IP  [NECESITA DISEÑO]
- Hoy el panel muestra una **URL con IP local** (http://192.168.x.x:5050) + QR.
- Se quiere que los músicos vayan a **miworship.miiglesiainternacional.org** y
  ahí vean el banner **"EN VIVO"** que los lleve a la vista en vivo.
- IMPLICACIÓN (no es solo texto): para que la sincronización de secciones por MIDI
  funcione en la web pública, el app local tendría que **relevar el estado en vivo
  al VPS**, y el VPS servir la vista sincronizada. Es una **función nueva**
  (relay en la nube). Hay que diseñarla.
- Dos caminos: (A) relevo en la nube — mejor experiencia, cuidar la latencia del
  internet del salón; (B) híbrido — sync local rápido + acceso amigable.
- El **escaneo (QR)** se conserva, apuntando a la web pública.

> Estado: en análisis (el usuario lo sigue pensando).

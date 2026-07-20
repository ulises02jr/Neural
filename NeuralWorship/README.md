# NeuralWorship

**El puente que conecta el sistema de secuencias (Logic/DAW por MIDI) con los
músicos**, que ven los acordes y letras en sus celulares — cambiando de sección
en vivo, en tiempo real.

La app es un **puente de comunicación**: no guarda nada ni administra contenido.
Los charts y repertorios los provee el **VPS** (`miworship.miiglesiainternacional.org`);
la música la maneja el DAW por MIDI; y NeuralWorship traduce eso para los músicos.

---

## Cómo funciona

```
   DAW (Logic / Reaper / …)                 VPS (la nube)
     │  MIDI canal 16                          │  charts + repertorios
     ▼                                         ▼
   NeuralWorship  ──────────────────────────────
     │  sirve el visor por Wi-Fi (LAN)
     ▼
   Celulares de los músicos  (escanean el QR)
```

- **Program Change** = cargar canción N · **notas en escalera desde C1 (36)** =
  secciones · **C0 (24)** = fin.
- La app baja la biblioteca y el repertorio activo del VPS (HTTPS, token por header).
- Sirve el visor a los músicos en la red local y los sincroniza por WebSocket.

---

## Estructura

```
App_Puente/
├── puente_core.py          Motor (Flask + MIDI + WebSocket). Multiplataforma.
├── app.py                  Cara de macOS (barra de menú + ventana WKWebView)
├── app_windows.py          Cara de Windows (bandeja + ventana pywebview)
├── transposicion.py        Motor de acordes
├── templates/              visor.html (músicos) · panel.html (dashboard)
├── static/                 logo y assets del visor
├── setup.py                Empaquetado macOS (py2app → .app)
├── NeuralWorship.spec      Empaquetado Windows (PyInstaller → .exe)
├── build_windows.bat       Script de build de Windows
├── NeuralWorship.iss       Instalador de Windows (Inno Setup → Setup.exe)
├── icono.icns / icono.ico  Íconos
├── config.example.json     Plantilla de config (Mac)
├── config.windows.example.json  Plantilla de config (Windows)
├── requirements-windows.txt
├── README.md               (este archivo)
└── README_WINDOWS.md       Guía detallada de Windows (loopMIDI/rtpMIDI, build)
```

> El código completo vive en GitHub (repo **Logic-Secuencias**). No se necesita
> ningún disco externo: se clona y se trabaja desde cualquier Mac.

---

## Preparar en una máquina nueva

```bash
git clone git@github-playback:ulises02jr/Logic-Secuencias.git
cd Logic-Secuencias/App_Puente
```

Traé el `config.json` (con el token) desde la **carpeta privada del VPS**
(solo por SSH, no está en la web ni en el repo):

```bash
scp root@64.227.10.28:/home/charts/secretos/neuralworship-config.json config.json
```

> Alternativa manual: `cp config.example.json config.json` y pegar el token a mano.

`config.json` **no se versiona** (contiene el token). El token es el mismo
`live_token` del VPS; la copia lista para bajar vive en
`/home/charts/secretos/` (permisos `700`, solo accesible por SSH). La config del usuario (canal, puerto MIDI) se guarda aparte en el sistema:
- macOS: `~/Library/Application Support/NeuralWorship/`
- Windows: `%APPDATA%\NeuralWorship\`

---

## Correr en desarrollo

**macOS:**
```bash
pip3 install rumps pyobjc-framework-WebKit flask flask-sock mido python-rtmidi certifi qrcode
python3 app.py
```

**Windows:**
```powershell
pip install -r requirements-windows.txt
python app_windows.py
```

Se abre la ventana con el panel (estado del VPS, QR, canal y fuente MIDI). Los
músicos entran por la URL/QR que muestra el panel, en la misma Wi-Fi.

---

## Compilar

### macOS (.app)
py2app no firma bien en discos exFAT; construí en el disco interno (APFS):
```bash
cd App_Puente
python3 setup.py py2app --dist-dir="$HOME/NeuralWorship_dist" --bdist-base="$HOME/NeuralWorship_build"
# resultado: ~/NeuralWorship_dist/NeuralWorship.app
```

### Windows (.exe + instalador)
Ver **README_WINDOWS.md** (requiere una PC Windows: loopMIDI/rtpMIDI, PyInstaller,
Inno Setup). Resumen: `pip install -r requirements-windows.txt` → `build_windows.bat`
→ compilar `NeuralWorship.iss` con Inno Setup.

---

## Publicar los instaladores (para descarga en el admin del VPS)

En el admin, sección **Software Live**, hay botones de descarga Mac/Windows.
Se activan al subir el archivo correspondiente al VPS:

```bash
# Mac
scp NeuralWorship.zip root@64.227.10.28:/home/charts/charts_app/descargas/NeuralWorship-Mac.zip
# Windows
scp NeuralWorship-Setup.exe root@64.227.10.28:/home/charts/charts_app/descargas/NeuralWorship-Windows-Setup.exe
# y darles dueño charts:
ssh root@64.227.10.28 'chown charts:charts /home/charts/charts_app/descargas/*'
```

---

## Seguridad

- La app solo hace **llamadas salientes** al VPS (no abre puertos al internet).
- El servidor local sirve el **visor** a la LAN; el **panel** y los controles
  responden **solo a la propia máquina** (127.0.0.1).
- El token viaja por header `Authorization: Bearer …` (no en la URL) y **no** se
  versiona en git.

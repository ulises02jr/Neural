# NeuralWorship — versión Windows

El **motor es el mismo** que en Mac (`puente_core.py`). Lo único distinto es la
"cara" (`app_windows.py`, con ventana WebView2 + ícono en la bandeja) y el
empaquetado (**PyInstaller** en vez de py2app).

En Windows no existe el IAC del Mac; el puerto MIDI virtual se crea con **loopMIDI**.

---

## 1. Requisitos en la PC Windows

- Windows 10 o 11
- **Python 3.11+** (marcá "Add Python to PATH" al instalar)
- **WebView2 Runtime** (viene en Windows 11; en Windows 10 se instala gratis desde Microsoft)
- **loopMIDI** (Tobias Erichsen) — para el puerto MIDI virtual

## 2. Fuente MIDI: loopMIDI o rtpMIDI (se elige EN la app)

La app necesita recibir el MIDI del secuenciador. Según el montaje:

- **Mismo PC** (el DAW y NeuralWorship en la misma máquina): instalá **loopMIDI**
  y creá un puerto con el botón `+`. En el DAW, mandá la pista de secuencias a ese
  puerto, en **canal 16**.
- **Por red** (el secuenciador va en otra máquina, p. ej. el Mac con Logic):
  instalá **rtpMIDI** en la PC Windows y conectá la sesión; aparece un puerto de red.

En cualquiera de los dos casos **NO se edita ningún archivo**: abrí el panel de
NeuralWorship y en **"Fuente MIDI"** elegí el puerto de la lista. La app lo recuerda
y reconecta sola.

> Idioma MIDI (igual que en Mac): Program Change = número de canción,
> notas en escalera desde C1 = secciones, C0 = fin. Canal 16 por defecto.

## 3. Instalar dependencias

Abrí **PowerShell** dentro de la carpeta `App_Puente` y corré:

```powershell
pip install -r requirements-windows.txt
```

## 4. Configurar el token

```powershell
copy config.windows.example.json config.json
```

Abrí `config.json` y pegá el **token del VPS** en `"token"`. El puerto MIDI ya no
se configura acá: se elige desde el panel de la app (ver paso 2).

## 5. Probar sin empaquetar (modo desarrollo)

```powershell
python app_windows.py
```

Debería abrir la **ventana del panel** (estado del VPS, QR, canal) y aparecer el
**ícono en la bandeja** (abajo a la derecha). Los músicos entran por la URL/QR que
muestra el panel, en la misma red Wi-Fi.

## 6. Empaquetar el .exe

```powershell
.\build_windows.bat
```

Queda en `dist\NeuralWorship\NeuralWorship.exe`. Podés comprimir la carpeta
`dist\NeuralWorship` y entregarla, o armar un instalador con **Inno Setup**
(opcional, se ve más pro: un solo `Setup.exe` que instala en Archivos de programa
y crea acceso directo).

---

## Qué necesita SÍ o SÍ una máquina Windows

Todo el **código ya está listo** desde acá. Lo que no se puede hacer/probar en la
Mac y hay que hacerlo en una PC Windows real (o una máquina virtual con Windows):

1. **Instalar y correr loopMIDI** — es una app solo de Windows.
2. **Construir el `.exe`** — PyInstaller NO hace "cross-compile"; el ejecutable de
   Windows se genera corriendo PyInstaller *en* Windows.
3. **Probar la ventana (WebView2) y la bandeja (pystray)** — usan APIs de Windows.
4. **Probar el MIDI real** (python-rtmidi sobre WinMM) con loopMIDI + el DAW.

## Nota sobre la advertencia de Windows (SmartScreen)

Igual que en Mac, un `.exe` sin firmar muestra "Windows protegió tu PC" →
**Más información → Ejecutar de todas formas**. Para quitar esa advertencia se
necesita un certificado de *code signing* de Windows (aparte del de Apple; distinto
proveedor y costo). Opcional, solo si lo vas a vender masivamente.

---

## 7. (Opcional) Instalador `Setup.exe` con Inno Setup

Para entregar un solo instalador (en vez de una carpeta comprimida):

1. Instalá **Inno Setup 6** (gratis): https://jrsoftware.org/isdl.php
2. Primero corré `build_windows.bat` (debe existir la carpeta `dist\NeuralWorship`).
3. Doble clic a **`NeuralWorship.iss`** → se abre Inno Setup → botón **Compile**
   (o clic derecho al `.iss` → *Compile*).
4. Queda en `installer\NeuralWorship-Setup.exe`.

Ese `Setup.exe` instala en *Archivos de programa*, crea acceso directo en el menú
Inicio y (opcional) en el Escritorio, y agrega el desinstalador. La config del
usuario se guarda aparte en `%APPDATA%\NeuralWorship`, así que desinstalar no borra
el token ni las preferencias.

> Nota: el `Setup.exe` también saldrá sin firmar (SmartScreen → "Más información →
> Ejecutar de todas formas") hasta que consigas un certificado de code signing de
> Windows. Es opcional.

---

## Resumen: qué está 100% listo y qué te toca a vos

**Listo desde ya (en el repo):** motor multiplataforma, `app_windows.py`,
`NeuralWorship.spec`, `build_windows.bat`, `NeuralWorship.iss`,
`config.windows.example.json`, `requirements-windows.txt`, `icono.ico` y este README.

**En la PC Windows (una sola vez):**
1. Instalar Python 3.11+, WebView2 Runtime, loopMIDI e Inno Setup.
2. Instalar la fuente MIDI: loopMIDI (mismo PC) o rtpMIDI (por red).
3. `pip install -r requirements-windows.txt`
4. `copy config.windows.example.json config.json` y pegar el token.
5. `build_windows.bat`  → genera `dist\NeuralWorship\NeuralWorship.exe`
6. Compilar `NeuralWorship.iss` con Inno Setup → `installer\NeuralWorship-Setup.exe`

---

## WebView2 automático en el instalador

El `NeuralWorship.iss` ya instala **WebView2 solo si falta** (la iglesia no hace nada).
Para eso, UNA vez, descargá el bootstrapper oficial de Microsoft y ponelo así:

```
App_Puente\redist\MicrosoftEdgeWebview2Setup.exe
```

Descarga (gratis, ~2 MB, redistribuible):
https://developer.microsoft.com/microsoft-edge/webview2/  → "Evergreen Bootstrapper".

Con eso, el `Setup.exe` detecta si la PC ya tiene WebView2; si no, lo instala en
silencio antes de abrir la app. En Windows 11 casi siempre ya viene incluido.

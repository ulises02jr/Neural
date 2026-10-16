# NeuralPlay — Build en Windows

Estado: **Fase 1** (arranque con WASAPI). El código ya es multiplataforma
(todos los `#include` de Apple están bajo guards, el keychain y la IP local
tienen respaldo). No hay que reescribir nada: solo compilar en una máquina
Windows con las herramientas de abajo.

## 1. Requisitos (una sola vez)

- **Visual Studio 2022** (Community sirve) con el workload
  **"Desarrollo para el escritorio con C++"** (incluye el compilador MSVC).
- **CMake** 3.22 o superior — https://cmake.org/download (marcar "Add to PATH").
- **Git** — https://git-scm.com

## 2. Traer el código (con JUCE)

El proyecto usa JUCE como subcarpeta. Si clonás el repo:

```bat
git clone <URL-del-repo> NeuralPlay
cd NeuralPlay
git submodule update --init --recursive
```

> Si copiás la carpeta a mano desde el Mac, asegurate de incluir la carpeta
> `JUCE/` completa.

## 3. Compilar (Fase 1 — WASAPI)

Desde la carpeta del proyecto, en el "Developer Command Prompt for VS 2022":

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

El ejecutable queda en:

```
build\NeuralPlaySpike_artefacts\Release\NeuralPlay.exe
```

Doble clic y debería abrir. El audio sale por **WASAPI** (la tarjeta de
sonido de Windows). Con esto validamos que la app corre en Windows.

## 4. Fase 2 — ASIO (32 salidas con interfaz de audio pro)

Para el enrutamiento multisalida real hace falta **ASIO**:

1. Descargar el **ASIO SDK** (gratis) de Steinberg:
   https://www.steinberg.net/developers/  -> descomprimir, por ej. en
   `C:\asiosdk`.
2. Reconfigurar activando ASIO:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DNP_ENABLE_ASIO=ON -DASIO_SDK_DIR=C:\asiosdk
cmake --build build --config Release
```

> El ASIO SDK NO se versiona en el repo (licencia de Steinberg). Cada quien
> lo descarga en su maquina.

## Estado por fase

- **Fase 1 (WASAPI):** listo. Compila y arranca con el audio de Windows.
- **Fase 2 (ASIO + keychain):**
  - **ASIO:** andamiaje listo en el CMake; se activa con las banderas de
    arriba una vez descargado el SDK de Steinberg.
  - **Guardado de contraseña:** IMPLEMENTADO con **Windows Credential
    Manager** (`np_keychain.cpp`, rama `_WIN32`). Guarda por usuario, persiste
    entre reinicios, no requiere admin. Se enlaza `Advapi32` automaticamente.

## Pendiente (Fase 3)

- **Instalador + firma de codigo:** por ahora es un `.exe` suelto. Fase 3:
  crear instalador y firmarlo para que Windows no lo marque como
  "editor desconocido".

## Notas

- No se modifico nada del build de macOS/iOS: siguen igual.
- El bloque de Windows en `CMakeLists.txt` solo se activa cuando el sistema
  es Windows (`if (WIN32)`), asi que no afecta las otras plataformas.

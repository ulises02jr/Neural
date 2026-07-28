# -*- coding: utf-8 -*-
"""
NeuralWorship — App de escritorio (Windows)
===========================================
Misma idea que la de Mac, pero con piezas de Windows:
  - Ventana nativa con el panel/dashboard  (pywebview → Edge WebView2)
  - Ícono en la bandeja del sistema         (pystray)

El MOTOR es el mismo archivo (puente_core.py). Lo único que cambia es esta
"cara" y el empaquetado (PyInstaller en vez de py2app).

MIDI en Windows: no hay IAC. Se usa loopMIDI (puerto virtual). El nombre del
puerto se pone en config.json -> "iac_port" (ver config.windows.example.json).

Correr (con Python instalado):   python app_windows.py
Empaquetar:                      ver build_windows.bat  /  NeuralWorship.spec
"""
import sys
import threading
import webbrowser
from pathlib import Path

import puente_core as core

if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    BASE = Path(sys._MEIPASS)              # recursos dentro del .exe (PyInstaller)
else:
    BASE = Path(__file__).resolve().parent


def arrancar_motor():
    """Levanta MIDI + heartbeat + servidor web en hilos. Devuelve la IP local."""
    try:
        ip = core.arrancar_servicios()
    except Exception as e:
        print(f"Error arrancando servicios: {e}")
        ip = "127.0.0.1"
    puerto = int(core.CONFIG.get("puerto", 5050))
    threading.Thread(
        target=lambda: core.app.run(host="0.0.0.0", port=puerto,
                                    debug=False, use_reloader=False),
        daemon=True).start()
    return ip, puerto


def iniciar_bandeja(window, ip, puerto):
    """Ícono en la bandeja del sistema (opcional; si falla, la app sigue con la ventana)."""
    try:
        import pystray
        from PIL import Image
    except Exception as e:
        print(f"Bandeja no disponible ({e}); solo ventana.")
        return None

    ruta_icono = BASE / "icono_1024.png"
    try:
        imagen = Image.open(str(ruta_icono))
    except Exception:
        imagen = Image.new("RGBA", (64, 64), (91, 140, 255, 255))

    panel_url = f"http://127.0.0.1:{puerto}/panel"

    def abrir_panel(icon, item):
        try:
            window.show()
        except Exception:
            webbrowser.open(panel_url)

    def abrir_visor(icon, item):
        webbrowser.open(f"http://{ip}:{puerto}")

    def salir(icon, item):
        try:
            core.enviar_heartbeat("bye")
        except Exception:
            pass
        try:
            icon.stop()
        except Exception:
            pass
        try:
            window.destroy()
        except Exception:
            pass
        import os
        os._exit(0)

    menu = pystray.Menu(
        pystray.MenuItem("Abrir panel", abrir_panel, default=True),
        pystray.MenuItem("Abrir visor (músicos)", abrir_visor),
        pystray.MenuItem("Salir", salir),
    )
    icono = pystray.Icon("NeuralWorship", imagen, "NeuralWorship", menu)
    try:
        icono.run_detached()   # corre en su propio hilo (soportado en Windows)
    except Exception as e:
        print(f"No se pudo iniciar la bandeja en segundo plano ({e}).")
    return icono


def main():
    ip, puerto = arrancar_motor()
    panel_url = f"http://127.0.0.1:{puerto}/panel"

    import webview  # pywebview
    window = webview.create_window(
        "NeuralWorship",
        panel_url,
        width=960,
        height=680,
        min_size=(780, 580),
    )

    iniciar_bandeja(window, ip, puerto)

    # Bloquea en el hilo principal con el loop de la GUI (Edge WebView2 en Windows)
    webview.start()


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""
MI Worship — Puente (App de escritorio, macOS)
==============================================
App presentable con:
  - Ventana nativa (WKWebView) con el panel/dashboard  →  /panel
  - Ícono en la barra de menú (rumps) para tenerla siempre a mano

La ventana muestra: estado del VPS (EN VIVO), MIDI (escuchando), músicos
conectados, la URL + QR para los músicos, y el selector de canal MIDI.

Correr:   python3 app.py
Empaquetar:  python3 setup.py py2app
"""
import threading
import webbrowser
from pathlib import Path

try:
    import rumps
except ImportError:
    print("Falta rumps:  pip3 install rumps")
    raise SystemExit(1)

# PyObjC (ventana nativa)
from AppKit import (NSWindow, NSBackingStoreBuffered, NSApp,
                    NSWindowStyleMaskTitled, NSWindowStyleMaskClosable,
                    NSWindowStyleMaskMiniaturizable, NSWindowStyleMaskResizable,
                    NSViewWidthSizable, NSViewHeightSizable,
                    NSApplicationActivationPolicyRegular)
from WebKit import WKWebView, WKWebViewConfiguration
from Foundation import NSURL, NSURLRequest, NSObject

import puente_core as core

BASE = Path(__file__).resolve().parent
ICONO_BAR = str(BASE / "icono_bar.png")

_delegates = []   # retiene los delegados de ventana (evita que el GC los recoja)


class _CerrarAlSalir(NSObject):
    """Cierra la app por completo al cerrar la ventana (como NeuralPlay)."""
    def windowWillClose_(self, _notif):
        try:
            core.enviar_heartbeat("bye")
        except Exception:
            pass
        rumps.quit_application()


def crear_ventana(url):
    rect = ((0.0, 0.0), (940.0, 660.0))
    estilo = (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
    win = NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(
        rect, estilo, NSBackingStoreBuffered, False)
    win.setTitle_("NeuralSync")
    win.setReleasedWhenClosed_(False)
    win.setMinSize_((780.0, 580.0))
    _d = _CerrarAlSalir.alloc().init()   # la X cierra la app por completo
    win.setDelegate_(_d)
    _delegates.append(_d)

    cfg = WKWebViewConfiguration.alloc().init()
    web = WKWebView.alloc().initWithFrame_configuration_(rect, cfg)
    web.setAutoresizingMask_(NSViewWidthSizable | NSViewHeightSizable)
    win.setContentView_(web)

    req = NSURLRequest.requestWithURL_(NSURL.URLWithString_(url))
    web.loadRequest_(req)
    win.center()
    return win, web


class PuenteApp(rumps.App):
    def __init__(self):
        try:
            super().__init__("NeuralSync", icon=ICONO_BAR, template=True, quit_button=None)
        except Exception:
            super().__init__("NeuralSync", quit_button=None)
        self.puerto = int(core.CONFIG.get("puerto", 5050))
        self.ip = "127.0.0.1"
        self.win = None
        self.web = None

        self.menu = [
            rumps.MenuItem("Abrir panel", callback=self.cb_panel),
            rumps.MenuItem("Abrir visor (músicos)", callback=self.cb_visor),
            None,
            rumps.MenuItem("Salir", callback=self.cb_salir),
        ]

        self._arrancar()
        # Abre el panel automáticamente cuando Flask ya levantó
        self._t = rumps.Timer(self._abrir_inicial, 2)
        self._t.start()

    # ── Arranque del motor + servidor web ──
    def _arrancar(self):
        try:
            self.ip = core.arrancar_servicios()
        except Exception as e:
            print(f"Error arrancando servicios: {e}")
        threading.Thread(
            target=lambda: core.app.run(host="0.0.0.0", port=self.puerto,
                                        debug=False, use_reloader=False),
            daemon=True).start()

    def _abrir_inicial(self, _):
        try:
            self._t.stop()
        except Exception:
            pass
        try:
            NSApp().setActivationPolicy_(NSApplicationActivationPolicyRegular)   # ícono en el Dock + menú junto a la manzana
        except Exception:
            pass
        self.abrir_panel()

    # ── Ventana del panel ──
    def abrir_panel(self):
        url = f"http://127.0.0.1:{self.puerto}/panel"
        try:
            if self.win is None:
                self.win, self.web = crear_ventana(url)
            self.win.makeKeyAndOrderFront_(None)
            NSApp().activateIgnoringOtherApps_(True)
        except Exception as e:
            print(f"No se pudo abrir la ventana: {e}")
            webbrowser.open(url)

    # ── Callbacks ──
    def cb_panel(self, _):
        self.abrir_panel()

    def cb_visor(self, _):
        webbrowser.open(f"http://{self.ip}:{self.puerto}")

    def cb_salir(self, _):
        try:
            core.enviar_heartbeat("bye")
        except Exception:
            pass
        rumps.quit_application()


if __name__ == "__main__":
    PuenteApp().run()

# -*- coding: utf-8 -*-
"""
MI Worship — Puente (motor)
===========================
App de escritorio que conecta Logic (MIDI) con los músicos, leyendo los charts
DIRECTO del VPS (la nube es la única fuente de verdad; no hay biblioteca local).

Hace:
  1. Baja la biblioteca del VPS  (GET /api/sync/biblioteca?token=...)
  2. Escucha MIDI del IAC (canal configurable, por defecto 16)
  3. Sirve el visor (músicos) y el control, y sincroniza por WebSocket
  4. Manda el heartbeat al VPS  -> banner "EN VIVO"

Se puede correr solo para probar:   python3 puente_core.py            (con MIDI)
                                     python3 puente_core.py --sim      (sin MIDI)
"""
import os, sys, json, time, socket, threading, urllib.request, urllib.error
from pathlib import Path
from functools import wraps

from flask import Flask, render_template, jsonify, request, abort
try:
    from flask_sock import Sock
except ImportError:
    print("Falta instalar dependencias:  pip3 install flask flask-sock mido python-rtmidi certifi")
    sys.exit(1)

# ── Rutas / config ────────────────────────────────────────────
BASE = Path(__file__).resolve().parent            # App_Puente/  (o .../Resources empaquetado)
if getattr(sys, "frozen", False):
    if hasattr(sys, "_MEIPASS"):                  # Windows (PyInstaller)
        RECURSOS = Path(sys._MEIPASS)
    else:                                          # macOS (py2app)
        RECURSOS = Path(sys.executable).resolve().parent.parent / "Resources"
else:
    RECURSOS = BASE                               # App_Puente/ (autocontenida: templates, static, transposicion)
sys.path.insert(0, str(BASE))
sys.path.insert(0, str(RECURSOS))                 # para importar transposicion.py
from transposicion import transponer_cancion, transponer_acorde, usar_sostenidos

# Config y cache en un lugar escribible (no dentro del .app / .exe)
if sys.platform == "darwin":
    APP_SOPORTE = Path.home() / "Library" / "Application Support" / "NeuralWorship"
elif sys.platform.startswith("win"):
    APP_SOPORTE = Path(os.environ.get("APPDATA", str(Path.home()))) / "NeuralWorship"
else:
    APP_SOPORTE = Path.home() / ".config" / "NeuralWorship"
CONFIG_BUNDLED = [BASE / "config.json", RECURSOS / "config.json"]  # fabrica (script o .app)
ARCHIVO_CONFIG = APP_SOPORTE / "config.json"      # config del usuario (escribible)
ARCHIVO_SETLIST = APP_SOPORTE / "setlist.json"

DEFAULT_CONFIG = {
    "vps_url": "https://miworship.miiglesiainternacional.org",
    "token": "",
    "midi_channel": 16,
    "iac_port": "To Secuencias Live",
    "puerto": 5050,
    "simulacion": False,
}

def cargar_config():
    cfg = dict(DEFAULT_CONFIG)
    for p in CONFIG_BUNDLED + [ARCHIVO_CONFIG]:    # fabrica primero, luego lo del usuario
        try:
            cfg.update(json.loads(p.read_text(encoding="utf-8")))
        except Exception:
            pass
    return cfg

def guardar_config(cfg):
    try:
        APP_SOPORTE.mkdir(parents=True, exist_ok=True)
        ARCHIVO_CONFIG.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception as e:
        print(f"⚠️  No se pudo guardar config.json: {e}")

CONFIG = cargar_config()
APP_VERSION = "1.0"

NOTA_BASE_SECCION = 36   # C1
NOTA_FIN = 24            # C0
HEARTBEAT_INTERVALO_SEG = 30

# ── Estado global ─────────────────────────────────────────────
class Estado:
    def __init__(self):
        self.biblioteca = {}          # {numero: dict}
        self.cancion_actual = None
        self.seccion_actual = 0
        self.estado = "waiting"
        self.clientes = set()
        self.setlist = []
        self.pc_pendiente = None
        self.midi_channel = int(CONFIG.get("midi_channel", 16))   # 1-16
        self.midi_filtro = str(CONFIG.get("iac_port", ""))        # nombre/substring del puerto (loopMIDI, rtpMIDI, IAC...)
        self.midi_puerto_actual = ""                              # puerto realmente abierto
        self.midi_status = "iniciando"
        self.live_ok = False
        self.update_disponible = None

    def canal_midi_0based(self):
        return max(0, min(15, int(self.midi_channel) - 1))

    def cargar_setlist(self):
        if not ARCHIVO_SETLIST.exists():
            self.setlist = []; return
        try:
            data = json.loads(ARCHIVO_SETLIST.read_text(encoding="utf-8"))
            if "items" in data:
                self.setlist = [{"id": it["id"], "tono": it.get("tono")} for it in data["items"] if it.get("id") in self.biblioteca]
            elif "numeros" in data:
                self.setlist = [{"id": n, "tono": None} for n in data["numeros"] if n in self.biblioteca]
            else:
                self.setlist = []
        except Exception as e:
            print(f"⚠️  Error leyendo setlist: {e}"); self.setlist = []

    def guardar_setlist(self):
        try:
            APP_SOPORTE.mkdir(parents=True, exist_ok=True)
            ARCHIVO_SETLIST.write_text(json.dumps({"items": self.setlist}, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"⚠️  Error guardando setlist: {e}")

    def tono_del_setlist(self, numero):
        for it in self.setlist:
            if it["id"] == numero:
                return it["tono"]
        return None

    def calcular_semitonos(self, numero):
        tono_destino = self.tono_del_setlist(numero)
        if not tono_destino or numero not in self.biblioteca:
            return 0
        tono_original = self.biblioteca[numero].get("tono", "")
        if not tono_original or tono_original == tono_destino:
            return 0
        from transposicion import NOTA_A_INDICE
        import re
        def raiz(t):
            m = re.match(r'^([A-G][#b]?)', t); return m.group(1) if m else None
        ro, rd = raiz(tono_original), raiz(tono_destino)
        if not ro or not rd or ro not in NOTA_A_INDICE or rd not in NOTA_A_INDICE:
            return 0
        return (NOTA_A_INDICE[rd] - NOTA_A_INDICE[ro]) % 12

    def cancion_transpuesta(self, numero):
        if numero not in self.biblioteca:
            return None
        sem = self.calcular_semitonos(numero)
        return self.biblioteca[numero] if sem == 0 else transponer_cancion(self.biblioteca[numero], sem)

    def resumen_biblioteca(self):
        return [{"numero": c["numero"], "titulo": c["titulo"], "artista": c.get("artista", ""),
                 "tono": c.get("tono", ""), "portada": c.get("portada", "")}
                for c in sorted(self.biblioteca.values(), key=lambda x: x["numero"])]

    def resumen_setlist(self):
        out = []
        for it in self.setlist:
            n = it["id"]
            if n not in self.biblioteca:
                continue
            base = self.biblioteca[n]
            to = base.get("tono", "")
            te = it["tono"] or to
            out.append({"numero": n, "titulo": base["titulo"], "artista": base.get("artista", ""),
                        "tono": te, "tono_original": to, "transpuesto": te != to,
                        "portada": base.get("portada", "")})
        return out

    def snapshot(self):
        return {"tipo": "estado", "biblioteca": self.resumen_biblioteca(), "setlist": self.resumen_setlist(),
                "cancion": self.cancion_actual, "seccion": self.seccion_actual, "estado": self.estado}

estado = Estado()
_IP_LOCAL = "127.0.0.1"

# ── Biblioteca desde el VPS ───────────────────────────────────
def _ssl_ctx():
    try:
        import ssl, certifi
        return ssl.create_default_context(cafile=certifi.where())
    except Exception:
        return None

def actualizar_biblioteca():
    """Baja la biblioteca completa del VPS. Devuelve (ok, mensaje)."""
    url = CONFIG["vps_url"].rstrip("/") + "/api/sync/biblioteca"
    try:
        req = urllib.request.Request(url, headers={"Accept": "application/json",
                                                   "Authorization": "Bearer " + CONFIG.get("token", "")})
        ctx = _ssl_ctx()
        resp = urllib.request.urlopen(req, timeout=15, context=ctx) if ctx else urllib.request.urlopen(req, timeout=15)
        with resp as r:
            data = json.loads(r.read().decode("utf-8"))
        if not data.get("ok"):
            return False, "El VPS respondió sin ok (¿token?)"
        estado.biblioteca = {c["numero"]: c for c in data.get("canciones", [])}
        # Repertorio activo desde el VPS (ya no se crea en el control local)
        sl = data.get("setlist")
        if sl is not None:
            estado.setlist = [{"id": it["id"], "tono": it.get("tono")}
                              for it in sl if it.get("id") in estado.biblioteca]
            estado.guardar_setlist()
        else:
            estado.cargar_setlist()
        print(f"📚 Biblioteca del VPS: {len(estado.biblioteca)} canción(es) · repertorio: {data.get('setlist_nombre', '—')}")
        difundir(estado.snapshot())
        return True, f"{len(estado.biblioteca)} canciones"
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}"
    except Exception as e:
        return False, str(e)

# ── WebSocket broadcast ───────────────────────────────────────
lock_clientes = threading.Lock()

def difundir(mensaje):
    data = json.dumps(mensaje, ensure_ascii=False)
    muertos = []
    with lock_clientes:
        actuales = list(estado.clientes)
    for ws in actuales:
        try:
            ws.send(data)
        except Exception:
            muertos.append(ws)
    if muertos:
        with lock_clientes:
            for ws in muertos:
                estado.clientes.discard(ws)

# ── Acciones ──────────────────────────────────────────────────
def cargar_cancion(numero, fuente="manual"):
    if numero not in estado.biblioteca:
        print(f"⚠️  Canción #{numero} no está en la biblioteca"); return False
    if estado.cancion_actual and estado.cancion_actual.get("numero") == numero and estado.estado == "playing":
        return True
    if fuente == "midi":
        if estado.pc_pendiente == numero:
            return True
        estado.pc_pendiente = numero
        return True
    estado.pc_pendiente = None
    _aplicar_carga(numero)
    return True

def _confirmar_pc_pendiente():
    if estado.pc_pendiente is not None:
        n = estado.pc_pendiente; estado.pc_pendiente = None
        if not (estado.cancion_actual and estado.cancion_actual.get("numero") == n and estado.estado == "playing"):
            _aplicar_carga(n)

def _aplicar_carga(numero):
    cancion = estado.cancion_transpuesta(numero)
    if cancion is None:
        return
    estado.cancion_actual = cancion
    estado.seccion_actual = 0
    estado.estado = "playing"
    print(f"🎵 Canción #{numero}: {cancion['titulo']} [{cancion.get('tono','')}]")
    difundir({"tipo": "cancion", "cancion": estado.cancion_actual})

_hist = []
VENTANA_ANTIBUCLE = 1.5
LIMITE_ANTIBUCLE = 5

def saltar_seccion(indice, fuente="manual"):
    if fuente == "midi":
        _confirmar_pc_pendiente()
    if not estado.cancion_actual:
        return False
    if indice < 0 or indice >= len(estado.cancion_actual["secciones"]):
        return False
    if estado.seccion_actual == indice and estado.estado == "playing":
        return True
    ahora = time.time()
    _hist.append((ahora, indice))
    while _hist and ahora - _hist[0][0] > VENTANA_ANTIBUCLE:
        _hist.pop(0)
    if len(_hist) > LIMITE_ANTIBUCLE:
        _hist.clear(); return False
    estado.seccion_actual = indice
    estado.estado = "playing"
    sec = estado.cancion_actual["secciones"][indice]
    print(f"   → Sección {indice+1}: {sec['tipo']}")
    difundir({"tipo": "seccion", "indice": indice})
    return True

def marcar_fin():
    if not estado.cancion_actual:
        return False
    estado.cancion_actual = None; estado.seccion_actual = 0
    estado.estado = "waiting"; estado.pc_pendiente = None
    difundir({"tipo": "fin"})
    return True

def volver_espera():
    estado.cancion_actual = None; estado.seccion_actual = 0
    estado.estado = "waiting"; estado.pc_pendiente = None
    difundir(estado.snapshot())
    return True

def actualizar_setlist(numeros):
    prev = {it["id"]: it["tono"] for it in estado.setlist}
    vistos = set(); nuevo = []
    for n in numeros:
        try:
            n = int(n)
        except (TypeError, ValueError):
            continue
        if n in estado.biblioteca and n not in vistos:
            nuevo.append({"id": n, "tono": prev.get(n)}); vistos.add(n)
    estado.setlist = nuevo; estado.guardar_setlist()
    difundir({"tipo": "setlist", "setlist": estado.resumen_setlist()})
    return True

def transponer_item_setlist(numero, delta):
    if numero not in estado.biblioteca:
        return False
    it = next((x for x in estado.setlist if x["id"] == numero), None)
    if not it:
        return False
    base = estado.biblioteca[numero]
    tono_actual = it["tono"] or base.get("tono", "")
    if not tono_actual:
        return False
    ts = transponer_acorde(tono_actual, delta, usar_sost=True)
    nuevo = transponer_acorde(tono_actual, delta, usar_sost=usar_sostenidos(ts))
    it["tono"] = None if nuevo == base.get("tono", "") else nuevo
    estado.guardar_setlist()
    difundir({"tipo": "setlist", "setlist": estado.resumen_setlist()})
    if estado.cancion_actual and estado.cancion_actual.get("numero") == numero:
        _aplicar_carga(numero)
    return True

def resetear_tono_item(numero):
    it = next((x for x in estado.setlist if x["id"] == numero), None)
    if not it:
        return False
    it["tono"] = None; estado.guardar_setlist()
    difundir({"tipo": "setlist", "setlist": estado.resumen_setlist()})
    if estado.cancion_actual and estado.cancion_actual.get("numero") == numero:
        _aplicar_carga(numero)
    return True

def limpiar_setlist():
    estado.setlist = []; estado.guardar_setlist()
    difundir({"tipo": "setlist", "setlist": []})
    return True

def set_canal(n):
    """Cambiar el canal MIDI en caliente (1-16). Persiste en config."""
    try:
        n = int(n)
    except (TypeError, ValueError):
        return False
    if not (1 <= n <= 16):
        return False
    estado.midi_channel = n
    CONFIG["midi_channel"] = n
    guardar_config(CONFIG)
    print(f"🎚  Canal MIDI cambiado a {n}")
    return True

def set_midi_puerto(nombre):
    """Elegir el puerto MIDI a escuchar (nombre exacto o substring). Persiste."""
    estado.midi_filtro = (nombre or "").strip()
    CONFIG["iac_port"] = estado.midi_filtro
    guardar_config(CONFIG)
    print(f"🎹 Puerto MIDI seleccionado: {estado.midi_filtro or '(automático)'}")
    return True

# ── Escucha MIDI ──────────────────────────────────────────────
def _procesar_midi(msg):
    try:
        if hasattr(msg, "channel") and msg.channel != estado.canal_midi_0based():
            return
        if msg.type == "program_change":
            cargar_cancion(msg.program + 1, fuente="midi")
        elif msg.type == "note_on" and msg.velocity > 0:
            if msg.note == NOTA_FIN:
                marcar_fin()
            elif msg.note >= NOTA_BASE_SECCION:
                saltar_seccion(msg.note - NOTA_BASE_SECCION, fuente="midi")
    except Exception as e:
        print(f"⚠️  Error procesando MIDI: {e}")

def _elegir_puerto(puertos):
    """Escoge el puerto según el filtro; si no hay filtro y hay solo uno, ese."""
    filtro = (estado.midi_filtro or "").strip().lower()
    if filtro:
        return next((p for p in puertos if filtro in p.lower()), None)
    if len(puertos) == 1:
        return puertos[0]
    return None

def escuchar_midi():
    """Supervisor: encuentra el puerto, escucha con poll, reconecta y permite
    cambiar de puerto en caliente (loopMIDI, rtpMIDI, IAC, hardware...)."""
    try:
        import mido
    except ImportError:
        print("⚠️  mido no está instalado, MIDI deshabilitado")
        estado.midi_status = "sin mido"; return
    while True:
        try:
            puertos = mido.get_input_names()
        except Exception as e:
            estado.midi_status = f"error MIDI: {e}"; time.sleep(2); continue
        objetivo = _elegir_puerto(puertos)
        if not objetivo:
            estado.midi_puerto_actual = ""
            estado.midi_status = "elegí un puerto MIDI" if puertos else "esperando puerto MIDI…"
            time.sleep(2); continue
        estado.midi_puerto_actual = objetivo
        estado.midi_status = f"Escuchando: {objetivo}"
        print(f"🎹 Escuchando MIDI en: {objetivo}  (canal {estado.midi_channel})")
        filtro_ini = estado.midi_filtro
        ultimo_check = time.time()
        try:
            with mido.open_input(objetivo) as port:
                while True:
                    if estado.midi_filtro != filtro_ini:
                        break                                  # cambiaron el puerto -> reabrir
                    ahora = time.time()
                    if ahora - ultimo_check > 1.0:             # ¿el puerto sigue existiendo?
                        ultimo_check = ahora
                        if objetivo not in mido.get_input_names():
                            break
                    msg = port.poll()
                    if msg is None:
                        time.sleep(0.005); continue
                    _procesar_midi(msg)
        except Exception as e:
            estado.midi_status = f"error MIDI: {e}"
            print(f"⚠️  {e}")
            time.sleep(2)
        estado.midi_puerto_actual = ""

# ── Servidor web ──────────────────────────────────────────────
app = Flask(__name__, template_folder=str(RECURSOS / "templates"), static_folder=str(RECURSOS / "static"))
sock = Sock(app)

def solo_local(f):
    """Restringe una ruta a la propia Mac (127.0.0.1). Los músicos NO entran aquí."""
    @wraps(f)
    def _wrap(*a, **k):
        if request.remote_addr not in ("127.0.0.1", "::1", "localhost"):
            abort(403)
        return f(*a, **k)
    return _wrap

@app.route("/")
def visor():
    return render_template("visor.html")

@app.route("/api/cancion/<int:numero>")
def api_obtener_cancion(numero):
    if numero in estado.biblioteca:
        return jsonify({"ok": True, "cancion": estado.cancion_transpuesta(numero)})
    return jsonify({"ok": False, "error": "no encontrada"}), 404

@app.route("/panel")
@solo_local
def panel():
    return render_template("panel.html")

@app.route("/api/estado")
@solo_local
def api_estado():
    est = estado
    puerto = int(CONFIG.get("puerto", 5050))
    return jsonify({
        "ok": True,
        "live": est.live_ok,
        "midi": est.midi_status,
        "puerto_midi": est.midi_puerto_actual,
        "musicos": len(est.clientes),
        "biblioteca": len(est.biblioteca),
        "repertorio": len(est.setlist),
        "repertorio_lista": [{"titulo": x["titulo"], "tono": x["tono"]} for x in est.resumen_setlist()],
        "canal": est.midi_channel,
        "sonando": (est.cancion_actual or {}).get("titulo") if est.cancion_actual else None,
        "estado": est.estado,
        "update": est.update_disponible,
        "ip": _IP_LOCAL,
        "puerto": puerto,
        "url": f"http://{_IP_LOCAL}:{puerto}",
    })

@app.route("/api/set_canal", methods=["POST"])
@solo_local
def api_set_canal():
    d = request.get_json(silent=True) or {}
    return jsonify({"ok": set_canal(d.get("canal"))})

@app.route("/api/actualizar", methods=["POST"])
@solo_local
def api_actualizar():
    ok, msg = actualizar_biblioteca()
    return jsonify({"ok": ok, "mensaje": msg})

@app.route("/api/abrir_descarga", methods=["POST"])
@solo_local
def api_abrir_descarga():
    import webbrowser
    webbrowser.open(CONFIG["vps_url"].rstrip("/") + "/admin")
    return jsonify({"ok": True})

@app.route("/api/qr.svg")
@solo_local
def api_qr():
    from io import BytesIO
    from flask import Response
    import qrcode
    import qrcode.image.svg as qsvg
    url = f"http://{_IP_LOCAL}:{int(CONFIG.get('puerto', 5050))}"
    img = qrcode.make(url, image_factory=qsvg.SvgPathImage, box_size=11, border=2)
    buf = BytesIO(); img.save(buf)
    return Response(buf.getvalue(), mimetype="image/svg+xml")

@app.route("/api/midi_puertos")
@solo_local
def api_midi_puertos():
    try:
        import mido
        puertos = list(mido.get_input_names())
    except Exception:
        puertos = []
    return jsonify({"ok": True, "puertos": puertos,
                    "actual": estado.midi_puerto_actual, "filtro": estado.midi_filtro})

@app.route("/api/set_midi_puerto", methods=["POST"])
@solo_local
def api_set_midi_puerto():
    d = request.get_json(silent=True) or {}
    return jsonify({"ok": set_midi_puerto(d.get("puerto", ""))})

@sock.route("/ws")
def ws_handler(ws):
    with lock_clientes:
        estado.clientes.add(ws)
    try:
        ws.send(json.dumps(estado.snapshot(), ensure_ascii=False))
        while True:
            msg = ws.receive(timeout=90)
            if msg is None:
                break
            try:
                if json.loads(msg).get("tipo") == "ping":
                    ws.send(json.dumps({"tipo": "pong"}))
            except Exception:
                pass
    except Exception:
        pass
    finally:
        with lock_clientes:
            estado.clientes.discard(ws)

# ── Heartbeat al VPS ──────────────────────────────────────────
def ip_local():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80)); ip = s.getsockname()[0]; s.close(); return ip
    except Exception:
        return "127.0.0.1"

def enviar_heartbeat(accion="ping"):
    try:
        data = json.dumps({"token": CONFIG.get("token", ""), "ip": ip_local(), "accion": accion}).encode("utf-8")
        req = urllib.request.Request(CONFIG["vps_url"].rstrip("/") + "/api/live_ping", data=data,
                                     headers={"Content-Type": "application/json"}, method="POST")
        ctx = _ssl_ctx()
        opener = urllib.request.urlopen(req, timeout=10, context=ctx) if ctx else urllib.request.urlopen(req, timeout=10)
        with opener as resp:
            estado.live_ok = (resp.status == 200)
            return estado.live_ok
    except Exception:
        estado.live_ok = False
        return False

def _ver_tuple(v):
    try:
        return tuple(int(x) for x in str(v).split("."))
    except Exception:
        return (0,)

def verificar_version():
    """Consulta la ultima version publicada en el VPS y marca si hay update."""
    try:
        url = CONFIG["vps_url"].rstrip("/") + "/api/version"
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        ctx = _ssl_ctx()
        resp = urllib.request.urlopen(req, timeout=10, context=ctx) if ctx else urllib.request.urlopen(req, timeout=10)
        with resp as r:
            data = json.loads(r.read().decode("utf-8"))
        latest = str(data.get("version", "")).strip()
        if latest and _ver_tuple(latest) > _ver_tuple(APP_VERSION):
            estado.update_disponible = latest
            print(f"\u2b06\ufe0f  Actualizacion disponible: v{latest} (tenes v{APP_VERSION})")
        else:
            estado.update_disponible = None
    except Exception:
        pass

def hilo_heartbeat():
    enviar_heartbeat("ping")
    while True:
        time.sleep(HEARTBEAT_INTERVALO_SEG)
        enviar_heartbeat("ping")

# ── Arranque ──────────────────────────────────────────────────
def arrancar_servicios(simulacion=None):
    """Inicia MIDI + heartbeat en hilos. Devuelve la IP local. (No arranca Flask.)"""
    if simulacion is None:
        simulacion = bool(CONFIG.get("simulacion", False))
    ok, msg = actualizar_biblioteca()
    if not ok:
        print(f"⚠️  No se pudo bajar la biblioteca del VPS: {msg}")
    if not simulacion:
        threading.Thread(target=escuchar_midi, daemon=True).start()
    else:
        estado.midi_status = "simulación (sin MIDI)"
    threading.Thread(target=hilo_heartbeat, daemon=True).start()
    threading.Thread(target=verificar_version, daemon=True).start()
    global _IP_LOCAL
    _IP_LOCAL = ip_local()
    return _IP_LOCAL

def main():
    simulacion = "--sim" in sys.argv
    print("=" * 60); print("  MI Worship — Puente (motor)"); print("=" * 60)
    ip = arrancar_servicios(simulacion)
    puerto = int(CONFIG.get("puerto", 5050))
    print(f"\n🌐 Visor (músicos):  http://{ip}:{puerto}\n")
    try:
        app.run(host="0.0.0.0", port=puerto, debug=False, use_reloader=False)
    except KeyboardInterrupt:
        print("\n👋 Cerrando…")
    finally:
        enviar_heartbeat("bye")

if __name__ == "__main__":
    main()

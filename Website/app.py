#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────
# NeuralWorship — Website (sitio de marketing)
#
# App Flask INDEPENDIENTE que sirve SOLO el sitio público:
#   /             → landing
#   /neuralplay   → página de producto NeuralPlay
#   /neuralcharts → página de producto NeuralCharts
#
# El resto del ecosistema (registro, login admin, /api, /app…) lo sirve la
# app principal `charts_app`. Nginx enruta cada ruta al servicio correcto.
# Este servicio corre en 127.0.0.1:5052 (gunicorn) detrás de nginx.
# ─────────────────────────────────────────────────────────────
import os, sys
from flask import Flask, render_template, redirect

# Fuente ÚNICA de planes y catálogo de funciones (compartida con charts_app).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "shared"))
from planes_catalogo import PAQUETES, catalogo_features, catalogo_plan

app = Flask(__name__)
app.jinja_env.globals["catalogo_features"] = catalogo_features
app.jinja_env.globals["catalogo_plan"] = catalogo_plan

# URL de la app principal (para enlaces que viven en charts_app).
APP_PRINCIPAL = "https://neuralworship.com"


import datetime as _dt
# Modo lanzamiento: muestra el banner "gratis" hasta la fecha; en enero poner False.
MODO_LANZAMIENTO = True
FIN_LANZAMIENTO  = "2026-12-31"   # inclusive (YYYY-MM-DD)
def _en_lanzamiento():
    return bool(MODO_LANZAMIENTO) and _dt.date.today().isoformat() <= FIN_LANZAMIENTO


@app.route("/")
def home():
    return render_template("landing.html", paquetes=PAQUETES, modo_lanzamiento=_en_lanzamiento())


@app.route("/neuralplay")
def neuralplay():
    return render_template("neuralplay.html")


@app.route("/neuralcharts")
def neuralcharts():
    return render_template("neuralcharts.html")


# ── Endpoints "puente" ──
# Existen SOLO para que url_for() en las plantillas genere la ruta correcta.
# En producción nginx enruta estas rutas a la app principal, así que estos
# handlers casi nunca se ejecutan; si alguien llega directo, redirige a la app.
def _puente(path):
    return redirect(APP_PRINCIPAL + path)


app.add_url_rule("/crear-organizacion", "crear_organizacion", lambda: _puente("/crear-organizacion"))
app.add_url_rule("/admin/login",        "admin_login",        lambda: _puente("/admin/login"))
app.add_url_rule("/unirse",             "unirse",             lambda: _puente("/unirse"))
app.add_url_rule("/terminos",           "terminos",           lambda: _puente("/terminos"))
app.add_url_rule("/privacidad",         "privacidad",         lambda: _puente("/privacidad"))
app.add_url_rule("/derechos",           "derechos",           lambda: _puente("/derechos"))


@app.route("/healthz")
def healthz():
    return "ok", 200


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5052, debug=False)

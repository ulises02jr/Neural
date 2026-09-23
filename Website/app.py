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
from flask import Flask, render_template, redirect

app = Flask(__name__)

# URL de la app principal (para enlaces que viven en charts_app).
APP_PRINCIPAL = "https://neuralworship.com"

# Planes que muestra la landing. Se replican aquí para que el Website no dependa
# de la base de datos. Si cambian los precios en la app, actualizar también aquí.
PAQUETES = {
    "basico":     {"nombre": "Básico",  "precio": 0.0,  "asientos": 2,  "gb": 25,  "midi": False},
    "premium":    {"nombre": "Plus",    "precio": 10.0, "asientos": 5,  "gb": 100, "midi": True},
    "ministerio": {"nombre": "Premium", "precio": 20.0, "asientos": 10, "gb": 200, "midi": True},
}


@app.route("/")
def home():
    return render_template("landing.html", paquetes=PAQUETES)


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

# -*- coding: utf-8 -*-
"""
Empaquetar MI Worship Puente como .app (macOS)

    python3 setup.py py2app

El resultado queda en  dist/MI Worship Puente.app
"""
import os, shutil
from setuptools import setup

BASE = os.path.dirname(os.path.abspath(__file__))

# Recolectar templates/ y static/ recursivamente (autocontenidos en App_Puente)
def arbol(src, dest):
    salida = []
    if not os.path.isdir(src):
        return salida
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        rel = os.path.relpath(root, src)
        destino = dest if rel == "." else os.path.join(dest, rel)
        archivos = [os.path.join(root, f) for f in files if not f.startswith(".")]
        if archivos:
            salida.append((destino, archivos))
    return salida

DATA_FILES = []
DATA_FILES += arbol(os.path.join(BASE, "templates"), "templates")
DATA_FILES += arbol(os.path.join(BASE, "static"), "static")
DATA_FILES += [("", [os.path.join(BASE, "config.json")])]

OPTIONS = {
    "argv_emulation": False,
    "iconfile": os.path.join(BASE, "icono.icns"),
    "includes": ["transposicion"],
    "packages": [
        "flask", "flask_sock", "simple_websocket", "wsproto",
        "jinja2", "werkzeug", "click", "markupsafe", "itsdangerous",
        "qrcode", "mido", "rtmidi", "certifi",
    ],
    "plist": {
        "CFBundleName": "NeuralWorship",
        "CFBundleDisplayName": "NeuralWorship",
        "CFBundleIdentifier": "org.miiglesiainternacional.neuralworship",
        "CFBundleVersion": "1.0.0",
        "CFBundleShortVersionString": "1.0",
        "LSUIElement": True,   # utilidad de barra de menú (sin ícono en el Dock)
        "NSHighResolutionCapable": True,
        "NSAppTransportSecurity": {"NSAllowsLocalNetworking": True},
        "LSMinimumSystemVersion": "11.0",
        "NSHumanReadableCopyright": "© Mi Iglesia Internacional",
    },
}

setup(
    name="NeuralWorship",
    app=["app.py"],
    data_files=DATA_FILES,
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
)

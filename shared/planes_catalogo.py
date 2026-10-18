# -*- coding: utf-8 -*-
"""
FUENTE UNICA DE PLANES Y FUNCIONES (catalogo).

La importan las DOS apps:
  - charts_app (servidor_vps.py, puerto 5051)  -> admin, pagina de planes
  - website   (app.py, puerto 5052)            -> landing publica

Para cambiar un plan, un precio o el texto/inclusion de una funcion, se edita
AQUI y nada mas. En el servidor vive en /home/charts/shared/planes_catalogo.py
y ambas apps agregan ../shared a sys.path.

Nota: el "modo lanzamiento" (acceso gratis temporal) NO vive aqui: es propio de
charts_app. Este modulo entrega siempre las capacidades REALES de cada plan.
"""

# -- Definicion canonica de paquetes --
ASIENTOS_ILIM = 100000   # centinela: asientos "ilimitados"
PAQUETES = {
    "basico":     {"nombre": "Básico",  "precio": 0.0,  "asientos": 1,             "gb": 5,   "midi": False, "salidas": 2,  "tipo": "iglesia"},
    "premium":    {"nombre": "Plus",    "precio": 9.99, "asientos": 5,             "gb": 50,  "midi": True,  "salidas": 32, "tipo": "iglesia"},
    "ministerio": {"nombre": "Premium", "precio": 19.99, "asientos": ASIENTOS_ILIM, "gb": 100, "midi": True,  "salidas": 32, "tipo": "iglesia"},
    "reproductor": {"nombre": "NeuralPlay", "precio": 12.99, "asientos": 0, "gb": 100, "midi": True,  "salidas": 32, "tipo": "reproductor"},
    "sync":        {"nombre": "NeuralSync", "precio": 10.0, "asientos": 3, "gb": 25, "midi": False, "salidas": 2,  "tipo": "sync"},
}


def features_de_plan(paquete):
    """Capacidades REALES de un plan (sin modo lanzamiento)."""
    p = PAQUETES.get(paquete, PAQUETES["basico"])
    full = bool(p.get("midi"))
    repro = p.get("tipo") == "reproductor"
    sync = p.get("tipo") == "sync"
    return {
        "midi": full,
        "infinito": full,
        "neuralsync": False,
        "export_pdf": (full and not repro) or sync,
        "charts": (not repro),
        "usuarios": (not repro),
        "stems_propios": repro,
        "solo_reproductor": repro,
        "neuralplay": (not sync),
        "tipo": p.get("tipo", "iglesia"),
        "salidas": p.get("salidas", 2),
        "asientos": p.get("asientos", 3),
        "gb": p.get("gb", 20),
        "paquete": paquete,
        "nombre": p.get("nombre", ""),
    }


# -- Catalogo unico de funciones --
# (seccion, etiqueta, regla, clave_i18n). etiqueta y clave_i18n pueden ser funcion(f).
CATALOGO_FEATURES = [
    ("General", "Creación de repertorios",                                  lambda f: True,                           "f_repertorios"),
    ("General", "Cargar pads",                                              lambda f: True,                           "f_pads"),
    ("General", "Cargar multitracks",                                       lambda f: True,                           "f_multitracks"),
    ("General", "Cargar tus propios cifrados",                              lambda f: f["charts"],                    "f_cifprop"),
    ("General", "Almacenar notas MIDI",                                     lambda f: f["midi"],                      "f_notasmidi"),
    ("General", "Exportar lyrics para presentación",                        lambda f: f["export_pdf"],                "f_exlyrics"),
    ("General", "Exportar charts en PDF",                                   lambda f: f["export_pdf"],                "f_expdf"),

    ("NeuralCharts", "Charts (cifrado + letras)",                           lambda f: f["charts"],                    "f_charts"),
    ("NeuralCharts", "Sincroniza tus cifrados con NeuralPlay",              lambda f: f["charts"] and f["neuralplay"],"f_synccif"),
    ("NeuralCharts", "Reproductor de multitracks para ensayo (sincronizado con cifrados)", lambda f: f["charts"],     "f_reproens"),
    ("NeuralCharts", "Disponible para iPhone, iPad y Android",              lambda f: f["charts"],                    "f_disp"),

    ("NeuralPlay", "Reproductor de multitracks / stems",                    lambda f: f["neuralplay"],                "f_reprostems"),
    ("NeuralPlay", "Cambio de tono/tempo fuera de línea a los 12 tonos",    lambda f: f["neuralplay"],                "f_tonotempo"),
    ("NeuralPlay", "Crea y guarda varios repertorios",                      lambda f: f["neuralplay"],                "f_variosrep"),
    ("NeuralPlay", "Sincronización en vivo con NeuralCharts",               lambda f: f["neuralplay"] and f["charts"],"f_syncvivo"),
    ("NeuralPlay", "Salto de secciones en vivo",                            lambda f: f["neuralplay"],                "f_salto"),
    ("NeuralPlay", "Edición de secciones del arreglo original",             lambda f: f["neuralplay"],                "f_edicion"),
    ("NeuralPlay", "Transiciones de canciones en vivo (Autolink y Crossfade)", lambda f: f["midi"],                   "f_transiciones"),
    ("NeuralPlay", "Ajusta tiempo inicial y final",                         lambda f: f["neuralplay"],                "f_tiempo"),
    ("NeuralPlay", "Reproduce pads ambientales con las canciones",          lambda f: f["neuralplay"],                "f_padsamb"),
    ("NeuralPlay", "Interfaz avanzada de Pad Player",                       lambda f: f["neuralplay"],                "f_padadv"),
    ("NeuralPlay", "NeuralPlay para Mac, iPad y iPhone",                    lambda f: f["neuralplay"],                "f_npmac"),
    ("NeuralPlay", lambda f: ("Hasta 32 MultiOuts® con buses" if f["midi"] else "2 salidas de audio"), lambda f: f["neuralplay"], lambda f: ("f_out32" if f["midi"] else "f_out2")),
    ("NeuralPlay", "Añadir sección de clicks a las canciones al final",     lambda f: f["neuralplay"],                "f_clicks"),
    ("NeuralPlay", "Repite secciones con Loop / Loop Infinito",             lambda f: f["midi"],                      "f_loop"),
    ("NeuralPlay", "Asignación MIDI / teclado / controlador",               lambda f: f["midi"],                      "f_midiasig"),
    ("NeuralPlay", "Reproductor de notas MIDI en sincronización",           lambda f: f["midi"],                      "f_midisync"),
    ("NeuralPlay", "Múltiples salidas MIDI para lyrics/luces y efectos",    lambda f: f["midi"],                      "f_midimulti"),
    ("NeuralPlay", "MIDI Clock",                                            lambda f: f["midi"],                      "f_midiclock"),
    ("NeuralPlay", "Gestión de almacenamiento en dispositivo",             lambda f: f["neuralplay"],                "f_gestalm"),
]


def catalogo_features(f):
    """Lista del catalogo con etiqueta resuelta, 'incluido' y clave i18n."""
    out = []
    for sec, etq, regla, i18n in CATALOGO_FEATURES:
        label = etq(f) if callable(etq) else etq
        key = i18n(f) if callable(i18n) else i18n
        out.append({"seccion": sec, "label": label, "incluido": bool(regla(f)), "i18n": key})
    return out


def catalogo_plan(pk):
    """Catalogo de un plan por su clave, con capacidades REALES (sin lanzamiento)."""
    return catalogo_features(features_de_plan(pk))

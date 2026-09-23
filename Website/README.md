# NeuralWorship — Website

Sitio de marketing de NeuralWorship (landing + páginas de producto). Es un
proyecto **independiente** de la app principal (`charts_app`).

## Qué sirve
- `/` — landing
- `/neuralplay` — página de producto NeuralPlay
- `/neuralcharts` — página de producto NeuralCharts
- `/healthz` — health check

El registro, el login de admin, la API y la app de músicos los sirve
`charts_app`. Nginx enruta cada ruta al servicio correcto.

## Estructura
```
Website/
├── app.py              # Flask (sirve landing + productos)
├── requirements.txt
├── templates/          # landing.html, neuralplay.html, neuralcharts.html
└── static/             # marca.png, logo.png, neuralplay_icon.png
    └── landing/        # capturas de las apps + logo.svg
```

## Correr local
```bash
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
python app.py           # http://127.0.0.1:5052
```

## Producción (VPS)
- Ruta: `/home/charts/website`
- Servicio: `website.service` (systemd → gunicorn en `127.0.0.1:5052`)
- Nginx enruta `/`, `/neuralplay`, `/neuralcharts` y `/static/landing/` aquí;
  todo lo demás va a `charts_app` (5051).

Desplegar cambios: subir archivos y `systemctl restart website.service`.

## Nota
Los precios de los planes están replicados en `app.py` (`PAQUETES`). Si cambian
en la app principal, actualizarlos aquí también.

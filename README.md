# Charts VPS

Servidor Flask para charts/cifrados de canciones cristianas en modo ensayo.
Permite que los músicos vean el setlist y biblioteca desde cualquier dispositivo.

## Estructura

- `servidor_vps.py` - Servidor Flask
- `templates/` - HTML templates
- `canciones/` - JSONs de canciones (no incluidos en git)

## Uso local

```bash
python3 -m venv venv
source venv/bin/activate
pip install flask
python3 servidor_vps.py
```

Abrí http://localhost:5051 en el navegador.

## Passwords por defecto

- Músicos: `musicos2026`
- Admin: `admin2026`

**Cambialos al primer login** desde /admin.

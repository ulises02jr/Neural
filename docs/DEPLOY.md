# Deploy y respaldo

## Filosofía

**Producción se edita en el VPS. GitHub es solo respaldo.** No se despliega *desde*
GitHub: el servidor vive y se edita en `/home/charts/charts_app` en el VPS, y GitHub
guarda una copia ordenada (este monorepo) como respaldo y control de versiones.

```
[ VPS: /home/charts/charts_app ]  ── respaldar.sh ──▶  [ repo mono ]  ── git push ──▶  [ GitHub ]
        (producción viva)                                 server/                        (respaldo)
```

## Servidor en producción (VPS)

- Carpeta viva: `/home/charts/charts_app`
- Servicio: `charts.service` (systemd) → gunicorn `127.0.0.1:5051` (2 workers)
- Nginx + Let's Encrypt al frente.
- Tras cambios de backend: verificar sintaxis y `systemctl restart charts.service`.

> El monorepo **no** cambia cómo corre producción. La carpeta viva sigue igual.

## Cómo respaldar a GitHub

El repo mono está clonado en el VPS (p. ej. `/home/charts/Neural`). Para respaldar
el estado actual del servidor:

```bash
cd /home/charts/Neural
./respaldar.sh "mensaje describiendo el cambio"
```

El script copia **solo el código** del servidor (nunca datos ni secretos) a `server/`,
y hace commit + push. Ver `.gitignore` para lo que nunca se sube.

## Levantar un servidor nuevo desde este repo

1. Copiar `server/` a la máquina destino.
2. `cp server/config.example.json server/config.json` y llenar valores.
3. `cp server/secrets.example.json server/secrets.json` y llenar valores.
4. `python3 -m venv venv && venv/bin/pip install -r server/requirements.txt`
5. Configurar systemd (gunicorn) + Nginx + certificado.

## Nunca subir a Git

`secrets.json`, `config.json`, `usuarios.db`, `pistas/`, `canciones/*.json`,
`static/portadas/`, `descargas/`, `backups_canciones/`, `venv/`, `*.bak`.

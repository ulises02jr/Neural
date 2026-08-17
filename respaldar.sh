#!/usr/bin/env bash
# Respaldo del servidor MI Worship a GitHub (monorepo).
#   Copia SOLO el codigo del servidor (nunca datos ni secretos) a server/,
#   y hace commit + push. Produccion no se toca.
#
# Uso:  ./respaldar.sh "mensaje del commit"
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
PROD_DIR="${MIW_PROD_DIR:-/home/charts/charts_app}"
MSG="${1:-Respaldo del servidor $(date +%F_%H-%M)}"

if [ ! -d "$PROD_DIR" ]; then
  echo "No existe el directorio de produccion: $PROD_DIR" >&2
  exit 1
fi

echo "Sincronizando codigo del servidor  ($PROD_DIR  ->  server/)"
rsync -a \
  --exclude='.git' \
  --exclude='venv' \
  --exclude='__pycache__' \
  --exclude='config.json' \
  --exclude='secrets.json' \
  --exclude='usuarios.db' \
  --exclude='*.db' \
  --exclude='*.bak' --exclude='*.bak-*' --exclude='*.bak*' \
  --exclude='pistas' \
  --exclude='pads' \
  --exclude='descargas' \
  --exclude='backups_canciones' \
  --exclude='canciones' \
  --exclude='static/portadas' \
  --exclude='README.md' \
  "$PROD_DIR"/ "$REPO_DIR"/server/

cd "$REPO_DIR"
git add -A
if git diff --cached --quiet; then
  echo "Sin cambios que respaldar."
  exit 0
fi
git commit -m "$MSG"
git push
echo "OK — respaldado a GitHub."

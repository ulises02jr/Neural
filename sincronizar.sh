#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────
# Sincroniza el repositorio Neural con GitHub.
#  - Si hay cambios sin commitear, los commitea.
#  - Intenta subir DIRECTO a GitHub (si tu Mac ya tiene acceso).
#  - Si no hay acceso directo, sube por el puente: Mac -> VPS -> GitHub.
#
# Uso:
#   ./sincronizar.sh "mensaje del commit"
#   ./sincronizar.sh              (usa un mensaje con la fecha)
# ─────────────────────────────────────────────────────────────
set -e
cd "$(dirname "$0")"

VPS="root@68.183.106.62"
VPS_REPO="/home/charts/Neural"

# 1) Commit (solo si hay algo que guardar)
if [ -n "$(git status --porcelain)" ]; then
  MSG="${1:-Actualizacion $(date '+%Y-%m-%d %H:%M')}"
  echo "→ Cambios detectados, commiteando: $MSG"
  git add -A
  git commit -m "$MSG"
else
  echo "→ Sin cambios nuevos que commitear."
fi

# 2) Subir a GitHub
echo "→ Subiendo a GitHub…"
if git push origin main 2>/dev/null; then
  echo "✓ Subido DIRECTO a GitHub."
else
  echo "  (sin acceso directo — usando el puente por el VPS)"
  git push vps main
  ssh "$VPS" "cd $VPS_REPO && git push origin main"
  # Deja la referencia local al día (tu Mac no lee GitHub directo por ahora)
  git update-ref refs/remotes/origin/main "$(git rev-parse main)"
  echo "✓ Subido vía VPS -> GitHub."
fi

echo "✓ Listo. Mac, VPS y GitHub sincronizados en $(git rev-parse --short main)."

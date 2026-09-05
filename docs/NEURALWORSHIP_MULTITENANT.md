# NeuralWorship — Diseño Multi-Tenant (SaaS)

> Documento vivo. Captura las decisiones de arquitectura para convertir el sistema de
> una sola organización (local) a multi-inquilino (venta comercial). Se actualiza conforme decidimos.

## 1. Meta
Cada cliente (iglesia/equipo) = su propia **organización aislada**: sus canciones, setlists, pads,
usuarios y token. Ninguna organización ve datos de otra. Marca fija = NeuralWorship (no personalizable).

## 2. Entidades / Base de datos
- **organizations**: id (ID corto y único, sirve de código para unirse), nombre (solo display, puede repetirse),
  owner_user_id, token (secreto, enlaza NeuralPlay/API), paquete, max_musicos, estado_suscripcion, creado.
- **usuarios**: + columna **org_id** (cada usuario pertenece a una organización), rol (admin/musico), estado (activo/pendiente).
- **invitaciones**: org_id, email, token (único, con vencimiento), estado (pendiente/usado).
- Canciones, setlists, pads, y el token de NeuralPlay → **todo lleva org_id**.
- El `config.json` global (setlists + token único) se reparte por organización.

## 3. Roles
- **Admin/Dueño**: crea y administra la organización (biblioteca, setlists, invitaciones, suscripción).
  **También es músico** (ver §7). No consume asiento de músico.
- **Músico**: ve charts (visor nube + visor local en LAN). Entra solo por invitación o self-registro con ID (con aprobación).

## 4. Alta / Onboarding
### Admin (cliente que compra)
- Se registra desde la web/"Crear cuenta" con un campo NUEVO: **Nombre de la organización**.
- Al registrarse el sistema crea: organización + usuario admin (dueño) + **ID de organización único** + **token secreto**.
- Auto-aprobado (quitar el mensaje "un administrador debe aprobarla" en el registro de admin).
- Debe **pagar** para usar (cobro por organización, con tarifas por versión/paquete). Detalle de pago = tema aparte.

### Músico — dos caminos
1. **Invitación por correo** (auto-aprobado): el admin ingresa el email → se envía invitación con un
   **token de invitación seguro** (ligado a ese email, con vencimiento). El músico abre el link → va DIRECTO
   al formulario de crear cuenta con el **nombre de organización autollenado** y el **ID fijado por el servidor** (no editable).
2. **Self-registro con ID** (requiere aprobación): si el admin no pidió el correo, le pasa el **ID** y le dice
   "andá a registrarte". El músico se registra poniendo el ID → queda **pendiente** → el admin lo **aprueba**.
   (Reutiliza el flujo de aprobación que ya existe.)
- **Límite de músicos** por organización según paquete (por ahora fijo = 5). Se valida al invitar/aprobar.
- Registro abierto sin ID/invitación → **cerrado**.

## 5. NeuralPlay / NeuralSync
- Se les agrega **inicio de sesión** (email + contraseña = las mismas del admin en la web).
- Al iniciar, reciben el **token de su organización** → bajan SOLO la biblioteca de esa organización.
- El "¿Quién sos?" del visor local = roster de músicos de esa organización.

## 6. Almacenamiento (aislado por organización)
- **Una carpeta por organización** en el volumen:
  `/mnt/ssd50gb/charts_data/orgs/<ID-org>/{canciones,pistas,pads}/…`
- El servidor arma rutas con el org_id del usuario logueado → aislamiento físico (A no ve carpeta de B).
- **Cuota de almacenamiento** por paquete (medible con `du` por carpeta). Volumen ampliable en DigitalOcean.

## 7. El admin también es músico
- Sus credenciales sirven para el panel de músico/visor (ya el decorador permite admin en rutas de músico).
- Agregar en el panel admin un **botón de acceso directo al visor** (sugerencia de nombre: "Ver como músico" / "Entrar al visor" / "Tocar en vivo").
- El dueño NO consume asiento de músico (los 5 son para los invitados).

## 8. Cobro / Vendedor
- Cobro **por organización**, tarifas por versión/paquete. Pago requerido (después del registro).
- **Panel de súper-admin (para el vendedor, NeuralWorship)**: ver todas las organizaciones, finanzas,
  activar/suspender. Separado de los admins de cada organización.
- Qué pasa si no paga (bloqueo/solo-lectura) → definir con el tema de suscripción.

## 9. Seguridad (columna vertebral)
- **Toda** consulta/ruta filtra por el **org_id** del usuario logueado (nadie ve datos de otra organización cambiando la URL).
- Invitaciones con token seguro + vencimiento (no self-join adivinando el ID).
- Cerrar registro abierto.

## 10. Dependencias
- **Correo funcionando** (noreply@neuralworship.com) es requisito: invitaciones + "olvidé mi contraseña".
  → Adelantar el correo (estaba como último) porque las invitaciones lo necesitan.

## 11. Fases (sin romper lo actual)
1. Meter organización en la base (organizations + org_id en usuarios) y migrar lo actual como "organización #1".
2. Aislar biblioteca + config + token por organización (carpetas por org_id).
3. Invitaciones (correo) + self-registro con ID + aprobación + cerrar registro abierto.
4. Login en NeuralPlay/NeuralSync con credenciales → token de organización.
5. Suscripción/cobro por organización + panel de súper-admin.

## Decisiones tomadas
- Cobro por organización (no por músico), con tarifas por versión.
- Dos caminos de alta de músico: invitación (auto) y self-registro con ID (aprobación).
- Admin = también músico (botón al visor; no consume asiento).
- Marca fija NeuralWorship (sin personalización de nombre/logo/acento).

## Pendiente de decidir
- ¿Prueba antes de pagar, o pago obligatorio desde el registro?
- Tarifas/paquetes concretos (límites de músicos y almacenamiento por paquete).
- Detalles de la pasarela de pago (Stripe/PayPal) y el panel de súper-admin.

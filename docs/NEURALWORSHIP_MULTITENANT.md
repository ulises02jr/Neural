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
- Detalles de la pasarela de pago (Stripe/PayPal) y el panel de súper-admin.
- (Prueba de 7 días y tarifas/paquetes → YA decididos, ver §12.)

---

## Actualización (decisiones nuevas)
- Botón admin→visor se llamará **"Modo Músico"** (ícono en la cuadrícula del panel admin).
- Dueño/admin es músico y **no consume asiento** (los asientos quedan libres para invitados) — confirmado.
- **Prueba gratis: 7 días** (máximo).
- **Correo (noreply@neuralworship.com) = PRIMERA tarea de implementación** (dependencia de invitaciones + reset de contraseña; se experimentará bastante).

## 12. Paquetes / Precios (FINAL — decidido)
Cobro por organización. Gating de features según paquete (la app/servidor consulta el paquete de la org al iniciar sesión y habilita/deshabilita).

### Tabla de paquetes
| | **Básico** | **Premium** | **Ministerio** |
|---|---|---|---|
| **Precio (mensual)** | $12/mes | $25/mes | $50/mes |
| **Precio (anual, 2 meses gratis)** | $120/año (~$10/mes) | $250/año (~$20.83/mes) | $500/año (~$41.67/mes) |
| **Asientos** | 3 | 5 | 10 |
| **Almacenamiento** | 20 GB | 50 GB | 100 GB |
| **NeuralPlay** | Basic (2 salidas, estéreo, sin MIDI) | Premium (32 salidas + MIDI mapping + MIDI OUT) | Premium |
| **NeuralSync** (puente Logic) | ❌ | ✅ | ✅ |
| **Exportar PDF** (charts) | ❌ | ✅ | ✅ |
| **Charts + Setlists + Pads** | ✅ | ✅ | ✅ |

### Add-ons (extras)
| Extra | Precio | Costo p/nosotros | Margen |
|---|---|---|---|
| **+25 GB** | $5/mes | ~$2.50 | ~$2.50 (50%) |
| **+1 asiento (músico)** | $2/mes | ~$0 | ~$2 (casi 100%) |

### Manejo del espacio
- La **cuota** (20/50/100 GB) cuenta **TODO**: canciones + pistas + **tonos generados** (los tonos NO auto-expiran; lo que generan se queda).
- Se mide con `du` sobre `/mnt/ssd50gb/charts_data/orgs/<ID>/`.
- En el admin: **barra de uso** ("Usando X de Y GB"). Aviso al ~90%.
- Si se llena: **se bloquean subidas nuevas** (nunca se borra lo existente; sigue tocando).
- Para más espacio: comprar bloques **+25 GB ($5)** o subir de plan. El que genera muchos tonos → paga más espacio, natural.
- **No hay botón de borrar tonos** (evita que la gente borre para no pagar; y menos que construir).

### Economía por paquete (costo vs. margen) — con Spaces
| Plan | Ingresa | Costo aprox (almacen.+cómputo+banda+comisión) | **Margen** |
|---|---|---|---|
| Básico | $12 | ~$1.65 | **~86%** |
| Premium | $25 | ~$2.88 | **~88%** |
| Ministerio | $50 | ~$4.85 | **~90%** |
- **Clave del margen: el audio vive en DigitalOcean Spaces ($0.02/GiB/mes), 5× más barato que el volumen de bloque ($0.10/GiB).** Eso es lo que lleva el margen de ~65% a ~86-90%. Ver §14.
- Droplet desde $4; transferencia de Spaces $0.01/GiB (1 TiB incluido en el base de $5).
- Comisión de pago ~2.9%+$0.30 (puede ser mayor en Latam/internacional; el cobro anual la reduce).
- Aun con descuento anual (2 meses gratis) el margen se mantiene **>80%** en los tres planes.
- Los add-ons (sobre todo asientos, casi 100% margen) suben todavía más la utilidad.

### Notas comerciales
- Prueba de **7 días** en cualquier paquete.
- El Ministerio ($50) se puede subir más cuando se agregue un servicio nuevo que hoy no existe (para que el precio se sienta ganado, no forzado).
- **Cobro anual = "2 meses gratis"** (pagás 10, tenés 12; ~16.7% off). Mejora flujo de caja + reduce comisiones de pasarela. Empujarlo fuerte, sobre todo en Latam.
- Referencia de mercado: MultiTracks Playback ≈ $15/mes (solo playback); Planning Center ≈ $14+/mes. NeuralWorship queda competitivo/mejor.
- **Compresión**: si el usuario sube MP3/stems comprimidos, ahorra su propia cuota (es su decisión). Ojo producto: un solo MP3 estéreo pierde la mezcla multipista; lo ideal es stem por stem.

### Nota técnica de gating
El paquete de la org se guarda en `organizations`. Al iniciar sesión NeuralPlay/NeuralSync, el servidor devuelve el tier → la app **habilita/deshabilita** salidas, MIDI, NeuralSync y Exportar PDF. Debe validarse en servidor/app (no solo ocultar en UI).

---

## 13. Precio regional (PPP / Latam)
Cobrar distinto según país (como Spotify, Netflix, Canva). **Regla de oro: nunca bajar del piso que deja ~67%+ de margen** (con Spaces el costo es ~$1.65-4.85, así que hay mucho espacio).

### Tres niveles
| Plan | 🌎 Global | 🌮 Latam estándar¹ | 🆘 Económico² |
|---|---|---|---|
| Básico | $12 | $8 | $6 |
| Premium | $25 | $16 | $12 |
| Ministerio | $50 | $32 | $25 |
| *Margen Básico* | 86% | 79% | 72% |
| *Margen Premium* | 88% | 82% | 76% |
| *Margen Ministerio* | 90% | 85% | 81% |

¹ México, Colombia, Perú, Chile, Centroamérica, etc. (~35% off).
² Venezuela, Argentina, Bolivia, Nicaragua, Cuba (~50% off). Aun así 72-81% margen.

### El reto real en VE/AR = el PAGO, no el precio
- **Argentina**: el Impuesto PAIS se eliminó (no se cobra desde 2026), pero al pago con tarjeta le suman ~30% de retención (recuperable) + 21% IVA → el precio USD igual se le infla ~30-50%. Solución: **cobro anual** (un golpe al año) + procesador local (Mercado Pago / dLocal) cuando se pueda.
- **Venezuela**: tarjetas internacionales/PayPal casi no funcionan (controles de cambio). Allá lo digital se paga con **USDT (stablecoin)** — de facto la moneda. Para vender ahí: **aceptar USDT** o tener un representante local que cobre.

### Anti-abuso
- El descuento regional se activa por el **país del medio de pago / dirección de facturación**, NO por IP (evita VPN).
- Empujar cobro anual en Latam.

### Beca Ministerial (opcional)
- Descuento **discrecional manual** desde el súper-admin para congregaciones que de verdad no pueden. Da corazón sin abrir la puerta a todos.

---

## 14. Arquitectura de almacenamiento — DigitalOcean Spaces
**Decisión: mover el audio del volumen de bloque a Spaces (object storage, compatible S3).**

### Por qué
- **Costo**: $0.02/GiB/mes vs $0.10/GiB del volumen → **5× más barato**. Es lo que lleva el margen a 85-90%.
- **Rendimiento**: Spaces trae **CDN integrado** (sirve desde el borde, cerca del usuario). Descargas más rápidas, sobre todo en Latam.
- **Concurrencia**: clase S3 → aguanta **miles de descargas simultáneas**. El droplet ya NO sirve los bytes de audio.
- **Encaja con multi-tenant**: prefijos por org (`orgs/<ID>/…`), URLs firmadas, entrega por CDN.

### Separación de cargas (clave de la escala)
- **Audio (pesado, GB)** → Spaces/CDN. El droplet ni lo toca.
- **Charts/login/admin (liviano, KB)** → droplet (Flask/gunicorn).
- **Reproducción en vivo (domingo)** → NeuralPlay toca del **caché local del Mac**; NO depende del servidor ni del internet. Máxima fiabilidad (crítico para iglesias con mala conexión).

### Trabajo de ingeniería (pendiente)
- Subir/servir audio desde Spaces (S3 API), URLs firmadas, migrar la biblioteca actual del volumen a Spaces. Es re-arquitectura real, pero es la base correcta para vender.

---

## 15. Infraestructura para escala (plan 100 iglesias)
**Casi cero inversión de capital**: DO se paga mes a mes y escala solo. Se arranca con ~$30/mes y se sube capacidad conforme entran iglesias.

### Qué contratar (a 100 iglesias)
| Servicio | Para qué | Costo/mes |
|---|---|---|
| Droplet 4vCPU / **8GB** (Premium) | Servir charts, admin, API (pico domingo) | ~$48 |
| Spaces (~2 TB audio) | Guardar/servir pistas con CDN | ~$40 |
| Spaces — transferencia | Descargas sobre 1 TB incluido | ~$15 |
| Base administrada (Postgres 1GB)³ | Datos de orgs/usuarios, respaldos | ~$15 |
| Backups del droplet | Respaldo automático | ~$5 |
| Correo transaccional + varios | Invitaciones, reset de clave | ~$10 |
| **TOTAL** | | **~$135/mes** |

³ Opcional al inicio: seguir con SQLite (gratis) y pasar a base administrada al crecer. Sin ella ~$120/mes.

- Los 2 TB asumen ~20 GB promedio por iglesia (la cuota es el máximo, no todas la llenan). Solo se paga lo usado → el costo sube junto con el ingreso.
- **Concurrencia (domingo, 5 músicos × 100 = 500 conectados)**: el audio ya está cacheado local (tocan sin servidor); el server solo recibe login + cargar setlist (KB). 500 peticiones livianas = trivial para 8GB. Spaces aguanta las descargas.
- **8GB se agranda en 2 min sin migrar** si algún día aprieta. Load balancer + 2º droplet recién pasadas varias cientos de iglesias.

---

## 16. Modelo de negocio (OpEx, marketing, contratación)

### P&L a 100 iglesias (mezcla 60% Básico / 30% Premium / 10% Ministerio)
| Escenario | Ingreso/mes | Costos⁴ | Ganancia limpia/mes | Al año |
|---|---|---|---|---|
| 🌎 Global | ~$1,970 | ~$300 | **~$1,670** | ~$20,000 |
| 🌮 Latam estándar | ~$1,280 | ~$250 | **~$1,030** | ~$12,400 |
| 🆘 Económico | ~$970 | ~$200 | **~$770** | ~$9,200 |

⁴ Costos = infra (~$135) + comisiones (~$100-150) + herramientas/varios. (Sin empleados ni marketing pagado.)

### Costos operativos completos (con marketing/herramientas, sin empleados)
Infra ~$135 + comisiones ~$100-150 + marketing ~$100-300 (o casi $0 orgánico) + herramientas ~$40-60 + contabilidad/varios ~$50-100 → **~$450-750/mes**.

### Marketing
- **Ventaja injusta: red de iglesias existente (Mi Iglesia Internacional)** → primeras iglesias casi gratis, con confianza ganada. CAC bajo.
- Boca a boca + **programa de referidos** (1 mes gratis por referir) + videos demo + testimonios + alianzas con redes/conferencias de alabanza.
- Publicidad pagada opcional $100-300/mes; recuperar CAC en 3-6 meses. Iglesias = baja fuga (se quedan años) → ingreso se acumula.

### Etapas de contratación
- **~50 iglesias**: solo, orgánico. Prueba de concepto.
- **~100 iglesias**: solo + **ayudante medio tiempo** (soporte/onboarding, ~$300-500/mes; ideal un músico que conozca el sistema). Libera tu tiempo.
- **~300 iglesias** (~$3,800/mes): primer **empleado tiempo completo** (soporte/éxito del cliente).
- **~500+ iglesias** (~$6,400+/mes): equipito (soporte + desarrollo + alianzas/ventas).
- Clave: iglesias = bajo mantenimiento (se configuran una vez, usan semanal) → el soporte crece más lento que los clientes. La 1ª contratación es para quitarte el soporte de encima, no para vender.

### Fiabilidad (por qué no fallará seguido)
- Fallos catastróficos casi imposibles: el domingo en vivo corre del caché local (no depende del server/internet); Spaces no pierde archivos; base administrada con respaldos; aislamiento por org.
- Prácticas: respaldo antes de tocar producción, verificación de sintaxis, despliegue por etapas.
- Bugs chiquitos = normales al inicio; se arreglan rápido. Prueba de 7 días + buen onboarding dan confianza mientras el producto se pule.

---

## Estado de implementación (checkpoint 2026-09-05)
**Hecho y en producción (verificado):**
- **Correo** (Resend + Cloudflare Email Routing): envío `noreply@` + recepción `soporte@` → Gmail. Reset de contraseña conectado.
- **Fase 1**: tabla `organizations` + `org_id` en `usuarios`; org #1 = "Neural Worship" (token existente, dueño = admin id1, ministerio).
- **Fase 2 (aislamiento de almacenamiento)**:
  - Contexto de org: `org_actual()` (sesión o token) + `_cur_org()` (thread-local para hilos de render).
  - `config`/`setlists` por org (`archivo_config(org)`; org#1 → `config.json` legado).
  - `canciones` por org (`dir_canciones(org)`), `pistas` por org (`dir_pistas(_cur_org())`, 27 sitios), hilos `_render_tono`/`_asegurar_web`/`_asegurar_audio_admin` reciben `org` explícito.
  - Org #1 usa carpetas legado (sin mover 19 GB); orgs nuevas → `orgs/<id>/`.
  - **Aislamiento probado** con una org #2 de prueba (vio todo vacío; org #1 intacta). Org de prueba eliminada.
  - **Pads**: siguen **globales/compartidos** por ahora (sonidos genéricos) — decisión para reducir riesgo; aislar después si se decide.

**Pendiente (Fase 3 — onboarding, próximo):**
- Flujo "crear organización" (registro admin con nombre de org) que **cree el `orgs/<id>/config.json` con `live_token = organizations.token`** (evita el mismatch visto en la prueba).
- Cambiar `/api/sync/biblioteca` para usar `_token_ok()` (org-aware) en vez de comparar contra `config.live_token`.
- Invitaciones por correo (ya hay correo) + auto-registro con ID + aprobación + cerrar registro abierto.
- Login NeuralPlay/NeuralSync → token de la org. Suscripción/cobro + súper-admin.

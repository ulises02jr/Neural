/* ===== Modo Ensayo — reproductor de práctica multipista =====
   Familias en grilla (3 por fila, se abre una a la vez), siempre sigue el chart,
   botón en el footer del visor, empuja el chart hacia arriba. Tono por ?t=. */
(function () {
  const m = location.pathname.match(/\/cancion\/(\d+)/);
  if (!m) return;
  const numero = m[1];
  const isMobile = /iPhone|iPad|iPod|Android/i.test(navigator.userAgent);
  const tono = parseInt(new URLSearchParams(location.search).get('t') || '0', 10) || 0;
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const ORDEN = ['Voces','Guitarras','Teclados','Cuerdas','Metales','Bajo','Percusión','Guía','Música original','Otros','Click'];

  let ctx = null, stems = [], sources = [], duration = 0;
  let isPlaying = false, startTime = 0, pausedAt = 0, soloIndex = null, raf = null, loaded = false;
  let secciones = [], loopMode = false, loopRange = null, lastChartIdx = -1;
  let famActiva = null;

  const panel = document.createElement('div');
  panel.id = 'ensayo-panel';
  const tonoTxt = tono !== 0 ? (' · tono ' + (tono > 0 ? '+' : '') + tono) : '';
  panel.innerHTML =
    '<div class="ens-head">' +
      '<div class="ens-title">Modo ensayo<small>Pistas de esta canción' + tonoTxt + '</small></div>' +
      '<button class="ens-close" id="ens-close">×</button>' +
    '</div><div id="ens-body"><div class="ens-status">Cargando…</div></div>';
  document.body.appendChild(panel);

  const $ = s => panel.querySelector(s);
  const body = $('#ens-body');

  let abierto = false;
  document.addEventListener('click', e => {
    if (e.target && e.target.closest && e.target.closest('#ensayo-fab')) { toggle(); }
  });
  $('#ens-close').onclick = cerrar;

  function toggle(){ abierto ? cerrar() : abrir(); }
  async function abrir(){
    abierto = true; panel.classList.add('open'); document.body.classList.add('ensayo-abierto');
    ajustarAltura();
    if (!loaded) await cargar();
    ajustarAltura();
  }
  function cerrar(){ abierto = false; panel.classList.remove('open'); document.body.classList.remove('ensayo-abierto'); }
  function ajustarAltura(){ requestAnimationFrame(() => { document.documentElement.style.setProperty('--ensayo-h', panel.offsetHeight + 'px'); }); }

  function fmt(s){ s=Math.max(0,Math.floor(s||0)); return Math.floor(s/60)+':'+String(s%60).padStart(2,'0'); }
  function status(t){ body.innerHTML = '<div class="ens-status">'+t+'</div>'; ajustarAltura(); }

  function aMono(buf){
    if (buf.numberOfChannels <= 1) return buf;
    const mono = ctx.createBuffer(1, buf.length, buf.sampleRate);
    const out = mono.getChannelData(0), chs = buf.numberOfChannels;
    for (let c=0;c<chs;c++){ const d=buf.getChannelData(c); for (let i=0;i<d.length;i++) out[i]+=d[i]/chs; }
    return mono;
  }

  async function cargar() {
    status('Cargando…');
    let data;
    try { data = await (await fetch('/api/pistas/'+numero+'?t='+tono)).json(); }
    catch(e){ status('No pude cargar las pistas. Intentá de nuevo.'); return; }
    secciones = (data.secciones || []).slice().sort((a,b)=>a.t-b.t);

    if (!data.hay_pistas) { status('Esta canción todavía no tiene pistas cargadas. 🎵'); loaded = true; return; }

    if (!data.listo) {
      status('🎚️ Preparando el tono ' + (tono>0?'+':'') + tono + '…<br><small>La primera vez puede tardar unos minutos.</small>');
      try { await fetch('/api/pistas/'+numero+'/render/'+tono, { method:'POST' }); } catch(e){}
      let listo = false, vacios = 0;
      for (let i=0;i<240;i++){
        await sleep(2500);
        let e;
        try { e = await (await fetch('/api/pistas/'+numero+'/render/'+tono+'/estado')).json(); }
        catch(err){ continue; }
        if (e.listo){ listo = true; break; }
        if (!e.progreso) { vacios++; if (vacios >= 6){ vacios = 0; try { await fetch('/api/pistas/'+numero+'/render/'+tono, { method:'POST' }); } catch(x){} } }
        else vacios = 0;
        status('🎚️ Preparando el tono ' + (tono>0?'+':'') + tono + '… ' + (e.progreso||'') + '<br><small>Podés dejar esto abierto.</small>');
      }
      if (!listo){ status('El tono está tardando mucho. Recargá la página en un momento.'); return; }
      try { data = await (await fetch('/api/pistas/'+numero+'?t='+tono)).json(); }
      catch(e){ status('Error al cargar el tono.'); return; }
    }

    if (!data.stems || !data.stems.length){ status('No hay pistas para este tono.'); loaded = true; return; }

    status('Descargando pistas…');
    ctx = new (window.AudioContext || window.webkitAudioContext)(isMobile ? { sampleRate: 22050 } : {});
    try { if (navigator.audioSession) { navigator.audioSession.type = 'playback'; } } catch(e) {}
    let descargados = [];
    try {
      descargados = await Promise.all(data.stems.map(async st => {
        const resp = await fetch('/pista/'+numero+'/'+encodeURIComponent(st.file)+'?t='+tono);
        return { st: st, arr: await resp.arrayBuffer() };
      }));
    } catch(e) { status('Error descargando las pistas.'); return; }
    let hechas = 0;
    for (const d of descargados) {
      try {
        let audio = await ctx.decodeAudioData(d.arr);
        if (isMobile) audio = aMono(audio);
        const gain = ctx.createGain(); gain.gain.value = 1; gain.connect(ctx.destination);
        let nombre = d.st.name; if (nombre.length > 20) nombre = nombre.slice(0,20)+'…';
        stems.push({ name: nombre, buffer: audio, gain: gain, on: true, familia: d.st.familia || 'Otros' });
        duration = Math.max(duration, audio.duration);
      } catch(e) {}
      hechas++; status('Preparando pistas… ' + hechas + '/' + descargados.length);
    }
    loaded = true; render();
  }

  function render() {
    body.innerHTML =
      '<div class="ens-transport">' +
        '<span class="ens-time" id="ens-cur">0:00</span>' +
        '<input type="range" class="ens-seek" id="ens-seek" min="0" max="1000" value="0">' +
        '<span class="ens-time" id="ens-dur">'+fmt(duration)+'</span>' +
      '</div>' +
      '<div class="ens-secbar" id="ens-secbar"></div>' +
      '<div class="ens-transport" style="justify-content:center;">' +
        '<button class="ens-tbtn" id="ens-stop" title="Inicio">⏮</button>' +
        '<button class="ens-tbtn play" id="ens-play" title="Play/Pausa">▶</button>' +
        '<button class="ens-tbtn loop" id="ens-loop" title="Repetir sección">🔁</button>' +
      '</div>' +
      '<div id="ens-chips"></div>';
    $('#ens-play').onclick = togglePlay;
    $('#ens-stop').onclick = () => { pausedAt=0; loopRange = loopMode ? seccionEn(0) : null; if(isPlaying) startPlayback(0); else updateUI(); renderSecciones(); };
    $('#ens-loop').onclick = toggleLoop;
    $('#ens-seek').oninput = e => seekTo((e.target.value/1000)*duration);
    renderSecciones(); renderChips(); updateUI(); ajustarAltura();
  }

  // ---- Familias (botones 3 por fila, se abre una a la vez) ----
  function renderChips() {
    const c = $('#ens-chips'); if(!c) return;
    const grupos = {};
    stems.forEach((s,i)=>{ const f = s.familia || 'Otros'; (grupos[f]=grupos[f]||[]).push(i); });
    let fams = ORDEN.filter(f => grupos[f]);
    Object.keys(grupos).forEach(f => { if (fams.indexOf(f) === -1) fams.push(f); });
    if (famActiva && fams.indexOf(famActiva) === -1) famActiva = null;
    let html = '<div class="ens-fam-grid">';
    fams.forEach(f => {
      html += '<button class="ens-fam-b'+(f===famActiva?' active':'')+'" data-fam="'+f+'">'+f+'<span class="cnt">'+grupos[f].length+'</span></button>';
    });
    html += '</div><div class="ens-fam-chips" id="ens-fam-chips"></div>';
    c.innerHTML = html;
    c.querySelectorAll('.ens-fam-b').forEach(b => {
      b.onclick = () => { const f = b.getAttribute('data-fam'); famActiva = (famActiva===f)?null:f; renderChips(); };
    });
    const cont = $('#ens-fam-chips');
    if (famActiva && grupos[famActiva]) {
      grupos[famActiva].forEach(i => {
        const s = stems[i];
        const chip = document.createElement('div');
        chip.className = 'ens-chip'+(s.on?'':' off')+(soloIndex===i?' solo':'');
        chip.innerHTML = '<span class="ens-name">'+s.name+'</span><span class="s">S</span>';
        chip.querySelector('.ens-name').onclick = () => toggleStem(i);
        chip.querySelector('.s').onclick = (ev) => { ev.stopPropagation(); soloStem(i); };
        cont.appendChild(chip);
      });
    }
    ajustarAltura();
  }
  function toggleStem(i){ stems[i].on = !stems[i].on; soloIndex=null; applyGains(); renderChips(); }
  function soloStem(i){ soloIndex = (soloIndex===i? null : i); applyGains(); renderChips(); }
  function applyGains(){
    stems.forEach((s,i) => {
      const t = (soloIndex!==null) ? (i===soloIndex?1:0) : (s.on?1:0);
      s.gain.gain.setTargetAtTime(t, ctx.currentTime, 0.015);
    });
  }

  function renderSecciones() {
    const bar = $('#ens-secbar'); if (!bar) return;
    if (!secciones.length || !duration) { bar.style.display='none'; return; }
    bar.style.display='block'; bar.innerHTML='';
    secciones.forEach((sec,i) => {
      const start = sec.t, end = (i < secciones.length-1 ? secciones[i+1].t : duration);
      const seg = document.createElement('div');
      seg.className = 'ens-seg';
      seg.style.left = (start/duration*100)+'%';
      seg.style.width = ((end-start)/duration*100)+'%';
      seg.innerHTML = '<span>'+sec.nombre+'</span>';
      seg.onclick = () => { seekTo(start); if (loopMode) loopRange = [start,end]; renderSecciones(); };
      bar.appendChild(seg);
    });
    highlightSeg();
  }
  function highlightSeg() {
    const bar = $('#ens-secbar'); if (!bar || !bar.children.length) return;
    const p = pos();
    Array.prototype.forEach.call(bar.children, (seg,i) => {
      const start = secciones[i].t, end = (i < secciones.length-1 ? secciones[i+1].t : duration);
      seg.classList.toggle('cur', p>=start && p<end);
      seg.classList.toggle('loop', !!loopRange && Math.abs(loopRange[0]-start) < 0.02);
    });
  }
  function seccionEn(p) {
    if (!secciones.length) return null;
    for (let i=0;i<secciones.length;i++){
      const start = secciones[i].t, end = (i < secciones.length-1 ? secciones[i+1].t : duration);
      if (p>=start && p<end) return [start,end];
    }
    return [secciones[0].t, (secciones[1]?secciones[1].t:duration)];
  }
  function toggleLoop() {
    loopMode = !loopMode;
    loopRange = loopMode ? seccionEn(pos()) : null;
    const b = $('#ens-loop'); if (b) b.classList.toggle('on', loopMode);
    renderSecciones();
  }
  function syncAhora(){
    if (!secciones.length || typeof window.jump !== 'function') return;
    const p = pos();
    let cur = null;
    for (const s of secciones){ if (s.t <= p + 0.03) cur = s; else break; }
    if (cur && typeof cur.i === 'number' && cur.i !== lastChartIdx){
      lastChartIdx = cur.i;
      try { window.jump(cur.i); } catch(e) {}
    }
  }

  function startPlayback(offset){
    stopSources();
    startTime = ctx.currentTime - offset;
    sources = stems.map(s => {
      const src = ctx.createBufferSource();
      src.buffer = s.buffer; src.connect(s.gain);
      src.start(0, Math.min(offset, s.buffer.duration));
      return src;
    });
    isPlaying = true; $('#ens-play').textContent = '❚❚';
    applyGains(); tick();
  }
  function stopSources(){ sources.forEach(s=>{ try{s.stop();}catch(e){} }); sources=[]; }
  function pos(){ return isPlaying ? Math.min(ctx.currentTime-startTime, duration) : pausedAt; }

  function togglePlay(){
    if(!stems.length) return;
    if(ctx.state==='suspended') ctx.resume();
    if(isPlaying){ pausedAt=pos(); stopSources(); isPlaying=false; $('#ens-play').textContent='▶'; if(raf) cancelAnimationFrame(raf); }
    else { if(pausedAt>=duration) pausedAt=0; startPlayback(pausedAt); }
  }
  function seekTo(t){ pausedAt = Math.min(Math.max(0,t),duration); if(isPlaying) startPlayback(pausedAt); else updateUI(); syncAhora(); }

  function tick(){
    updateUI();
    if(loopRange && pos() >= loopRange[1]-0.03){ startPlayback(loopRange[0]); return; }
    if(pos()>=duration){ pausedAt=0; stopSources(); isPlaying=false; $('#ens-play').textContent='▶'; updateUI(); return; }
    raf = requestAnimationFrame(tick);
  }
  function updateUI(){
    const cur=$('#ens-cur'), sk=$('#ens-seek'); if(!cur) return;
    cur.textContent = fmt(pos());
    sk.value = duration ? (pos()/duration)*1000 : 0;
    highlightSeg(); syncAhora();
  }
})();

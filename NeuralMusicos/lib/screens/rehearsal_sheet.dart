import 'dart:async';
import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../audio_engine.dart';
// Acento del reproductor = mismo del visor (para respetar la linea visual).
const _acento = NW.chord; // gris-plata del visor
const _acentoSoft = NW.chordSoft;

const _orden = [
  'Voces', 'Guitarras', 'Teclados', 'Cuerdas', 'Metales',
  'Bajo', 'Percusión', 'Guía', 'Música original', 'Otros', 'Click'
];

void showRehearsal(BuildContext context, {required int numero, required int sem}) {
  showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    enableDrag: false,     // no se cierra al arrastrar el panel
    isDismissible: false,  // no se cierra al tocar afuera ni al desplazar; solo con la ×
    backgroundColor: NW.surface,
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(18))),
    builder: (_) => _RehearsalBody(numero: numero, sem: sem),
  );
}

class _Stem {
  final String id; // = file
  final String name;
  final String familia;
  bool on = true;
  _Stem(this.id, this.name, this.familia);
}

class _Sec {
  final double t;
  final String nombre;
  final int i; // indice de la seccion en el chart (para sincronizar)
  _Sec(this.t, this.nombre, this.i);
}

class _RehearsalBody extends StatefulWidget {
  final int numero;
  final int sem;
  const _RehearsalBody({required this.numero, required this.sem});

  @override
  State<_RehearsalBody> createState() => _RehearsalBodyState();
}

class _RehearsalBodyState extends State<_RehearsalBody> {
  bool _cargando = true;
  String? _status;
  bool _tonoNoListo = false;   // el tono pedido no esta renderizado -> ofrecer "Generar"
  bool _generando = false;     // render en curso (disparado desde la app)
  final List<_Stem> _stems = [];
  final List<_Sec> _secciones = [];
  String? _famActiva;
  int? _solo;

  bool _audioOk = false;
  double _dur = 0;
  double _pos = 0;
  bool _playing = false;
  bool _loop = false;
  List<double>? _loopRange; // [inicio, fin]
  Timer? _timer;
  int _cacheBytes = 0;
  double _dragAcc = 0;
  final ScrollController _secScroll = ScrollController(); // tira de secciones corrediza
  int _secScrollIdx = -1;                                 // última sección a la que se desplazó

  // Arreglo activo (índices de sección reales, con repeticiones). Vacío = canción completa.
  // Cuando hay arreglo, el AUDIO del ensayo salta de sección en sección siguiendo el orden.
  List<int> _arr = const [];
  int _arrStep = 0;
  bool get _arrOn => _arr.isNotEmpty;

  @override
  void initState() {
    super.initState();
    _cargar();
  }

  @override
  void dispose() {
    _timer?.cancel();
    _secScroll.dispose();
    // No detenemos el audio al cerrar el panel: sigue sonando (como en la web).
    // Se detiene al salir de la cancion (ChartScreen.dispose).
    super.dispose();
  }

  /// Dispara el render del tono en el servidor y hace polling del progreso.
  /// Al terminar, recarga las pistas (que ya quedan listas).
  Future<void> _generarTono() async {
    setState(() {
      _generando = true;
      _tonoNoListo = false;
      _status = '⏳ Generando el tono…';
    });
    final ok = await Api.I.pedirRenderTono(widget.numero, widget.sem);
    if (!mounted) return;
    if (!ok) {
      setState(() {
        _generando = false;
        _tonoNoListo = true;
        _status = 'No pude iniciar la generación del tono. Probá de nuevo.';
      });
      return;
    }
    // Polling cada 2.5 s hasta que el tono quede listo (o se agote el tiempo).
    const maxIntentos = 160; // ~6.5 min tope
    for (int i = 0; i < maxIntentos; i++) {
      await Future.delayed(const Duration(milliseconds: 2500));
      if (!mounted) return;
      final e = await Api.I.estadoRenderTono(widget.numero, widget.sem);
      if (!mounted) return;
      if (e == null) continue;
      if (e['listo'] == true) {
        await _cargar();   // ya está: carga las pistas y arranca el ensayo
        return;
      }
      final prog = (e['progreso'] ?? '').toString();
      setState(() {
        _status = prog.isNotEmpty ? '⏳ Generando el tono…  $prog' : '⏳ Generando el tono…';
      });
    }
    // Se tardó demasiado: dejar que reintente.
    if (!mounted) return;
    setState(() {
      _generando = false;
      _tonoNoListo = true;
      _status = 'El tono está tardando más de lo normal. Probá de nuevo en un momento.';
    });
  }

  // Familias que NO se transponen (van al tono original): Click, Guía, Batería, Percusión, Loops.
  bool _esFija(String fam) {
    final f = fam.toLowerCase().trim();
    return f.startsWith('click') || f.startsWith('gu') || f.startsWith('bater')
        || f.startsWith('percus') || f.startsWith('loop');
  }

  Future<void> _cargar() async {
    setState(() {
      _cargando = true;
      _status = null;
      _tonoNoListo = false;
    });
    // Pedimos el metadata (stems + secciones) en el TONO ORIGINAL: es independiente del tono,
    // y el audio se transpone en vivo en el dispositivo (ya no se renderizan tonos en el servidor).
    final j = await Api.I.pistas(widget.numero, 0);
    if (!mounted) return;
    if (j == null) {
      setState(() { _cargando = false; _status = 'No pude cargar las pistas. Intentá de nuevo.'; });
      return;
    }
    if (j['hay_pistas'] != true) {
      setState(() { _cargando = false; _status = 'Esta canción todavía no tiene pistas cargadas. 🎵'; });
      return;
    }
    if (j['listo'] != true) {
      final t = widget.sem;
      setState(() {
        _cargando = false;
        _tonoNoListo = true;
        _status = '🎚️ El tono ${t > 0 ? '+' : ''}$t todavía no está preparado.\n'
            'Podés generarlo ahora (tarda un momento) o abrir la canción en su tono original.';
      });
      return;
    }
    final stems = (j['stems'] as List? ?? []);
    final secs = (j['secciones'] as List? ?? []);
    _stems
      ..clear()
      ..addAll(stems.map((s) {
        final m = Map<String, dynamic>.from(s as Map);
        return _Stem((m['file'] ?? '').toString(), (m['name'] ?? '').toString(), (m['familia'] ?? 'Otros').toString());
      }));
    _secciones
      ..clear()
      ..addAll(secs
          .map((s) {
            final m = Map<String, dynamic>.from(s as Map);
            return _Sec(
              (m['t'] is num) ? (m['t'] as num).toDouble() : 0,
              (m['nombre'] ?? '').toString(),
              (m['i'] is num) ? (m['i'] as num).toInt() : -1,
            );
          })
          // Descartar secciones placeholder SIN NOMBRE (el metadata en tono original trae
          // un pickup/intro sin nombre al inicio). El chart no las muestra, así que si las
          // dejáramos la tira quedaría corrida una sección contra el chart.
          .where((sec) => sec.nombre.trim().isNotEmpty));
    _secciones.sort((a, b) => a.t.compareTo(b.t));
    // Marcadores para que el chart siga la musica (los lee ChartScreen).
    AudioEngine.I.loadedSecs =
        _secciones.where((s) => s.i >= 0).map((s) => [s.t, s.i.toDouble()]).toList();

    // Arreglo activo de la canción (si tiene): el ensayo seguirá ese orden.
    _arr = await Api.I.songArrangement(widget.numero);
    _arrStep = 0;
    AudioEngine.I.arrStep = _arrOn ? 0 : null;   // el chart resalta el paso exacto (o sync normal)
    if (!mounted) return;

    if (_stems.isEmpty) {
      setState(() { _cargando = false; _status = 'No hay pistas para este tono.'; });
      return;
    }

    // Si ya está cargada esta canción, reconectar sin re-descargar (sigue sonando) y solo
    // ajustar el tono en vivo (sin bajar nada de nuevo).
    if (AudioEngine.I.loadedFor(widget.numero, widget.sem)) {
      try {
        await AudioEngine.I.setPitch(widget.sem.toDouble());   // cambio de tono en vivo
        final pos = await AudioEngine.I.position();
        final playing = await AudioEngine.I.isPlaying();
        final bytes = await AudioEngine.I.cacheSize();
        if (!mounted) return;
        setState(() {
          _dur = AudioEngine.I.loadedDur;
          _pos = pos;
          _playing = playing;
          _audioOk = true;
          _cargando = false;
          _status = null;
          _cacheBytes = bytes;
        });
        _aplicarGanancias(); // el mixer vuelve a todas las pistas activas
        if (playing) _startTimer();
        return;
      } catch (_) {/* si falla, cae a recarga normal */}
    }

    // Cargar audio nativo.
    try {
      // Usar los proxys AAC (web/<base>.m4a): livianos como mp3 pero con indice de busqueda,
      // asi los saltos de seccion entran al instante (el mp3 decodificaba desde el principio).
      final list = _stems.map((s) {
        final base = s.id.split('/').last.replaceAll(RegExp(r'\.[^.]+$'), '');
        final proxy = 'web/$base.m4a';
        return <String, dynamic>{
          'id': s.id,
          'url': Api.I.stemDownloadUrl(widget.numero, proxy, 0),   // SIEMPRE el tono original
          'key': '${widget.numero}_t0_$base.m4a',
          'fixed': _esFija(s.familia),                             // Click/Guía/Batería/Percusión/Loops: no se transponen
        };
      }).toList();
      setState(() => _status = 'Descargando pistas…');
      final dur = await AudioEngine.I.load(list, numero: widget.numero, sem: widget.sem);
      await AudioEngine.I.setPitch(widget.sem.toDouble());   // transponer en vivo al tono pedido
      Api.I.marcarDescargada(widget.numero); // queda disponible offline
      if (!mounted) return;
      final bytes = await AudioEngine.I.cacheSize();
      if (!mounted) return;
      setState(() {
        _dur = dur;
        _audioOk = true;
        _cargando = false;
        _status = null;
        _cacheBytes = bytes;
      });
      // Con arreglo: posicionar el cursor en el inicio del primer paso.
      if (_arrOn) {
        _arrStep = 0;
        final r0 = _rangeReal(_arr[0]);
        _pos = r0[0];
        try { await AudioEngine.I.seek(r0[0]); } catch (_) {}
        if (mounted) setState(() {});
      }
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _cargando = false;
        _audioOk = false;
        _status = 'El audio todavía no está disponible en esta plataforma.\nLa interfaz sí funciona.';
      });
    }
  }

  void _startTimer() {
    _timer?.cancel();
    _timer = Timer.periodic(const Duration(milliseconds: 200), (_) async {
      if (!_playing) return;
      final p = await AudioEngine.I.position();
      if (!mounted) return;

      // ── Con arreglo: el audio salta de sección en sección en el orden del arreglo ──
      if (_arrOn) {
        final rg = _rangeReal(_arr[_arrStep.clamp(0, _arr.length - 1)]);
        if (_loop && _loopRange != null && p >= _loopRange![1] - 0.05) {
          await AudioEngine.I.seek(_loopRange![0]);
          setState(() => _pos = _loopRange![0]);
          return;
        }
        if (p >= rg[1] - 0.05) {   // terminó el tramo actual → siguiente paso del arreglo
          final next = _arrStep + 1;
          if (next >= _arr.length) {
            final r0 = _rangeReal(_arr[0]);
            if (_loop) {            // repetir todo el arreglo
              _arrStep = 0; _pubArrStep();
              await AudioEngine.I.seek(r0[0]);
              setState(() => _pos = r0[0]);
            } else {
              await AudioEngine.I.pause();
              _arrStep = 0; _pubArrStep();
              await AudioEngine.I.seek(r0[0]);
              setState(() { _playing = false; _pos = r0[0]; });
              _timer?.cancel();
            }
            return;
          }
          final prevEnd = rg[1];
          _arrStep = next; _pubArrStep();
          final rn = _rangeReal(_arr[_arrStep]);
          if ((rn[0] - prevEnd).abs() > 0.05) {
            await AudioEngine.I.seek(rn[0]);   // salto no contiguo → reposicionar (con fundido nativo)
            setState(() => _pos = rn[0]);
          } else {
            setState(() => _pos = p);          // contiguo: el audio sigue sin cortar (seamless)
          }
          return;
        }
        setState(() => _pos = p);
        return;
      }

      // ── Sin arreglo: canción completa (comportamiento original) ──
      if (_loop && _loopRange != null && p >= _loopRange![1] - 0.05) {
        await AudioEngine.I.seek(_loopRange![0]);
        setState(() => _pos = _loopRange![0]);
        return;
      }
      if (_dur > 0 && p >= _dur - 0.05) {
        await AudioEngine.I.pause();
        setState(() { _playing = false; _pos = 0; });
        _timer?.cancel();
        return;
      }
      setState(() => _pos = p);
    });
  }

  Future<void> _togglePlay() async {
    if (!_audioOk) return;
    if (_playing) {
      await AudioEngine.I.pause();
      setState(() => _playing = false);
      _timer?.cancel();
    } else {
      if (_arrOn) {
        final rg = _rangeReal(_arr[_arrStep.clamp(0, _arr.length - 1)]);
        // Si estamos fuera del tramo actual, arrancar al inicio de ese paso.
        if (_pos < rg[0] - 0.05 || _pos >= rg[1] - 0.05) {
          _pos = rg[0];
          await AudioEngine.I.seek(rg[0]);
        }
      } else {
        if (_pos >= _dur) _pos = 0;
      }
      await AudioEngine.I.play(_pos);
      setState(() => _playing = true);
      _startTimer();
    }
  }

  Future<void> _seek(double t) async {
    if (!_audioOk) return;
    await AudioEngine.I.seek(t);
    setState(() => _pos = t);
  }

  // Ir al inicio: con arreglo, al primer paso; sin arreglo, a 0.
  Future<void> _goStart() async {
    if (_arrOn) {
      _arrStep = 0;
      _pubArrStep();
      final r0 = _rangeReal(_arr[0]);
      await _seek(r0[0]);
    } else {
      await _seek(0);
    }
  }

  // Publica el paso exacto del arreglo para que el chart resalte la sección correcta
  // (aunque se repita). null = sin arreglo → el chart usa su sync normal.
  void _pubArrStep() { AudioEngine.I.arrStep = _arrOn ? _arrStep : null; }

  // Saltar al paso k del arreglo (desde la barra de secciones).
  Future<void> _irPaso(int k) async {
    if (!_arrOn || k < 0 || k >= _arr.length) return;
    _arrStep = k;
    _pubArrStep();
    final rg = _rangeReal(_arr[k]);
    if (_loop) _loopRange = rg;
    await _seek(rg[0]);
  }

  void _aplicarGanancias() {
    if (!_audioOk) return;
    for (var i = 0; i < _stems.length; i++) {
      final s = _stems[i];
      final v = (_solo != null) ? (i == _solo ? 1.0 : 0.0) : (s.on ? 1.0 : 0.0);
      AudioEngine.I.setVolume(s.id, v);
    }
  }

  List<double> _seccionEn(double p) {
    if (_secciones.isEmpty) return [0, _dur];
    for (var i = 0; i < _secciones.length; i++) {
      final start = _secciones[i].t;
      final end = i < _secciones.length - 1 ? _secciones[i + 1].t : _dur;
      if (p >= start && p < end) return [start, end];
    }
    return [_secciones.first.t, _secciones.length > 1 ? _secciones[1].t : _dur];
  }

  // ───────── Arreglo: rangos de audio y tiempo "display" (compactado) ─────────

  /// Rango de audio [inicio, fin] de la sección real [r] (índice de chart).
  List<double> _rangeReal(int r) {
    for (var i = 0; i < _secciones.length; i++) {
      if (_secciones[i].i == r) {
        final start = _secciones[i].t;
        final end = i < _secciones.length - 1 ? _secciones[i + 1].t : _dur;
        return [start, end];
      }
    }
    return [0, 0];
  }

  double _stepDur(int k) {
    if (k < 0 || k >= _arr.length) return 0;
    final rg = _rangeReal(_arr[k]);
    return (rg[1] - rg[0]).clamp(0, double.infinity).toDouble();
  }

  double get _arrTotal {
    var t = 0.0;
    for (var k = 0; k < _arr.length; k++) { t += _stepDur(k); }
    return t;
  }

  double _arrDispStart(int k) {
    var t = 0.0;
    for (var i = 0; i < k && i < _arr.length; i++) { t += _stepDur(i); }
    return t;
  }

  /// Posición en tiempo display (de la secuencia del arreglo) del cursor actual.
  double _arrDisplayNow() {
    if (!_arrOn) return _pos;
    final k = _arrStep.clamp(0, _arr.length - 1);
    final rg = _rangeReal(_arr[k]);
    final off = (_pos - rg[0]).clamp(0.0, (rg[1] - rg[0]).clamp(0.0, double.infinity));
    return _arrDispStart(k) + off;
  }

  /// Display → (paso, tiempo real de audio). Devuelve [real, paso].
  List<double> _arrRealFromDisplay(double dt) {
    var acc = 0.0;
    for (var k = 0; k < _arr.length; k++) {
      final d = _stepDur(k);
      if (dt < acc + d || k == _arr.length - 1) {
        final rg = _rangeReal(_arr[k]);
        return [rg[0] + (dt - acc).clamp(0.0, d), k.toDouble()];
      }
      acc += d;
    }
    return [0, 0];
  }

  int get _secActiva {
    if (_secciones.isEmpty) return 0;
    var cur = 0;
    for (var i = 0; i < _secciones.length; i++) {
      if (_secciones[i].t <= _pos + 0.03) { cur = i; } else { break; }
    }
    return cur;
  }

  String _fmt(double s) {
    if (s.isNaN || s < 0) s = 0;
    final m = (s ~/ 60), sec = (s % 60).floor();
    return '$m:${sec.toString().padLeft(2, '0')}';
  }

  Map<String, List<int>> get _grupos {
    final g = <String, List<int>>{};
    for (var i = 0; i < _stems.length; i++) {
      (g[_stems[i].familia] ??= []).add(i);
    }
    return g;
  }

  List<String> get _familias {
    final g = _grupos;
    final fams = _orden.where(g.containsKey).toList();
    for (final f in g.keys) {
      if (!fams.contains(f)) fams.add(f);
    }
    return fams;
  }

  @override
  Widget build(BuildContext context) {
    final maxH = MediaQuery.of(context).size.height * 0.72;
    return ConstrainedBox(
      constraints: BoxConstraints(maxHeight: maxH),
      child: Padding(
        padding: EdgeInsets.only(left: 16, right: 16, top: 12, bottom: 14 + MediaQuery.of(context).padding.bottom),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _handle(),
            _header(),
            const SizedBox(height: 8),
            Flexible(
              child: SingleChildScrollView(
                child: _cargando
                    ? _statusView(_status ?? 'Cargando…')
                    : (_status != null && !_audioOk)
                        ? _statusView(_status!)
                        : _contenido(),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _handle() {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: () => Navigator.of(context).pop(),
      onVerticalDragUpdate: (d) => _dragAcc += d.delta.dy,
      onVerticalDragEnd: (_) {
        if (_dragAcc > 45) Navigator.of(context).pop(); // jalar hacia abajo = cerrar
        _dragAcc = 0;
      },
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.only(top: 2, bottom: 8),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 40,
              height: 4,
              decoration: BoxDecoration(color: NW.line, borderRadius: BorderRadius.circular(2)),
            ),
            const Icon(Icons.keyboard_arrow_down, size: 20, color: NW.txt3),
          ],
        ),
      ),
    );
  }

  Widget _header() {
    final t = widget.sem;
    final tonoTxt = t != 0 ? ' · tono ${t > 0 ? '+' : ''}$t' : '';
    return Row(children: [
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('Modo ensayo', style: TextStyle(fontSize: 13, fontWeight: FontWeight.bold, letterSpacing: 0.3)),
          Text('Pistas de esta canción$tonoTxt', style: const TextStyle(fontSize: 11, color: NW.txt2)),
        ]),
      ),
      IconButton(icon: const Icon(Icons.close, color: NW.txt2), onPressed: () => Navigator.of(context).pop()),
    ]);
  }

  Widget _statusView(String t) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 24, horizontal: 10),
        child: Column(children: [
          Text(t, textAlign: TextAlign.center, style: const TextStyle(color: NW.txt2, fontSize: 13, height: 1.6)),
          if (_generando) ...[
            const SizedBox(height: 16),
            const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2.4)),
          ] else if (_tonoNoListo) ...[
            const SizedBox(height: 16),
            ElevatedButton.icon(
              onPressed: _generarTono,
              icon: const Icon(Icons.graphic_eq, size: 18),
              label: const Text('Generar este tono'),
            ),
          ],
          if (_status != null && _status!.contains('Intentá')) ...[
            const SizedBox(height: 14),
            OutlinedButton(onPressed: _cargar, child: const Text('Reintentar')),
          ],
        ]),
      );

  Widget _contenido() {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      _transport(),
      const SizedBox(height: 8),
      if (_secciones.isNotEmpty) _secbar(),
      const SizedBox(height: 10),
      _transportBtns(),
      const SizedBox(height: 14),
      _familiaGrid(),
      if (_famActiva != null) ...[const SizedBox(height: 10), _chips()],
      if (_cacheBytes > 0) ...[const SizedBox(height: 14), _cacheRow()],
    ]);
  }

  String _fmtMB(int bytes) {
    final mb = bytes / (1024 * 1024);
    if (mb < 1) return '${(bytes / 1024).round()} KB';
    return '${mb.toStringAsFixed(1)} MB';
  }

  Future<void> _liberar() async {
    await AudioEngine.I.clearCache();
      await Api.I.limpiarDescargas();
    if (!mounted) return;
    setState(() {
      _cacheBytes = 0;
      _playing = false;
      _pos = 0;
    });
    _timer?.cancel();
  }

  Widget _cacheRow() {
    return Row(
      children: [
        const Icon(Icons.download_done, size: 15, color: NW.txt3),
        const SizedBox(width: 6),
        Expanded(
          child: Text('Guardado offline · ${_fmtMB(_cacheBytes)}',
              style: const TextStyle(fontSize: 11, color: NW.txt3)),
        ),
        TextButton(
          onPressed: _liberar,
          style: TextButton.styleFrom(
            foregroundColor: NW.txt2,
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
            minimumSize: Size.zero,
            tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          ),
          child: const Text('Liberar espacio', style: TextStyle(fontSize: 12)),
        ),
      ],
    );
  }

  Widget _transport() {
    // Con arreglo, la barra avanza en tiempo "display" (la secuencia del arreglo).
    final double cur = _arrOn ? _arrDisplayNow() : _pos;
    final double total = _arrOn ? _arrTotal : _dur;
    return Row(children: [
      Text(_fmt(cur), style: const TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
      Expanded(
        child: SliderTheme(
          data: const SliderThemeData(
            trackHeight: 5,
            activeTrackColor: _acento,
            inactiveTrackColor: NW.line,
            thumbColor: _acento,
            thumbShape: RoundSliderThumbShape(enabledThumbRadius: 7),
            overlayShape: RoundSliderOverlayShape(overlayRadius: 14),
          ),
          child: Slider(
            value: total > 0 ? cur.clamp(0, total) : 0,
            max: total > 0 ? total : 1,
            onChanged: _audioOk
                ? (v) => setState(() {
                      if (_arrOn) {
                        final rr = _arrRealFromDisplay(v);
                        _pos = rr[0];
                        _arrStep = rr[1].toInt();
                        _pubArrStep();
                      } else {
                        _pos = v;
                      }
                      AudioEngine.I.scrubPos = _pos; // el chart sigue el arrastre en vivo
                    })
                : null,
            onChangeEnd: _audioOk
                ? (v) {
                    AudioEngine.I.scrubPos = null;
                    if (_arrOn) {
                      final rr = _arrRealFromDisplay(v);
                      _arrStep = rr[1].toInt();
                      _pubArrStep();
                      _seek(rr[0]);
                    } else {
                      _seek(v);
                    }
                  }
                : null,
          ),
        ),
      ),
      Text(_fmt(total), style: const TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
    ]);
  }

  String _nombreReal(int r) {
    for (final s in _secciones) { if (s.i == r) return s.nombre; }
    return '';
  }

  // Un chip de sección de ancho legible (ya no proporcional a la duración:
  // así no se apiñan cuando hay muchas). La tira entera se desplaza en horizontal.
  Widget _secChip(String texto, bool cur, bool loop, VoidCallback onTap) {
    final Color bg = loop ? _acento : (cur ? _acentoSoft : NW.raised);
    final Color fg = loop ? Colors.black : (cur ? NW.txt : NW.txt2);
    return Padding(
      padding: const EdgeInsets.only(right: 4),
      child: Material(
        color: bg,
        borderRadius: BorderRadius.circular(6),
        child: InkWell(
          borderRadius: BorderRadius.circular(6),
          onTap: onTap,
          child: Container(
            constraints: const BoxConstraints(minWidth: 46),
            padding: const EdgeInsets.symmetric(horizontal: 11),
            alignment: Alignment.center,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(6),
              border: Border.all(color: cur ? _acento : NW.line),
            ),
            child: Text(texto,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(fontSize: 11, fontWeight: cur ? FontWeight.w600 : FontWeight.w500, color: fg)),
          ),
        ),
      ),
    );
  }

  // Ancho aproximado de un chip (para seguir la sección activa al desplazar).
  double _anchoChip(String t) => (t.length * 7.0 + 26).clamp(46.0, 240.0);

  // Desplaza la tira para dejar visible la sección activa (con un poco de margen a la izquierda).
  void _seguirSeccion(List<String> labels, int activa) {
    if (!mounted || activa < 0 || activa == _secScrollIdx || !_secScroll.hasClients) return;
    _secScrollIdx = activa;
    try {
      double antes = 0;
      for (var i = 0; i < activa && i < labels.length; i++) { antes += _anchoChip(labels[i]) + 4; }
      final destino = (antes - 52).clamp(0.0, _secScroll.position.maxScrollExtent);
      _secScroll.animateTo(destino, duration: const Duration(milliseconds: 280), curve: Curves.easeOut);
    } catch (_) {/* si la tira aun no esta medida, no pasa nada */}
  }

  Widget _secbar() {
    final List<Widget> chips = [];
    final List<String> labels = [];
    int activa;

    if (_arrOn) {
      // Con arreglo: un chip por PASO del arreglo (con repeticiones), en su orden.
      activa = _arrStep;
      for (var k = 0; k < _arr.length; k++) {
        final nombre = _nombreReal(_arr[k]);
        labels.add(nombre);
        chips.add(_secChip(nombre, k == _arrStep, false, () => _irPaso(k)));
      }
    } else {
      // Sin arreglo: un chip por sección de la canción.
      activa = _secActiva;
      for (var i = 0; i < _secciones.length; i++) {
        final start = _secciones[i].t;
        final end = i < _secciones.length - 1 ? _secciones[i + 1].t : (_dur > 0 ? _dur : start + 1);
        final nombre = _secciones[i].nombre;
        final loop = _loopRange != null && (_loopRange![0] - start).abs() < 0.02;
        labels.add(nombre);
        chips.add(_secChip(nombre, i == activa, loop, () {
          _seek(start);
          if (_loop) setState(() => _loopRange = [start, end]);
        }));
      }
    }

    // Tras pintar, deja visible la sección activa deslizando la tira.
    WidgetsBinding.instance.addPostFrameCallback((_) => _seguirSeccion(labels, activa));

    return SizedBox(
      height: 30,
      child: ListView(
        controller: _secScroll,
        scrollDirection: Axis.horizontal,
        physics: const BouncingScrollPhysics(),
        padding: const EdgeInsets.symmetric(vertical: 1),
        children: chips,
      ),
    );
  }

  Widget _transportBtns() {
    return Row(mainAxisAlignment: MainAxisAlignment.center, children: [
      _tbtn(Icons.skip_previous_rounded, false, false, _goStart),
      const SizedBox(width: 12),
      _tbtn(_playing ? Icons.pause_rounded : Icons.play_arrow_rounded, true, false, _togglePlay),
      const SizedBox(width: 12),
      _tbtn(Icons.repeat_rounded, false, _loop, () {
        setState(() {
          _loop = !_loop;
          _loopRange = _loop ? _seccionEn(_pos) : null;
        });
      }),
    ]);
  }

  Widget _tbtn(IconData icono, bool play, bool on, VoidCallback onTap) {
    final size = play ? 50.0 : 42.0;
    final activo = play || on;
    return Material(
      color: activo ? _acento : NW.raised,
      shape: const CircleBorder(),
      child: InkWell(
        customBorder: const CircleBorder(),
        onTap: onTap,
        child: Container(
          width: size,
          height: size,
          alignment: Alignment.center,
          decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: activo ? _acento : NW.line)),
          child: Icon(icono, color: activo ? Colors.black : NW.txt, size: play ? 26 : 22),
        ),
      ),
    );
  }

  Widget _familiaGrid() {
    final fams = _familias;
    final g = _grupos;
    return GridView.count(
      crossAxisCount: 3,
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      mainAxisSpacing: 7,
      crossAxisSpacing: 7,
      childAspectRatio: 2.4,
      children: fams.map((f) {
        final activa = f == _famActiva;
        return Material(
          color: activa ? _acentoSoft : NW.raised,
          borderRadius: BorderRadius.circular(9),
          child: InkWell(
            borderRadius: BorderRadius.circular(9),
            onTap: () => setState(() => _famActiva = activa ? null : f),
            child: Container(
              alignment: Alignment.center,
              decoration: BoxDecoration(
                border: Border.all(color: activa ? _acento : NW.line),
                borderRadius: BorderRadius.circular(9),
              ),
              child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                Text(f, textAlign: TextAlign.center, style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: NW.txt)),
                Text('${g[f]!.length}', style: const TextStyle(fontSize: 10, color: NW.txt2)),
              ]),
            ),
          ),
        );
      }).toList(),
    );
  }

  Widget _chips() {
    final idxs = _grupos[_famActiva] ?? [];
    return Wrap(
      spacing: 7,
      runSpacing: 7,
      children: idxs.map((i) {
        final s = _stems[i];
        final solo = _solo == i;
        final on = s.on;
        return Container(
          padding: const EdgeInsets.fromLTRB(12, 6, 7, 6),
          decoration: BoxDecoration(
            color: on ? _acentoSoft : Colors.transparent,
            borderRadius: BorderRadius.circular(999),
            border: Border.all(color: on ? _acento : NW.line, width: 1.5),
          ),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            GestureDetector(
              onTap: () {
                setState(() { s.on = !s.on; _solo = null; });
                _aplicarGanancias();
              },
              child: Text(s.name, style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: on ? NW.txt : NW.txt2)),
            ),
            const SizedBox(width: 6),
            GestureDetector(
              onTap: () {
                setState(() => _solo = solo ? null : i);
                _aplicarGanancias();
              },
              child: Container(
                width: 19,
                height: 19,
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: solo ? _acento : NW.bg,
                  border: Border.all(color: solo ? _acento : NW.line),
                ),
                child: Text('S', style: TextStyle(fontSize: 9, fontWeight: FontWeight.bold, color: solo ? Colors.black : NW.txt2)),
              ),
            ),
          ]),
        );
      }).toList(),
    );
  }
}

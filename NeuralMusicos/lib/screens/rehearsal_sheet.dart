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

  @override
  void initState() {
    super.initState();
    _cargar();
  }

  @override
  void dispose() {
    _timer?.cancel();
    // No detenemos el audio al cerrar el panel: sigue sonando (como en la web).
    // Se detiene al salir de la cancion (ChartScreen.dispose).
    super.dispose();
  }

  Future<void> _cargar() async {
    setState(() {
      _cargando = true;
      _status = null;
    });
    final j = await Api.I.pistas(widget.numero, widget.sem);
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
        _status = '🎚️ El tono ${t > 0 ? '+' : ''}$t todavía no está preparado.\n'
            'Abrí la canción en su tono original o esperá a que se genere.';
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
      ..addAll(secs.map((s) {
        final m = Map<String, dynamic>.from(s as Map);
        return _Sec(
          (m['t'] is num) ? (m['t'] as num).toDouble() : 0,
          (m['nombre'] ?? '').toString(),
          (m['i'] is num) ? (m['i'] as num).toInt() : -1,
        );
      }));
    _secciones.sort((a, b) => a.t.compareTo(b.t));
    // Marcadores para que el chart siga la musica (los lee ChartScreen).
    AudioEngine.I.loadedSecs =
        _secciones.where((s) => s.i >= 0).map((s) => [s.t, s.i.toDouble()]).toList();

    if (_stems.isEmpty) {
      setState(() { _cargando = false; _status = 'No hay pistas para este tono.'; });
      return;
    }

    // Si ya esta cargada esta cancion+tono, reconectar sin re-descargar (sigue sonando).
    if (AudioEngine.I.loadedFor(widget.numero, widget.sem)) {
      try {
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
      // Usar los proxys MP3 (web/<base>.mp3): ~decenas de veces mas livianos que los WAV.
      final list = _stems.map((s) {
        final base = s.id.split('/').last.replaceAll(RegExp(r'\.[^.]+$'), '');
        final proxy = 'web/$base.mp3';
        return {
          'id': s.id,
          'url': Api.I.stemDownloadUrl(widget.numero, proxy, widget.sem),
          'key': '${widget.numero}_t${widget.sem}_$base.mp3',
        };
      }).toList();
      setState(() => _status = 'Descargando pistas…');
      final dur = await AudioEngine.I.load(list, numero: widget.numero, sem: widget.sem);
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
      if (_pos >= _dur) _pos = 0;
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
    return Row(children: [
      Text(_fmt(_pos), style: const TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
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
            value: _dur > 0 ? _pos.clamp(0, _dur) : 0,
            max: _dur > 0 ? _dur : 1,
            onChanged: _audioOk ? (v) => setState(() => _pos = v) : null,
            onChangeEnd: _audioOk ? _seek : null,
          ),
        ),
      ),
      Text(_fmt(_dur), style: const TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
    ]);
  }

  Widget _secbar() {
    final activa = _secActiva;
    return Container(
      height: 26,
      decoration: BoxDecoration(color: NW.bg, borderRadius: BorderRadius.circular(6)),
      clipBehavior: Clip.antiAlias,
      child: Row(
        children: List.generate(_secciones.length, (i) {
          final start = _secciones[i].t;
          final end = i < _secciones.length - 1 ? _secciones[i + 1].t : (_dur > 0 ? _dur : start + 1);
          final flex = ((end - start) * 1000).round().clamp(1, 1 << 30);
          final cur = i == activa;
          final loop = _loopRange != null && (_loopRange![0] - start).abs() < 0.02;
          return Expanded(
            flex: flex,
            child: GestureDetector(
              onTap: () {
                _seek(start);
                if (_loop) setState(() => _loopRange = [start, end]);
              },
              child: Container(
                decoration: BoxDecoration(
                  color: loop ? _acento : (cur ? _acentoSoft : NW.raised),
                  border: Border(left: BorderSide(color: i == 0 ? Colors.transparent : NW.line)),
                ),
                alignment: Alignment.centerLeft,
                padding: const EdgeInsets.only(left: 4),
                child: Text(_secciones[i].nombre,
                    maxLines: 1,
                    overflow: TextOverflow.clip,
                    style: TextStyle(fontSize: 9, color: loop ? Colors.black : (cur ? NW.txt : NW.txt2))),
              ),
            ),
          );
        }),
      ),
    );
  }

  Widget _transportBtns() {
    return Row(mainAxisAlignment: MainAxisAlignment.center, children: [
      _tbtn('⏮', false, false, () => _seek(0)),
      const SizedBox(width: 12),
      _tbtn(_playing ? '❚❚' : '▶', true, false, _togglePlay),
      const SizedBox(width: 12),
      _tbtn('🔁', false, _loop, () {
        setState(() {
          _loop = !_loop;
          _loopRange = _loop ? _seccionEn(_pos) : null;
        });
      }),
    ]);
  }

  Widget _tbtn(String glifo, bool play, bool on, VoidCallback onTap) {
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
          child: Text(glifo, style: TextStyle(color: activo ? Colors.black : NW.txt, fontSize: play ? 20 : 16)),
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

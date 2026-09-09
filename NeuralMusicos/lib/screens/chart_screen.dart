import 'dart:async';
import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../models.dart';
import 'rehearsal_sheet.dart';
import '../audio_engine.dart';

const _nombres = ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B'];

/// Opciones de color de acordes (igual que la web). null = por defecto (plata).
const _coloresAcorde = <String, Color?>{
  'Por defecto': null,
  'Blanco': Color(0xFFFFFFFF),
  'Amarillo': Color(0xFFFFD23F),
  'Celeste': Color(0xFF8FD3FF),
  'Verde': Color(0xFF8FE0A0),
  'Naranja': Color(0xFFFF9D5C),
};

class ChartScreen extends StatefulWidget {
  final int numero;
  final int semInicial;
  final String tonoBase;

  const ChartScreen({
    super.key,
    required this.numero,
    required this.semInicial,
    required this.tonoBase,
  });

  @override
  State<ChartScreen> createState() => _ChartScreenState();
}

class _ChartScreenState extends State<ChartScreen> {
  late int _sem;
  int _origBase = 0;
  bool _menor = false;

  Chart? _chart;
  bool _cargando = true;
  bool _error = false;

  int _idx = 0;
  int _nivel = 2;
  final _scroll = ScrollController();
  final List<GlobalKey> _keys = [];
  bool _scrollProgramatico = false;
  Timer? _syncTimer;    // sigue la musica del modo ensayo
  int _syncIdx = -1;
  bool _audioPlaying = false;
  double _lastSyncPos = -1;

  // Preferencias de vista (como la web)
  bool _claro = false; // tema dia
  String _modo = 'ambos'; // ambos | acordes | letra
  Color? _colorAcorde; // null = plata por defecto
  String _grosor = 'normal'; // fino | normal | grueso

  static const _lyricSizes = [14.0, 16.0, 18.0, 22.0, 26.0];
  static const _chordSizes = [13.0, 15.0, 17.0, 20.0, 24.0];
  static const _nivelLbls = ['XS', 'S', 'M', 'L', 'XL'];

  double get _lyricSize => _lyricSizes[_nivel];
  double get _chordSize => _chordSizes[_nivel];

  bool get _showChord => _modo != 'letra';
  bool get _showLyric => _modo != 'acordes';
  Color get _chordColor => _colorAcorde ?? NW.chord;
  FontWeight get _chordWeight =>
      _grosor == 'fino' ? FontWeight.w400 : (_grosor == 'grueso' ? FontWeight.w800 : FontWeight.w700);

  // Paleta segun tema (dia/noche)
  Color get _cBg => _claro ? const Color(0xFFF4F4F6) : NW.bg;
  Color get _cSurface => _claro ? Colors.white : NW.surface;
  Color get _cRaised => _claro ? const Color(0xFFECECEF) : NW.raised;
  Color get _cLine => _claro ? const Color(0xFFDCDCE2) : NW.line;
  Color get _cTxt => _claro ? const Color(0xFF15151A) : NW.txt;
  Color get _cTxt2 => _claro ? const Color(0xFF5B5B66) : NW.txt2;
  Color get _cTxt3 => _claro ? const Color(0xFF8A8A95) : NW.txt3;

  @override
  void initState() {
    super.initState();
    _sem = widget.semInicial;
    _parseTono(widget.tonoBase);
    _cargar();
    // El chart sigue la musica del ensayo mientras suena (panel abierto o cerrado).
    _syncTimer = Timer.periodic(const Duration(milliseconds: 150), (_) => _tickSync());
  }

  @override
  void dispose() {
    _syncTimer?.cancel();
    _scroll.dispose();
    AudioEngine.I.stop(); // al salir de la cancion, detener el audio del ensayo
    super.dispose();
  }

  /// Fuente de verdad = el audio: mueve el chart a la seccion que suena
  /// (mientras reproduce O mientras se mueve la barra, aunque este en pausa).
  bool _syncBusy = false;
  Future<void> _tickSync() async {
    if (_syncBusy) return;
    final ae = AudioEngine.I;
    if (ae.loadedNumero != widget.numero || ae.loadedSecs.isEmpty) {
      if (_audioPlaying && mounted) setState(() => _audioPlaying = false);
      return;
    }
    _syncBusy = true;
    late final bool playing;
    late final double p;
    try {
      playing = await ae.isPlaying();
      p = await ae.position();
    } finally {
      _syncBusy = false;
    }
    if (!mounted) return;
    // "Se mueve" = reproduciendo o la posicion cambio (el usuario esta desplazando).
    final moving = playing || (p - _lastSyncPos).abs() > 0.05;
    _lastSyncPos = p;
    if (moving != _audioPlaying) setState(() => _audioPlaying = moving);
    if (!moving) return; // en pausa quieto: no estorbar el scroll manual
    int cur = -1;
    for (final m in ae.loadedSecs) {
      if (m[0] <= p + 0.03) { cur = m[1].toInt(); } else { break; }
    }
    final n = _chart?.secciones.length ?? 0;
    if (cur >= 0 && cur < n && cur != _syncIdx) {
      _syncIdx = cur;
      _jump(cur, animate: false); // instantáneo, como NeuralPlay
    }
  }

  void _parseTono(String t) {
    final m = RegExp(r'^([A-Ga-g])([#b]?)(.*)$').firstMatch(t.trim());
    if (m == null) {
      _origBase = 0;
      _menor = false;
      return;
    }
    final nat = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}[m.group(1)!.toUpperCase()]!;
    var base = nat + (m.group(2) == '#' ? 1 : (m.group(2) == 'b' ? -1 : 0));
    _origBase = ((base % 12) + 12) % 12;
    _menor = RegExp(r'^m(?!aj)', caseSensitive: false).hasMatch(m.group(3) ?? '');
  }

  String _nombreTono(int semis) {
    final i = ((_origBase + semis) % 12 + 12) % 12;
    return _nombres[i] + (_menor ? 'm' : '');
  }

  int _normDelta(int d) {
    d = ((d % 12) + 12) % 12;
    if (d > 6) d -= 12;
    return d;
  }

  Future<void> _cargar() async {
    setState(() {
      _cargando = true;
      _error = false;
    });
    final c = await Api.I.chart(widget.numero, _sem);
    if (!mounted) return;
    setState(() {
      _chart = c;
      _error = c == null;
      _cargando = false;
      if (c != null) {
        _keys
          ..clear()
          ..addAll(List.generate(c.secciones.length, (_) => GlobalKey()));
        if (_idx >= c.secciones.length) _idx = 0;
      }
    });
  }

  void _irTono(int semAbs) {
    Navigator.of(context).pop();
    if (semAbs == _sem) return;
    setState(() => _sem = semAbs);
    _cargar();
  }

  void _jump(int i, {bool animate = true}) {
    final c = _chart;
    if (c == null || i < 0 || i >= c.secciones.length) return;
    setState(() => _idx = i);
    final ctx = _keys[i].currentContext;
    if (ctx == null) return;
    if (animate) {
      _scrollProgramatico = true;
      Scrollable.ensureVisible(ctx,
              duration: const Duration(milliseconds: 250), alignment: 0.0, curve: Curves.easeInOut)
          .then((_) => _scrollProgramatico = false);
    } else {
      // Instantáneo (como NeuralPlay: scrollTop = offset), sin animación que tiemble.
      _scrollProgramatico = true;
      Scrollable.ensureVisible(ctx, duration: Duration.zero, alignment: 0.0);
      WidgetsBinding.instance.addPostFrameCallback((_) => _scrollProgramatico = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = _chart;
    return Scaffold(
      backgroundColor: _cBg,
      body: SafeArea(
        child: _cargando
            ? const Center(child: CircularProgressIndicator(color: NW.gold))
            : _error || c == null
                ? _errorView()
                : Padding(
                    padding: const EdgeInsets.fromLTRB(14, 10, 14, 8),
                    child: Column(
                      children: [
                        _topbar(c),
                        const SizedBox(height: 10),
                        _timeline(c),
                        const SizedBox(height: 6),
                        Expanded(child: _stage(c)),
                        const SizedBox(height: 8),
                        _footer(c),
                      ],
                    ),
                  ),
      ),
    );
  }

  Widget _errorView() => Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Text('No se pudo cargar el chart', style: TextStyle(color: _cTxt2)),
          const SizedBox(height: 12),
          OutlinedButton(onPressed: _cargar, child: const Text('Reintentar')),
        ]),
      );

  Widget _topbar(Chart c) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Wrap(
                crossAxisAlignment: WrapCrossAlignment.center,
                spacing: 8,
                runSpacing: 4,
                children: [
                  Text(c.titulo, style: TextStyle(fontSize: 17, fontWeight: FontWeight.w600, color: _cTxt)),
                  _tonoChip(c),
                ],
              ),
              const SizedBox(height: 3),
              Text(
                [
                  if (c.tempo != null && c.tempo! > 0) '${c.tempo} BPM',
                  if (c.compas.isNotEmpty) c.compas,
                ].join(' · '),
                style: TextStyle(fontSize: 12, color: _cTxt2),
              ),
            ],
          ),
        ),
        const SizedBox(width: 10),
        Column(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            _dotsBtn(),
            const SizedBox(height: 6),
            Text('${_idx + 1} / ${c.secciones.length}', style: TextStyle(fontSize: 12, color: _cTxt2)),
          ],
        ),
      ],
    );
  }

  Widget _tonoChip(Chart c) {
    return Material(
      color: NW.chord,
      borderRadius: BorderRadius.circular(6),
      child: InkWell(
        borderRadius: BorderRadius.circular(6),
        onTap: _abrirTono,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 11, vertical: 4),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            Text(_nombreTono(_sem),
                style: const TextStyle(
                    color: Colors.black, fontWeight: FontWeight.bold, fontSize: 14, fontFamily: NW.mono)),
            const Text(' ▾', style: TextStyle(color: Colors.black54, fontSize: 10, fontWeight: FontWeight.bold)),
          ]),
        ),
      ),
    );
  }

  Widget _dotsBtn() {
    return Material(
      color: _cSurface,
      borderRadius: BorderRadius.circular(7),
      child: InkWell(
        borderRadius: BorderRadius.circular(7),
        onTap: _abrirAjustes,
        child: Container(
          width: 34,
          height: 30,
          alignment: Alignment.center,
          decoration: BoxDecoration(border: Border.all(color: _cLine), borderRadius: BorderRadius.circular(7)),
          child: Text('⋯', style: TextStyle(color: _cTxt2, fontSize: 18)),
        ),
      ),
    );
  }

  Widget _timeline(Chart c) {
    return SizedBox(
      height: 34,
      child: ListView.separated(
        scrollDirection: Axis.horizontal,
        itemCount: c.secciones.length,
        separatorBuilder: (_, __) => const SizedBox(width: 6),
        itemBuilder: (_, i) {
          final activo = i == _idx;
          final done = i < _idx;
          final bg = activo ? (_claro ? NW.chord : NW.txt) : _cSurface;
          return Material(
            color: bg,
            borderRadius: BorderRadius.circular(7),
            child: InkWell(
              borderRadius: BorderRadius.circular(7),
              onTap: () => _jump(i),
              child: Container(
                alignment: Alignment.center,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                decoration: BoxDecoration(
                  border: Border.all(color: activo ? bg : _cLine),
                  borderRadius: BorderRadius.circular(7),
                ),
                child: Text(c.secciones[i].tipo,
                    style: TextStyle(
                        fontSize: 13,
                        color: activo ? Colors.black : (done ? _cTxt3 : _cTxt2),
                        fontWeight: activo ? FontWeight.w600 : FontWeight.normal)),
              ),
            ),
          );
        },
      ),
    );
  }

  Widget _stage(Chart c) {
    return NotificationListener<ScrollNotification>(
      onNotification: (n) {
        if (!_scrollProgramatico && n is ScrollUpdateNotification) _detectarActiva();
        return false;
      },
      // ListView normal (no .builder): arma TODAS las secciones para poder saltar
      // a cualquiera al instante, como NeuralPlay (que tiene el chart completo en el DOM).
      child: ListView(
        controller: _scroll,
        padding: const EdgeInsets.only(top: 2, bottom: 4),
        children: [
          for (var i = 0; i < c.secciones.length; i++) _seccionCard(c.secciones[i], i),
        ],
      ),
    );
  }

  void _detectarActiva() {
    if (_audioPlaying) return; // mientras suena, manda el audio (no el scroll)
    int cur = _idx;
    double mejor = double.infinity;
    for (var i = 0; i < _keys.length; i++) {
      final ctx = _keys[i].currentContext;
      if (ctx == null) continue;
      final box = ctx.findRenderObject() as RenderBox?;
      if (box == null) continue;
      final dy = box.localToGlobal(Offset.zero).dy;
      if (dy <= 220 && (220 - dy) < mejor) {
        mejor = 220 - dy;
        cur = i;
      }
    }
    if (cur != _idx) setState(() => _idx = cur);
  }

  Widget _seccionCard(ChartSection s, int i) {
    final activo = i == _idx;
    return Container(
      key: _keys[i],
      margin: const EdgeInsets.only(bottom: 12),
      padding: const EdgeInsets.fromLTRB(16, 14, 16, 16),
      decoration: BoxDecoration(
        color: _cSurface,
        border: Border.all(color: activo ? NW.chord : _cLine),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 7),
            decoration: BoxDecoration(
              color: _cRaised,
              border: Border(left: BorderSide(color: activo ? NW.chord : _cLine, width: 3)),
              borderRadius: BorderRadius.circular(7),
            ),
            child: Text(s.tipo.toUpperCase(),
                style: TextStyle(
                    fontSize: 11,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 1.8,
                    color: activo ? NW.chord : _cTxt3)),
          ),
          if (s.nota != null && s.nota!.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 10),
              child: Text(s.nota!, style: TextStyle(fontSize: 13, color: _cTxt2, fontStyle: FontStyle.italic)),
            ),
          const SizedBox(height: 10),
          if (s.inst) _instrumental(s.prog) else ...s.lines.map(_linea),
        ],
      ),
    );
  }

  Widget _instrumental(List<String> prog) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Text('Instrumental', style: TextStyle(fontSize: 15, color: _cTxt2)),
        ),
        Wrap(spacing: 10, runSpacing: 10, children: prog.map(_chip).toList()),
      ],
    );
  }

  Widget _chip(String acorde) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color: NW.chordSoft,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: NW.chipBorder),
      ),
      child: Text(acorde,
          style: TextStyle(color: _chordColor, fontWeight: _chordWeight, fontFamily: NW.mono, fontSize: _chordSize)),
    );
  }

  Widget _linea(List<ChartSeg> segs) {
    final soloAcordes = segs.isNotEmpty && segs.every((t) => t.text.trim().isEmpty);
    if (soloAcordes) {
      return Padding(
        padding: const EdgeInsets.only(top: 2, bottom: 12),
        child: Wrap(
          spacing: 9,
          runSpacing: 9,
          children: segs.where((t) => t.chord.trim().isNotEmpty).map((t) => _chip(t.chord)).toList(),
        ),
      );
    }
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Wrap(
        crossAxisAlignment: WrapCrossAlignment.end,
        children: segs.map((seg) {
          return Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              if (_showChord)
                Text(seg.chord.isEmpty ? ' ' : seg.chord,
                    style: TextStyle(
                        color: _chordColor,
                        fontWeight: _chordWeight,
                        fontFamily: NW.mono,
                        fontSize: _chordSize,
                        height: 1.1)),
              if (_showLyric)
                Text(seg.text.isEmpty ? ' ' : seg.text,
                    style: TextStyle(color: _cTxt, fontSize: _lyricSize, height: 1.35)),
            ],
          );
        }).toList(),
      ),
    );
  }

  Widget _footer(Chart c) {
    return Row(
      children: [
        _iconBtn('‹', _idx == 0 ? null : () => _jump(_idx - 1)),
        const SizedBox(width: 6),
        Expanded(
          child: Material(
            color: _cRaised,
            borderRadius: BorderRadius.circular(8),
            child: InkWell(
              borderRadius: BorderRadius.circular(8),
              onTap: () => Navigator.of(context).pop(),
              child: Container(
                height: 46,
                alignment: Alignment.center,
                decoration: BoxDecoration(border: Border.all(color: _cLine), borderRadius: BorderRadius.circular(8)),
                child: Text('← Atrás', style: TextStyle(color: _cTxt, fontSize: 12.5, fontWeight: FontWeight.w500)),
              ),
            ),
          ),
        ),
        const SizedBox(width: 6),
        _iconBtn('›', _idx >= c.secciones.length - 1 ? null : () => _jump(_idx + 1)),
        const SizedBox(width: 6),
        _ensayoBtn(),
      ],
    );
  }

  Widget _ensayoBtn() {
    return Material(
      color: NW.goldSoft,
      borderRadius: BorderRadius.circular(8),
      child: InkWell(
        borderRadius: BorderRadius.circular(8),
        onTap: () => showRehearsal(context, numero: widget.numero, sem: _sem),
        child: Container(
          width: 44,
          height: 46,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            border: Border.all(color: NW.chipBorder),
            borderRadius: BorderRadius.circular(8),
          ),
          child: const Text('🎧', style: TextStyle(fontSize: 19)),
        ),
      ),
    );
  }

  Widget _iconBtn(String glifo, VoidCallback? onTap) {
    return Material(
      color: _cSurface,
      borderRadius: BorderRadius.circular(8),
      child: InkWell(
        borderRadius: BorderRadius.circular(8),
        onTap: onTap,
        child: Container(
          width: 44,
          height: 46,
          alignment: Alignment.center,
          decoration: BoxDecoration(border: Border.all(color: _cLine), borderRadius: BorderRadius.circular(8)),
          child: Text(glifo, style: TextStyle(color: onTap == null ? _cTxt3 : _cTxt, fontSize: 20)),
        ),
      ),
    );
  }

  // ── Hoja de tonalidad ──
  void _abrirTono() {
    final actualIdx = ((_origBase + _sem) % 12 + 12) % 12;
    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      backgroundColor: _cSurface,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (_) => SingleChildScrollView(
        child: Padding(
        padding: EdgeInsets.only(
            left: 18, right: 18, top: 18, bottom: 28 + MediaQuery.of(context).padding.bottom),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Tonalidad', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: _cTxt)),
            const SizedBox(height: 4),
            Text('Actual: ${_nombreTono(_sem)} · original: ${_nombreTono(0)}',
                style: TextStyle(fontSize: 12, color: _cTxt2)),
            const SizedBox(height: 14),
            GridView.count(
              crossAxisCount: 4,
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              mainAxisSpacing: 9,
              crossAxisSpacing: 9,
              childAspectRatio: 1.8,
              children: List.generate(12, (i) {
                final delta = _normDelta(i - _origBase);
                final cur = i == actualIdx;
                return Material(
                  color: cur ? NW.chord : _cRaised,
                  borderRadius: BorderRadius.circular(10),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(10),
                    onTap: () => _irTono(delta),
                    child: Container(
                      alignment: Alignment.center,
                      decoration: BoxDecoration(
                        border: Border.all(color: cur ? NW.chord : _cLine),
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: Text(_nombres[i] + (_menor ? 'm' : ''),
                          style: TextStyle(
                              color: cur ? Colors.black : _cTxt,
                              fontWeight: FontWeight.bold,
                              fontSize: 16,
                              fontFamily: NW.mono)),
                    ),
                  ),
                );
              }),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              child: OutlinedButton(
                style: OutlinedButton.styleFrom(
                  foregroundColor: _cTxt2,
                  side: BorderSide(color: _cLine),
                  padding: const EdgeInsets.symmetric(vertical: 12),
                ),
                onPressed: () => _irTono(0),
                child: const Text('Volver al tono original'),
              ),
            ),
          ],
        ),
      ),
      ),
    );
  }

  // ── Hoja de ajustes de vista (completo, como la web) ──
  void _abrirAjustes() {
    showModalBottomSheet(
      context: context,
      backgroundColor: _cSurface,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) {
          void set(VoidCallback fn) {
            setState(fn);
            setSheet(() {});
          }

          return SingleChildScrollView(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(18, 18, 18, 28),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('Ajustes de vista', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: _cTxt)),
                  _ajLbl('TAMAÑO DE LETRA'),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      _sizeBtn('A−', _nivel == 0 ? null : () => set(() => _nivel--)),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16),
                        child: Text(_nivelLbls[_nivel],
                            style: const TextStyle(
                                color: NW.chord, fontWeight: FontWeight.bold, fontSize: 16, fontFamily: NW.mono)),
                      ),
                      _sizeBtn('A+', _nivel == 4 ? null : () => set(() => _nivel++)),
                    ],
                  ),
                  _ajLbl('TEMA'),
                  Row(children: [
                    _seg('🌙 Oscuro', !_claro, () => set(() => _claro = false)),
                    const SizedBox(width: 6),
                    _seg('☀️ Día', _claro, () => set(() => _claro = true)),
                  ]),
                  _ajLbl('MOSTRAR'),
                  Row(children: [
                    _seg('Ambos', _modo == 'ambos', () => set(() => _modo = 'ambos')),
                    const SizedBox(width: 6),
                    _seg('Solo acordes', _modo == 'acordes', () => set(() => _modo = 'acordes')),
                    const SizedBox(width: 6),
                    _seg('Solo letra', _modo == 'letra', () => set(() => _modo = 'letra')),
                  ]),
                  _ajLbl('COLOR DE ACORDES'),
                  Wrap(
                    spacing: 12,
                    runSpacing: 10,
                    children: _coloresAcorde.entries.map((e) {
                      final col = e.value ?? NW.chord;
                      final sel = _colorAcorde == e.value;
                      return GestureDetector(
                        onTap: () => set(() => _colorAcorde = e.value),
                        child: Container(
                          width: 34,
                          height: 34,
                          decoration: BoxDecoration(
                            color: col,
                            shape: BoxShape.circle,
                            border: Border.all(color: sel ? _cTxt : _cLine, width: 2),
                          ),
                        ),
                      );
                    }).toList(),
                  ),
                  _ajLbl('GROSOR DE ACORDES'),
                  Row(children: [
                    _seg('Fino', _grosor == 'fino', () => set(() => _grosor = 'fino')),
                    const SizedBox(width: 6),
                    _seg('Normal', _grosor == 'normal', () => set(() => _grosor = 'normal')),
                    const SizedBox(width: 6),
                    _seg('Grueso', _grosor == 'grueso', () => set(() => _grosor = 'grueso')),
                  ]),
                ],
              ),
            ),
          );
        },
      ),
    );
  }

  Widget _ajLbl(String t) => Padding(
        padding: const EdgeInsets.only(top: 16, bottom: 8),
        child: Text(t,
            style: TextStyle(
                fontSize: 11, color: _cTxt2, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
      );

  Widget _seg(String t, bool activo, VoidCallback onTap) {
    return Expanded(
      child: Material(
        color: activo ? NW.chord : _cRaised,
        borderRadius: BorderRadius.circular(9),
        child: InkWell(
          borderRadius: BorderRadius.circular(9),
          onTap: onTap,
          child: Container(
            height: 42,
            alignment: Alignment.center,
            padding: const EdgeInsets.symmetric(horizontal: 4),
            decoration: BoxDecoration(
              border: Border.all(color: activo ? NW.chord : _cLine),
              borderRadius: BorderRadius.circular(9),
            ),
            child: Text(t,
                textAlign: TextAlign.center,
                style: TextStyle(
                    color: activo ? Colors.black : _cTxt,
                    fontSize: 13,
                    fontWeight: FontWeight.w600)),
          ),
        ),
      ),
    );
  }

  Widget _sizeBtn(String t, VoidCallback? onTap) {
    return Material(
      color: _cRaised,
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Container(
          width: 64,
          height: 52,
          alignment: Alignment.center,
          decoration: BoxDecoration(border: Border.all(color: _cLine), borderRadius: BorderRadius.circular(12)),
          child: Text(t, style: TextStyle(color: onTap == null ? _cTxt3 : _cTxt, fontWeight: FontWeight.bold, fontSize: 20)),
        ),
      ),
    );
  }
}

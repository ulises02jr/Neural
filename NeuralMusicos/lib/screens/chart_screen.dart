import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../models.dart';

const _nombres = ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B'];

class ChartScreen extends StatefulWidget {
  final int numero;
  final int semInicial;
  final String tonoBase; // tono original de la cancion (semitono 0)

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

  int _idx = 0; // seccion activa
  int _nivel = 2; // xs,s,m,l,xl
  final _scroll = ScrollController();
  final List<GlobalKey> _keys = [];
  bool _scrollProgramatico = false;

  static const _lyricSizes = [14.0, 16.0, 18.0, 22.0, 26.0];
  static const _chordSizes = [13.0, 15.0, 17.0, 20.0, 24.0];
  static const _nivelLbls = ['XS', 'S', 'M', 'L', 'XL'];

  double get _lyricSize => _lyricSizes[_nivel];
  double get _chordSize => _chordSizes[_nivel];

  @override
  void initState() {
    super.initState();
    _sem = widget.semInicial;
    _parseTono(widget.tonoBase);
    _cargar();
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
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

  void _jump(int i) {
    final c = _chart;
    if (c == null || i < 0 || i >= c.secciones.length) return;
    setState(() => _idx = i);
    final ctx = _keys[i].currentContext;
    if (ctx != null) {
      _scrollProgramatico = true;
      Scrollable.ensureVisible(ctx,
              duration: const Duration(milliseconds: 300),
              alignment: 0.02,
              curve: Curves.easeInOut)
          .then((_) => _scrollProgramatico = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = _chart;
    return Scaffold(
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
          const Text('No se pudo cargar el chart', style: TextStyle(color: NW.txt2)),
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
                  Text(c.titulo,
                      style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w600)),
                  _tonoChip(c),
                ],
              ),
              const SizedBox(height: 3),
              Text(
                [
                  if (c.tempo != null && c.tempo! > 0) '${c.tempo} BPM',
                  if (c.compas.isNotEmpty) c.compas,
                ].join(' · '),
                style: const TextStyle(fontSize: 12, color: NW.txt2),
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
            Text('${_idx + 1} / ${c.secciones.length}',
                style: const TextStyle(fontSize: 12, color: NW.txt2)),
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
                    color: Colors.black,
                    fontWeight: FontWeight.bold,
                    fontSize: 14,
                    fontFamily: NW.mono)),
            const Text(' ▾',
                style: TextStyle(color: Colors.black54, fontSize: 10, fontWeight: FontWeight.bold)),
          ]),
        ),
      ),
    );
  }

  Widget _dotsBtn() {
    return Material(
      color: NW.surface,
      borderRadius: BorderRadius.circular(7),
      child: InkWell(
        borderRadius: BorderRadius.circular(7),
        onTap: _abrirAjustes,
        child: Container(
          width: 34,
          height: 30,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            border: Border.all(color: NW.line),
            borderRadius: BorderRadius.circular(7),
          ),
          child: const Text('⋯', style: TextStyle(color: NW.txt2, fontSize: 18)),
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
          return Material(
            color: activo ? NW.txt : NW.surface,
            borderRadius: BorderRadius.circular(7),
            child: InkWell(
              borderRadius: BorderRadius.circular(7),
              onTap: () => _jump(i),
              child: Container(
                alignment: Alignment.center,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                decoration: BoxDecoration(
                  border: Border.all(color: activo ? NW.txt : NW.line),
                  borderRadius: BorderRadius.circular(7),
                ),
                child: Text(c.secciones[i].tipo,
                    style: TextStyle(
                        fontSize: 13,
                        color: activo ? Colors.black : (done ? NW.txt3 : NW.txt2),
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
      child: ListView.builder(
        controller: _scroll,
        padding: const EdgeInsets.only(top: 2, bottom: 4),
        itemCount: c.secciones.length,
        itemBuilder: (_, i) => _seccionCard(c.secciones[i], i),
      ),
    );
  }

  void _detectarActiva() {
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
        color: NW.surface,
        border: Border.all(color: activo ? NW.chord : NW.line),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // sechead
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 7),
            decoration: BoxDecoration(
              color: NW.raised,
              border: Border(left: BorderSide(color: activo ? NW.chord : NW.line, width: 3)),
              borderRadius: BorderRadius.circular(7),
            ),
            child: Text(s.tipo.toUpperCase(),
                style: TextStyle(
                    fontSize: 11,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 1.8,
                    color: activo ? NW.chord : NW.txt3)),
          ),
          if (s.nota != null && s.nota!.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 10),
              child: Text(s.nota!,
                  style: const TextStyle(
                      fontSize: 13, color: NW.txt2, fontStyle: FontStyle.italic)),
            ),
          const SizedBox(height: 10),
          if (s.inst)
            _instrumental(s.prog)
          else
            ...s.lines.map(_linea),
        ],
      ),
    );
  }

  Widget _instrumental(List<String> prog) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Padding(
          padding: EdgeInsets.only(bottom: 8),
          child: Text('Instrumental', style: TextStyle(fontSize: 15, color: NW.txt2)),
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
          style: TextStyle(
              color: NW.chord,
              fontWeight: FontWeight.bold,
              fontFamily: NW.mono,
              fontSize: _chordSize)),
    );
  }

  Widget _linea(List<ChartSeg> segs) {
    // Linea "solo acordes" (sin letra): fila de chips.
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
              Text(seg.chord.isEmpty ? ' ' : seg.chord,
                  style: TextStyle(
                      color: NW.chord,
                      fontWeight: FontWeight.bold,
                      fontFamily: NW.mono,
                      fontSize: _chordSize,
                      height: 1.1)),
              Text(seg.text.isEmpty ? ' ' : seg.text,
                  style: TextStyle(color: NW.txt, fontSize: _lyricSize, height: 1.35)),
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
            color: NW.raised,
            borderRadius: BorderRadius.circular(8),
            child: InkWell(
              borderRadius: BorderRadius.circular(8),
              onTap: () => Navigator.of(context).pop(),
              child: Container(
                height: 46,
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  border: Border.all(color: NW.line),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: const Text('← Atrás',
                    style: TextStyle(color: NW.txt, fontSize: 12.5, fontWeight: FontWeight.w500)),
              ),
            ),
          ),
        ),
        const SizedBox(width: 6),
        _iconBtn('›', _idx >= c.secciones.length - 1 ? null : () => _jump(_idx + 1)),
      ],
    );
  }

  Widget _iconBtn(String glifo, VoidCallback? onTap) {
    return Material(
      color: NW.surface,
      borderRadius: BorderRadius.circular(8),
      child: InkWell(
        borderRadius: BorderRadius.circular(8),
        onTap: onTap,
        child: Container(
          width: 44,
          height: 46,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            border: Border.all(color: NW.line),
            borderRadius: BorderRadius.circular(8),
          ),
          child: Text(glifo,
              style: TextStyle(color: onTap == null ? NW.txt3 : NW.txt, fontSize: 20)),
        ),
      ),
    );
  }

  // ── Hoja de tonalidad ──
  void _abrirTono() {
    final actualIdx = ((_origBase + _sem) % 12 + 12) % 12;
    showModalBottomSheet(
      context: context,
      backgroundColor: NW.surface,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (_) => Padding(
        padding: const EdgeInsets.fromLTRB(18, 18, 18, 28),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('Tonalidad',
                style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            const SizedBox(height: 4),
            Text('Actual: ${_nombreTono(_sem)} · original: ${_nombreTono(0)}',
                style: const TextStyle(fontSize: 12, color: NW.txt2)),
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
                  color: cur ? NW.chord : NW.raised,
                  borderRadius: BorderRadius.circular(10),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(10),
                    onTap: () => _irTono(delta),
                    child: Container(
                      alignment: Alignment.center,
                      decoration: BoxDecoration(
                        border: Border.all(color: cur ? NW.chord : NW.line),
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: Text(_nombres[i] + (_menor ? 'm' : ''),
                          style: TextStyle(
                              color: cur ? Colors.black : NW.txt,
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
                  foregroundColor: NW.txt2,
                  side: const BorderSide(color: NW.line),
                  padding: const EdgeInsets.symmetric(vertical: 12),
                ),
                onPressed: () => _irTono(0),
                child: const Text('Volver al tono original'),
              ),
            ),
          ],
        ),
      ),
    );
  }

  // ── Hoja de ajustes (tamano de letra) ──
  void _abrirAjustes() {
    showModalBottomSheet(
      context: context,
      backgroundColor: NW.surface,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => Padding(
          padding: const EdgeInsets.fromLTRB(18, 18, 18, 28),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('Ajustes de vista',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
              const SizedBox(height: 16),
              const Text('TAMAÑO DE LETRA',
                  style: TextStyle(
                      fontSize: 11,
                      color: NW.txt2,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 1.2)),
              const SizedBox(height: 10),
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  _sizeBtn('A−', _nivel == 0 ? null : () {
                    setState(() => _nivel--);
                    setSheet(() {});
                  }),
                  Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    child: Text(_nivelLbls[_nivel],
                        style: const TextStyle(
                            color: NW.chord,
                            fontWeight: FontWeight.bold,
                            fontSize: 16,
                            fontFamily: NW.mono)),
                  ),
                  _sizeBtn('A+', _nivel == 4 ? null : () {
                    setState(() => _nivel++);
                    setSheet(() {});
                  }),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _sizeBtn(String t, VoidCallback? onTap) {
    return Material(
      color: NW.raised,
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Container(
          width: 64,
          height: 52,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            border: Border.all(color: NW.line),
            borderRadius: BorderRadius.circular(12),
          ),
          child: Text(t,
              style: TextStyle(
                  color: onTap == null ? NW.txt3 : NW.txt,
                  fontWeight: FontWeight.bold,
                  fontSize: 20)),
        ),
      ),
    );
  }
}

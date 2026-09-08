import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';

// Orden de familias como en la web.
const _orden = [
  'Voces', 'Guitarras', 'Teclados', 'Cuerdas', 'Metales',
  'Bajo', 'Percusión', 'Guía', 'Música original', 'Otros', 'Click'
];

/// Abre el Modo Ensayo (vista previa: mixer + transporte, el audio se conecta luego).
void showRehearsal(BuildContext context, {required int numero, required int sem}) {
  showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    backgroundColor: NW.surface,
    shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(18))),
    builder: (_) => _RehearsalBody(numero: numero, sem: sem),
  );
}

class _Stem {
  final String name;
  final String familia;
  bool on = true;
  _Stem(this.name, this.familia);
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
  String? _status; // mensaje de estado (sin pistas / no listo / error)
  final List<_Stem> _stems = [];
  final List<String> _secciones = [];
  String? _famActiva;
  int? _solo;
  int _secActiva = 0;
  bool _playing = false;

  @override
  void initState() {
    super.initState();
    _cargar();
  }

  Future<void> _cargar() async {
    setState(() {
      _cargando = true;
      _status = null;
    });
    final j = await Api.I.pistas(widget.numero, widget.sem);
    if (!mounted) return;
    if (j == null) {
      setState(() {
        _cargando = false;
        _status = 'No pude cargar las pistas. Intentá de nuevo.';
      });
      return;
    }
    if (j['hay_pistas'] != true) {
      setState(() {
        _cargando = false;
        _status = 'Esta canción todavía no tiene pistas cargadas. 🎵';
      });
      return;
    }
    if (j['listo'] != true) {
      final t = widget.sem;
      setState(() {
        _cargando = false;
        _status = '🎚️ El tono ${t > 0 ? '+' : ''}$t todavía no está preparado.\n'
            'Se genera la primera vez que se usa (falta conectar el audio).';
      });
      return;
    }
    final stems = (j['stems'] as List? ?? []);
    final secs = (j['secciones'] as List? ?? []);
    setState(() {
      _cargando = false;
      _stems
        ..clear()
        ..addAll(stems.map((s) {
          final m = Map<String, dynamic>.from(s as Map);
          return _Stem((m['name'] ?? '').toString(), (m['familia'] ?? 'Otros').toString());
        }));
      _secciones
        ..clear()
        ..addAll(secs.map((s) => (Map<String, dynamic>.from(s as Map)['nombre'] ?? '').toString()));
      if (_stems.isEmpty) _status = 'No hay pistas para este tono.';
    });
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
    final maxH = MediaQuery.of(context).size.height * 0.7;
    return ConstrainedBox(
      constraints: BoxConstraints(maxHeight: maxH),
      child: Padding(
        padding: EdgeInsets.only(
          left: 16, right: 16, top: 12,
          bottom: 14 + MediaQuery.of(context).padding.bottom,
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _header(),
            const SizedBox(height: 8),
            Flexible(
              child: SingleChildScrollView(
                child: _cargando
                    ? _statusView('Cargando…')
                    : _status != null
                        ? _statusView(_status!)
                        : _contenido(),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _header() {
    final t = widget.sem;
    final tonoTxt = t != 0 ? ' · tono ${t > 0 ? '+' : ''}$t' : '';
    return Row(
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('Modo ensayo',
                  style: TextStyle(fontSize: 13, fontWeight: FontWeight.bold, letterSpacing: 0.3)),
              Text('Pistas de esta canción$tonoTxt',
                  style: const TextStyle(fontSize: 11, color: NW.txt2)),
            ],
          ),
        ),
        IconButton(
          icon: const Icon(Icons.close, color: NW.txt2),
          onPressed: () => Navigator.of(context).pop(),
        ),
      ],
    );
  }

  Widget _statusView(String t) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 24, horizontal: 10),
        child: Column(
          children: [
            Text(t,
                textAlign: TextAlign.center,
                style: const TextStyle(color: NW.txt2, fontSize: 13, height: 1.6)),
            if (_status != null && _status!.contains('Intentá')) ...[
              const SizedBox(height: 14),
              OutlinedButton(onPressed: _cargar, child: const Text('Reintentar')),
            ],
          ],
        ),
      );

  Widget _contenido() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _transport(),
        const SizedBox(height: 8),
        if (_secciones.isNotEmpty) _secbar(),
        const SizedBox(height: 10),
        _transportBtns(),
        const SizedBox(height: 6),
        const Center(
          child: Text('Vista previa — el audio se conecta en el próximo paso',
              style: TextStyle(fontSize: 10.5, color: NW.txt3, fontStyle: FontStyle.italic)),
        ),
        const SizedBox(height: 14),
        _familiaGrid(),
        if (_famActiva != null) ...[
          const SizedBox(height: 10),
          _chips(),
        ],
      ],
    );
  }

  Widget _transport() {
    return Row(
      children: [
        const Text('0:00', style: TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
        Expanded(
          child: SliderTheme(
            data: const SliderThemeData(
              trackHeight: 5,
              activeTrackColor: NW.gold,
              inactiveTrackColor: NW.line,
              thumbColor: NW.gold,
              thumbShape: RoundSliderThumbShape(enabledThumbRadius: 7),
              overlayShape: RoundSliderOverlayShape(overlayRadius: 14),
            ),
            child: Slider(value: 0, onChanged: (_) {}),
          ),
        ),
        const Text('--:--', style: TextStyle(color: NW.txt2, fontSize: 12, fontFamily: NW.mono)),
      ],
    );
  }

  Widget _secbar() {
    return Container(
      height: 26,
      decoration: BoxDecoration(
        color: const Color(0xFF0F0F0F),
        borderRadius: BorderRadius.circular(6),
      ),
      clipBehavior: Clip.antiAlias,
      child: Row(
        children: List.generate(_secciones.length, (i) {
          final cur = i == _secActiva;
          return Expanded(
            child: GestureDetector(
              onTap: () => setState(() => _secActiva = i),
              child: Container(
                decoration: BoxDecoration(
                  color: cur ? const Color(0xFF3A3320) : const Color(0xFF1C1C1C),
                  border: Border(
                      left: BorderSide(color: i == 0 ? Colors.transparent : NW.line)),
                ),
                alignment: Alignment.centerLeft,
                padding: const EdgeInsets.only(left: 4),
                child: Text(_secciones[i],
                    maxLines: 1,
                    overflow: TextOverflow.clip,
                    style: TextStyle(fontSize: 9, color: cur ? NW.txt : const Color(0xFFB7B7B7))),
              ),
            ),
          );
        }),
      ),
    );
  }

  Widget _transportBtns() {
    return Row(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        _tbtn('⏮', false, () {}),
        const SizedBox(width: 12),
        _tbtn(_playing ? '❚❚' : '▶', true, () => setState(() => _playing = !_playing)),
        const SizedBox(width: 12),
        _tbtn('🔁', false, () {}),
      ],
    );
  }

  Widget _tbtn(String glifo, bool play, VoidCallback onTap) {
    final size = play ? 50.0 : 42.0;
    return Material(
      color: play ? NW.gold : NW.raised,
      shape: const CircleBorder(),
      child: InkWell(
        customBorder: const CircleBorder(),
        onTap: onTap,
        child: Container(
          width: size,
          height: size,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            border: Border.all(color: play ? NW.gold : NW.line),
          ),
          child: Text(glifo,
              style: TextStyle(color: play ? Colors.black : NW.txt, fontSize: play ? 20 : 16)),
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
          color: activa ? const Color(0xFF3A3320) : NW.raised,
          borderRadius: BorderRadius.circular(9),
          child: InkWell(
            borderRadius: BorderRadius.circular(9),
            onTap: () => setState(() => _famActiva = activa ? null : f),
            child: Container(
              alignment: Alignment.center,
              decoration: BoxDecoration(
                border: Border.all(color: activa ? NW.gold : NW.line),
                borderRadius: BorderRadius.circular(9),
              ),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text(f,
                      textAlign: TextAlign.center,
                      style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: NW.txt)),
                  Text('${g[f]!.length}',
                      style: const TextStyle(fontSize: 10, color: NW.txt2)),
                ],
              ),
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
            color: on ? NW.goldSoft : Colors.transparent,
            borderRadius: BorderRadius.circular(999),
            border: Border.all(color: on ? NW.gold : NW.line, width: 1.5),
          ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              GestureDetector(
                onTap: () => setState(() {
                  s.on = !s.on;
                  _solo = null;
                }),
                child: Text(s.name,
                    style: TextStyle(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                        color: on ? NW.txt : NW.txt2)),
              ),
              const SizedBox(width: 6),
              GestureDetector(
                onTap: () => setState(() => _solo = solo ? null : i),
                child: Container(
                  width: 19,
                  height: 19,
                  alignment: Alignment.center,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: solo ? NW.gold : const Color(0xFF0A0A0A),
                    border: Border.all(color: solo ? NW.gold : NW.line),
                  ),
                  child: Text('S',
                      style: TextStyle(
                          fontSize: 9,
                          fontWeight: FontWeight.bold,
                          color: solo ? Colors.black : NW.txt2)),
                ),
              ),
            ],
          ),
        );
      }).toList(),
    );
  }
}

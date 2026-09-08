import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../models.dart';

class ChartScreen extends StatefulWidget {
  final int numero;
  final int semInicial;
  final String tonoNombre;

  const ChartScreen({
    super.key,
    required this.numero,
    required this.semInicial,
    required this.tonoNombre,
  });

  @override
  State<ChartScreen> createState() => _ChartScreenState();
}

class _ChartScreenState extends State<ChartScreen> {
  late int _sem;
  Chart? _chart;
  bool _cargando = true;
  bool _error = false;
  double _escala = 1.0;

  @override
  void initState() {
    super.initState();
    _sem = widget.semInicial;
    _cargar();
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
    });
  }

  void _transponer(int delta) {
    final nuevo = (_sem + delta).clamp(-6, 6);
    if (nuevo == _sem) return;
    setState(() => _sem = nuevo);
    _cargar();
  }

  @override
  Widget build(BuildContext context) {
    final c = _chart;
    return Scaffold(
      appBar: AppBar(
        title: Text(c?.titulo ?? 'Cargando...',
            maxLines: 1, overflow: TextOverflow.ellipsis,
            style: const TextStyle(fontSize: 16)),
        actions: [
          IconButton(
            icon: const Icon(Icons.text_decrease, size: 20),
            tooltip: 'Letra mas chica',
            onPressed: () => setState(() => _escala = (_escala - 0.1).clamp(0.8, 1.6)),
          ),
          IconButton(
            icon: const Icon(Icons.text_increase, size: 20),
            tooltip: 'Letra mas grande',
            onPressed: () => setState(() => _escala = (_escala + 0.1).clamp(0.8, 1.6)),
          ),
        ],
      ),
      body: _cargando
          ? const Center(child: CircularProgressIndicator(color: NW.gold))
          : _error || c == null
              ? Center(
                  child: Column(mainAxisSize: MainAxisSize.min, children: [
                    const Text('No se pudo cargar el chart', style: TextStyle(color: NW.txt2)),
                    const SizedBox(height: 12),
                    OutlinedButton(onPressed: _cargar, child: const Text('Reintentar')),
                  ]),
                )
              : Column(
                  children: [
                    _barraInfo(c),
                    Expanded(
                      child: ListView.builder(
                        padding: const EdgeInsets.fromLTRB(16, 8, 16, 40),
                        itemCount: c.secciones.length,
                        itemBuilder: (_, i) => _seccion(c.secciones[i]),
                      ),
                    ),
                  ],
                ),
    );
  }

  Widget _barraInfo(Chart c) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      decoration: const BoxDecoration(
        color: NW.surface,
        border: Border(bottom: BorderSide(color: NW.line)),
      ),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (c.artista.isNotEmpty)
                  Text(c.artista,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(color: NW.txt2, fontSize: 13)),
                Row(children: [
                  if (c.tempo != null && c.tempo! > 0)
                    Text('${c.tempo} BPM  ', style: const TextStyle(color: NW.txt3, fontSize: 12)),
                  if (c.compas.isNotEmpty)
                    Text(c.compas, style: const TextStyle(color: NW.txt3, fontSize: 12)),
                ]),
              ],
            ),
          ),
          _btnTono(Icons.remove, () => _transponer(-1)),
          Container(
            margin: const EdgeInsets.symmetric(horizontal: 6),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            decoration: BoxDecoration(color: NW.goldSoft, borderRadius: BorderRadius.circular(6)),
            child: Column(
              children: [
                Text(c.tono.isEmpty ? '—' : c.tono,
                    style: const TextStyle(
                        color: NW.gold, fontWeight: FontWeight.bold, fontSize: 16, fontFamily: NW.mono)),
                Text(_sem == 0 ? 'tono' : (_sem > 0 ? '+$_sem' : '$_sem'),
                    style: const TextStyle(color: NW.txt3, fontSize: 10)),
              ],
            ),
          ),
          _btnTono(Icons.add, () => _transponer(1)),
        ],
      ),
    );
  }

  Widget _btnTono(IconData ic, VoidCallback onTap) {
    return Material(
      color: NW.raised,
      borderRadius: BorderRadius.circular(6),
      child: InkWell(
        borderRadius: BorderRadius.circular(6),
        onTap: onTap,
        child: Padding(padding: const EdgeInsets.all(8), child: Icon(ic, size: 20, color: NW.txt)),
      ),
    );
  }

  Widget _seccion(ChartSection s) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(top: 16, bottom: 6),
          child: Row(children: [
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
              decoration: BoxDecoration(
                color: NW.goldSoft,
                borderRadius: BorderRadius.circular(4),
              ),
              child: Text(s.tipo,
                  style: const TextStyle(
                      color: NW.gold, fontWeight: FontWeight.bold, fontSize: 12)),
            ),
            if (s.nota != null && s.nota!.isNotEmpty)
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.only(left: 10),
                  child: Text(s.nota!,
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(color: NW.txt3, fontSize: 11, fontStyle: FontStyle.italic)),
                ),
              ),
          ]),
        ),
        if (s.inst) _progInstrumental(s.prog) else ...s.lines.map(_lineaLetra),
      ],
    );
  }

  Widget _progInstrumental(List<String> prog) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Wrap(
        spacing: 10,
        runSpacing: 6,
        children: prog
            .map((ch) => Text(ch,
                style: TextStyle(
                    color: NW.gold,
                    fontWeight: FontWeight.bold,
                    fontFamily: NW.mono,
                    fontSize: 16 * _escala)))
            .toList(),
      ),
    );
  }

  /// Linea de letra: cada segmento muestra el acorde arriba (dorado) y el texto abajo.
  Widget _lineaLetra(List<ChartSeg> segs) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Wrap(
        crossAxisAlignment: WrapCrossAlignment.end,
        children: segs.map((seg) {
          return Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SizedBox(
                height: 20 * _escala,
                child: Text(seg.chord,
                    style: TextStyle(
                        color: NW.gold,
                        fontWeight: FontWeight.bold,
                        fontFamily: NW.mono,
                        fontSize: 14 * _escala,
                        height: 1.0)),
              ),
              Text(seg.text,
                  style: TextStyle(color: NW.txt, fontSize: 16 * _escala, height: 1.2)),
            ],
          );
        }).toList(),
      ),
    );
  }
}

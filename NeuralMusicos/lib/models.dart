// Modelos de datos de la API de Neural Worship.

class Song {
  final int id;
  final String titulo;
  final String artista;
  final String tono;
  final String compas;
  final String portada;
  final int portadaTs;
  final int? tempo;

  Song({
    required this.id,
    required this.titulo,
    required this.artista,
    required this.tono,
    required this.compas,
    required this.portada,
    required this.portadaTs,
    this.tempo,
  });

  factory Song.fromIndex(String key, Map<String, dynamic> j) {
    int parseTempo(dynamic t) => (t is num) ? t.toInt() : 0;
    return Song(
      id: (j['id'] is num) ? (j['id'] as num).toInt() : int.tryParse(key) ?? 0,
      titulo: (j['titulo'] ?? '').toString(),
      artista: (j['artista'] ?? '').toString(),
      tono: (j['tono'] ?? '').toString(),
      compas: (j['compas'] ?? '').toString(),
      portada: (j['portada'] ?? '').toString(),
      portadaTs: (j['portada_ts'] is num) ? (j['portada_ts'] as num).toInt() : 0,
      tempo: (j['tempo'] == null) ? null : parseTempo(j['tempo']),
    );
  }
}

class SetlistItem {
  final int id;
  final String tonoNombre; // tono a mostrar (override o base)
  final int tonoSemitonos; // semitonos respecto al tono base

  SetlistItem({required this.id, required this.tonoNombre, required this.tonoSemitonos});

  factory SetlistItem.fromJson(Map<String, dynamic> j) => SetlistItem(
        id: (j['id'] is num) ? (j['id'] as num).toInt() : 0,
        tonoNombre: (j['tono_nombre'] ?? j['tono'] ?? '').toString(),
        tonoSemitonos: (j['tono_semitonos'] is num) ? (j['tono_semitonos'] as num).toInt() : 0,
      );
}

class Setlist {
  final String id;
  final String nombre;
  final String fecha;
  final List<SetlistItem> canciones;

  Setlist({required this.id, required this.nombre, required this.fecha, required this.canciones});

  factory Setlist.fromJson(Map<String, dynamic> j) {
    final items = <SetlistItem>[];
    for (final c in (j['canciones'] as List? ?? [])) {
      items.add(SetlistItem.fromJson(Map<String, dynamic>.from(c as Map)));
    }
    return Setlist(
      id: (j['id'] ?? '').toString(),
      nombre: (j['nombre'] ?? '').toString(),
      fecha: (j['fecha'] ?? '').toString(),
      canciones: items,
    );
  }
}

/// Un segmento de una linea del chart: acorde (puede ir vacio) + texto.
class ChartSeg {
  final String chord;
  final String text;
  ChartSeg(this.chord, this.text);
}

/// Una seccion del chart: instrumental (prog de acordes) o de letra (lines).
class ChartSection {
  final String tipo;
  final String? nota;
  final bool inst;
  final List<String> prog;
  final List<List<ChartSeg>> lines;

  ChartSection({
    required this.tipo,
    required this.nota,
    required this.inst,
    required this.prog,
    required this.lines,
  });

  factory ChartSection.fromJson(Map<String, dynamic> j) {
    final inst = j['inst'] == true;
    final prog = <String>[];
    for (final p in (j['prog'] as List? ?? [])) {
      prog.add(p.toString());
    }
    final lines = <List<ChartSeg>>[];
    for (final ln in (j['lines'] as List? ?? [])) {
      final segs = <ChartSeg>[];
      for (final seg in (ln as List? ?? [])) {
        final s = seg as List;
        final chord = s.isNotEmpty ? s[0].toString() : '';
        final text = s.length > 1 ? s[1].toString() : '';
        segs.add(ChartSeg(chord, text));
      }
      lines.add(segs);
    }
    return ChartSection(
      tipo: (j['tipo'] ?? '').toString(),
      nota: j['nota']?.toString(),
      inst: inst,
      prog: prog,
      lines: lines,
    );
  }
}

class Chart {
  final int numero;
  final String titulo;
  final String artista;
  final String tono;
  final int? tempo;
  final String compas;
  final List<ChartSection> secciones;

  Chart({
    required this.numero,
    required this.titulo,
    required this.artista,
    required this.tono,
    required this.tempo,
    required this.compas,
    required this.secciones,
  });

  factory Chart.fromJson(Map<String, dynamic> j) {
    final secs = <ChartSection>[];
    for (final s in (j['secciones'] as List? ?? [])) {
      secs.add(ChartSection.fromJson(Map<String, dynamic>.from(s as Map)));
    }
    return Chart(
      numero: (j['numero'] is num) ? (j['numero'] as num).toInt() : 0,
      titulo: (j['titulo'] ?? '').toString(),
      artista: (j['artista'] ?? '').toString(),
      tono: (j['tono'] ?? '').toString(),
      tempo: (j['tempo'] is num) ? (j['tempo'] as num).toInt() : null,
      compas: (j['compas'] ?? '').toString(),
      secciones: secs,
    );
  }
}

import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../models.dart';
import 'login_screen.dart';
import 'chart_screen.dart';
import 'settings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> with SingleTickerProviderStateMixin {
  late TabController _tabs;
  List<Song> _songs = [];
  List<Setlist> _setlists = [];
  Map<int, Song> _porId = {};
  bool _cargando = true;
  String? _error;
  String _busca = '';

  @override
  void initState() {
    super.initState();
    _tabs = TabController(length: 2, vsync: this);
    _cargar();
  }

  @override
  void dispose() {
    _tabs.dispose();
    super.dispose();
  }

  Future<void> _cargar() async {
    setState(() {
      _cargando = true;
      _error = null;
    });
    final r = await Api.I.biblioteca();
    if (!mounted) return;
    if (r['ok'] == true) {
      final songs = (r['songs'] as List).cast<Song>();
      setState(() {
        _songs = songs;
        _setlists = (r['setlists'] as List).cast<Setlist>();
        _porId = {for (final s in songs) s.id: s};
        _cargando = false;
      });
    } else if (r['error'] == 'unauthorized') {
      await _salir();
    } else {
      setState(() {
        _cargando = false;
        _error = 'No se pudo cargar la biblioteca';
      });
    }
  }

  Future<void> _salir() async {
    await Api.I.logout();
    if (!mounted) return;
    Navigator.of(context).pushReplacement(
      MaterialPageRoute(builder: (_) => const LoginScreen()),
    );
  }

  String _fechaLinda(String iso) {
    final m = RegExp(r'^(\d{4})-(\d{2})-(\d{2})').firstMatch(iso);
    if (m == null) return iso;
    const meses = ['ene', 'feb', 'mar', 'abr', 'may', 'jun', 'jul', 'ago', 'sep', 'oct', 'nov', 'dic'];
    final mo = int.parse(m.group(2)!);
    if (mo < 1 || mo > 12) return iso;
    return '${int.parse(m.group(3)!)} ${meses[mo - 1]} ${m.group(1)}';
  }

  void _abrir(Song s, int sem) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => ChartScreen(
        numero: s.id,
        semInicial: sem,
        tonoBase: s.tono,
      ),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        centerTitle: true,
        title: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            Text(Api.I.orgNombre.isEmpty ? 'Neural Worship' : Api.I.orgNombre,
                textAlign: TextAlign.center,
                style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
            if (Api.I.nombre.isNotEmpty)
              Text('Bienvenido ${Api.I.nombre}',
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                      fontSize: 11, color: NW.txt3, fontWeight: FontWeight.normal)),
          ],
        ),
        actions: [
          IconButton(
            tooltip: 'Configuración',
            icon: const Icon(Icons.settings_outlined, size: 22, color: NW.txt2),
            onPressed: () async {
              final nav = Navigator.of(context);
              await nav.push(
                MaterialPageRoute(builder: (_) => const SettingsScreen()),
              );
              if (mounted && !Api.I.logueado) {
                nav.pushReplacement(
                  MaterialPageRoute(builder: (_) => const LoginScreen()),
                );
              }
            },
          ),
        ],
        bottom: TabBar(
          controller: _tabs,
          indicatorColor: NW.chord,
          labelColor: NW.txt,
          unselectedLabelColor: NW.txt3,
          tabs: [
            Tab(text: 'Repertorios (${_setlists.length})'),
            Tab(text: 'Biblioteca (${_songs.length})'),
          ],
        ),
      ),
      body: _cargando
          ? const Center(child: CircularProgressIndicator(color: NW.chord))
          : _error != null
              ? _errorView()
              : TabBarView(
                  controller: _tabs,
                  children: [_setlistsTab(), _bibliotecaTab()],
                ),
    );
  }

  Widget _errorView() => Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(_error!, style: const TextStyle(color: NW.txt2)),
            const SizedBox(height: 12),
            OutlinedButton(onPressed: _cargar, child: const Text('Reintentar')),
          ],
        ),
      );

  Widget _bibliotecaTab() {
    final q = _busca.trim().toLowerCase();
    final lista = q.isEmpty
        ? _songs
        : _songs
            .where((s) =>
                s.titulo.toLowerCase().contains(q) || s.artista.toLowerCase().contains(q))
            .toList();
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(14, 12, 14, 8),
          child: TextField(
            decoration: const InputDecoration(
              hintText: 'Buscar por título o artista...',
              prefixIcon: Icon(Icons.search, color: NW.txt3, size: 20),
              isDense: true,
            ),
            onChanged: (v) => setState(() => _busca = v),
          ),
        ),
        Expanded(
          child: RefreshIndicator(
            color: NW.chord,
            onRefresh: _cargar,
            child: lista.isEmpty
                ? ListView(children: const [
                    Padding(
                      padding: EdgeInsets.all(40),
                      child: Text('No se encontraron canciones',
                          textAlign: TextAlign.center, style: TextStyle(color: NW.txt3)),
                    )
                  ])
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(14, 4, 14, 20),
                    itemCount: lista.length,
                    separatorBuilder: (_, __) => const SizedBox(height: 7),
                    itemBuilder: (_, i) {
                      final s = lista[i];
                      return _songTile(
                        song: s,
                        numLabel: '#${s.id}',
                        tonoText: s.tono,
                        sem: 0,
                      );
                    },
                  ),
          ),
        ),
      ],
    );
  }

  Widget _setlistsTab() {
    if (_setlists.isEmpty) {
      return RefreshIndicator(
        color: NW.chord,
        onRefresh: _cargar,
        child: ListView(children: const [
          Padding(
            padding: EdgeInsets.all(40),
            child: Text('No hay setlist armado para hoy',
                textAlign: TextAlign.center,
                style: TextStyle(color: NW.txt3, fontStyle: FontStyle.italic)),
          )
        ]),
      );
    }
    return RefreshIndicator(
      color: NW.chord,
      onRefresh: _cargar,
      child: ListView.builder(
        padding: const EdgeInsets.fromLTRB(14, 12, 14, 20),
        itemCount: _setlists.length,
        itemBuilder: (_, i) {
          final sl = _setlists[i];
          return Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Container(
                margin: const EdgeInsets.only(bottom: 12, top: 4),
                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
                decoration: BoxDecoration(
                  color: NW.surface,
                  borderRadius: BorderRadius.circular(10),
                  border: Border.all(color: NW.line),
                ),
                child: Row(
                  children: [
                    Container(
                      width: 40,
                      height: 40,
                      alignment: Alignment.center,
                      decoration: BoxDecoration(
                        color: NW.raised,
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: const Icon(Icons.calendar_month_outlined, size: 20, color: NW.txt2),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(sl.nombre.isEmpty ? 'Repertorio' : sl.nombre,
                              style: const TextStyle(
                                  fontWeight: FontWeight.w700, fontSize: 15, color: NW.txt)),
                          if (sl.fecha.isNotEmpty)
                            Padding(
                              padding: const EdgeInsets.only(top: 2),
                              child: Text(_fechaLinda(sl.fecha),
                                  style: const TextStyle(fontSize: 12, color: NW.txt2)),
                            ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              ...sl.canciones.asMap().entries.map((e) {
                final it = e.value;
                final song = _porId[it.id];
                if (song == null) return const SizedBox.shrink();
                return Padding(
                  padding: const EdgeInsets.only(bottom: 7),
                  child: _songTile(
                    song: song,
                    numLabel: '${e.key + 1}',
                    tonoText: it.tonoNombre,
                    sem: it.tonoSemitonos,
                    setlist: true,
                  ),
                );
              }),
              const SizedBox(height: 12),
            ],
          );
        },
      ),
    );
  }

  Widget _songTile({
    required Song song,
    required String numLabel,
    required String tonoText,
    required int sem,
    bool setlist = false,
  }) {
    return Material(
      color: setlist ? const Color(0xFF161616) : NW.surface,
      borderRadius: BorderRadius.circular(10),
      child: InkWell(
        borderRadius: BorderRadius.circular(10),
        onTap: () => _abrir(song, sem),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 12),
          decoration: BoxDecoration(
            border: Border.all(color: setlist ? const Color(0xFF333333) : NW.line),
            borderRadius: BorderRadius.circular(10),
          ),
          child: Row(
            children: [
              ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: Image.network(
                  Api.I.portadaUrl(song.portada, ts: song.portadaTs),
                  width: 38,
                  height: 38,
                  fit: BoxFit.cover,
                  errorBuilder: (_, __, ___) => Container(
                    width: 38,
                    height: 38,
                    color: NW.raised,
                    child: const Icon(Icons.music_note, size: 18, color: NW.txt3),
                  ),
                ),
              ),
              const SizedBox(width: 12),
              SizedBox(
                width: 30,
                child: Text(numLabel,
                    style: TextStyle(
                        color: setlist ? NW.chord : NW.txt3,
                        fontFamily: NW.mono,
                        fontSize: 13,
                        fontWeight: FontWeight.w500)),
              ),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Flexible(
                          child: Text(song.titulo,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: const TextStyle(
                                  fontSize: 15, fontWeight: FontWeight.w600, color: NW.txt)),
                        ),
                        if (tonoText.isNotEmpty) ...[
                          const SizedBox(width: 8),
                          Container(
                            padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 1),
                            decoration: BoxDecoration(
                              color: NW.chordSoft,
                              borderRadius: BorderRadius.circular(4),
                            ),
                            child: Text(tonoText,
                                style: const TextStyle(
                                    color: NW.chord,
                                    fontWeight: FontWeight.bold,
                                    fontSize: 13,
                                    fontFamily: NW.mono)),
                          ),
                        ],
                      ],
                    ),
                    if (song.artista.isNotEmpty)
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: Text(song.artista,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(fontSize: 12, color: NW.txt2)),
                      ),
                  ],
                ),
              ),
              const Text('›', style: TextStyle(color: NW.txt3, fontSize: 22)),
            ],
          ),
        ),
      ),
    );
  }
}

import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../models.dart';
import 'login_screen.dart';
import 'chart_screen.dart';

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

  void _abrir(int numero, int sem, String tonoNombre) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => ChartScreen(numero: numero, semInicial: sem, tonoNombre: tonoNombre),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        titleSpacing: 16,
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(Api.I.orgNombre.isEmpty ? 'Neural Worship' : Api.I.orgNombre,
                style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
            if (Api.I.nombre.isNotEmpty)
              Text('Bienvenido ${Api.I.nombre}',
                  style: const TextStyle(fontSize: 11, color: NW.txt3, fontWeight: FontWeight.normal)),
          ],
        ),
        actions: [
          IconButton(
            tooltip: 'Salir',
            icon: const Icon(Icons.logout, size: 20, color: NW.txt3),
            onPressed: _salir,
          ),
        ],
        bottom: TabBar(
          controller: _tabs,
          indicatorColor: NW.gold,
          labelColor: NW.txt,
          unselectedLabelColor: NW.txt3,
          tabs: [
            Tab(text: 'Biblioteca (${_songs.length})'),
            Tab(text: 'Setlists (${_setlists.length})'),
          ],
        ),
      ),
      body: _cargando
          ? const Center(child: CircularProgressIndicator(color: NW.gold))
          : _error != null
              ? _errorView()
              : TabBarView(
                  controller: _tabs,
                  children: [_bibliotecaTab(), _setlistsTab()],
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
              hintText: 'Buscar cancion o artista',
              prefixIcon: Icon(Icons.search, color: NW.txt3, size: 20),
              isDense: true,
            ),
            onChanged: (v) => setState(() => _busca = v),
          ),
        ),
        Expanded(
          child: RefreshIndicator(
            color: NW.gold,
            onRefresh: _cargar,
            child: lista.isEmpty
                ? ListView(children: const [
                    Padding(
                      padding: EdgeInsets.all(40),
                      child: Text('Sin resultados',
                          textAlign: TextAlign.center, style: TextStyle(color: NW.txt3)),
                    )
                  ])
                : ListView.separated(
                    padding: const EdgeInsets.fromLTRB(14, 4, 14, 20),
                    itemCount: lista.length,
                    separatorBuilder: (_, __) => const SizedBox(height: 7),
                    itemBuilder: (_, i) => _songTile(lista[i], lista[i].tono, 0),
                  ),
          ),
        ),
      ],
    );
  }

  Widget _setlistsTab() {
    if (_setlists.isEmpty) {
      return const Center(
        child: Text('No hay setlists todavia', style: TextStyle(color: NW.txt3)),
      );
    }
    return RefreshIndicator(
      color: NW.gold,
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
                margin: const EdgeInsets.only(bottom: 10, top: 4),
                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                decoration: const BoxDecoration(
                  color: NW.goldSoft,
                  border: Border(left: BorderSide(color: NW.gold, width: 3)),
                  borderRadius: BorderRadius.all(Radius.circular(6)),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(sl.nombre.isEmpty ? 'Setlist' : sl.nombre,
                        style: const TextStyle(fontWeight: FontWeight.w600, color: NW.txt)),
                    if (sl.fecha.isNotEmpty)
                      Text(sl.fecha,
                          style: const TextStyle(fontSize: 11, color: NW.txt2, fontFamily: NW.mono)),
                  ],
                ),
              ),
              ...sl.canciones.map((it) {
                final song = _porId[it.id];
                if (song == null) return const SizedBox.shrink();
                return Padding(
                  padding: const EdgeInsets.only(bottom: 7),
                  child: _songTile(song, it.tonoNombre, it.tonoSemitonos),
                );
              }),
              const SizedBox(height: 12),
            ],
          );
        },
      ),
    );
  }

  Widget _songTile(Song s, String tonoMostrar, int sem) {
    return Material(
      color: NW.surface,
      borderRadius: BorderRadius.circular(10),
      child: InkWell(
        borderRadius: BorderRadius.circular(10),
        onTap: () => _abrir(s.id, sem, tonoMostrar),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 12),
          decoration: BoxDecoration(
            border: Border.all(color: NW.line),
            borderRadius: BorderRadius.circular(10),
          ),
          child: Row(
            children: [
              SizedBox(
                width: 28,
                child: Text('${s.id}',
                    textAlign: TextAlign.right,
                    style: const TextStyle(color: NW.txt3, fontFamily: NW.mono, fontSize: 13)),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(s.titulo,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: NW.txt)),
                    if (s.artista.isNotEmpty)
                      Text(s.artista,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(fontSize: 12, color: NW.txt2)),
                  ],
                ),
              ),
              if (tonoMostrar.isNotEmpty)
                Container(
                  margin: const EdgeInsets.only(left: 8),
                  padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                  decoration: BoxDecoration(
                    color: NW.goldSoft,
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Text(tonoMostrar,
                      style: const TextStyle(
                          color: NW.gold, fontWeight: FontWeight.bold, fontSize: 13, fontFamily: NW.mono)),
                ),
              const Icon(Icons.chevron_right, color: NW.txt3),
            ],
          ),
        ),
      ),
    );
  }
}

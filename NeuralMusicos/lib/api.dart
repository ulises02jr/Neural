import 'dart:convert';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';
import 'models.dart';

/// Cliente de la API de Neural Worship. Autentica con email+password,
/// guarda el token de la organizacion y lo usa en las llamadas /api/live/*.
class Api {
  static const String baseUrl = 'https://neuralworship.com';

  String? token;
  String orgNombre = '';
  String nombre = '';
  Map<String, dynamic> features = {};

  static final Api I = Api._();
  Api._();

  Map<String, String> get _authHeaders => {'Authorization': 'Bearer ${token ?? ''}'};

  Future<void> cargarSesion() async {
    final sp = await SharedPreferences.getInstance();
    token = sp.getString('nw_token');
    orgNombre = sp.getString('nw_org') ?? '';
    nombre = sp.getString('nw_nombre') ?? '';
  }

  Future<void> _guardarSesion() async {
    final sp = await SharedPreferences.getInstance();
    if (token != null) await sp.setString('nw_token', token!);
    await sp.setString('nw_org', orgNombre);
    await sp.setString('nw_nombre', nombre);
  }

  Future<void> logout() async {
    final sp = await SharedPreferences.getInstance();
    await sp.remove('nw_token');
    await sp.remove('nw_org');
    await sp.remove('nw_nombre');
    token = null;
    orgNombre = '';
    nombre = '';
    features = {};
  }

  bool get logueado => token != null && token!.isNotEmpty;

  /// Devuelve {ok:bool, mensaje:String?}
  Future<Map<String, dynamic>> login(String email, String password) async {
    try {
      final r = await http
          .post(
            Uri.parse('$baseUrl/api/auth/login'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({'email': email, 'password': password}),
          )
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (r.statusCode == 200 && j['ok'] == true) {
        token = j['token']?.toString();
        orgNombre = (j['org_nombre'] ?? '').toString();
        nombre = (j['nombre'] ?? '').toString();
        features = Map<String, dynamic>.from(j['features'] ?? {});
        await _guardarSesion();
        return {'ok': true};
      }
      return {'ok': false, 'mensaje': (j['mensaje'] ?? 'No se pudo iniciar sesion').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexion con el servidor'};
    }
  }

  /// Trae setlists + indice de canciones en una sola llamada.
  /// Devuelve {ok, songs: List<Song>, setlists: List<Setlist>}
  Future<Map<String, dynamic>> biblioteca() async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/setlists'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (r.statusCode == 403) return {'ok': false, 'error': 'unauthorized'};
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] != true) return {'ok': false};
      final songs = <Song>[];
      final idx = Map<String, dynamic>.from(j['canciones'] ?? {});
      idx.forEach((k, v) => songs.add(Song.fromIndex(k, Map<String, dynamic>.from(v as Map))));
      songs.sort((a, b) => a.titulo.toLowerCase().compareTo(b.titulo.toLowerCase()));
      final setlists = <Setlist>[];
      for (final s in (j['setlists'] as List? ?? [])) {
        setlists.add(Setlist.fromJson(Map<String, dynamic>.from(s as Map)));
      }
      return {'ok': true, 'songs': songs, 'setlists': setlists};
    } catch (e) {
      return {'ok': false, 'error': 'network'};
    }
  }

  /// Chart de una cancion transpuesto [sem] semitonos.
  Future<Chart?> chart(int numero, int sem) async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/chart/$numero?t=$sem'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] != true) return null;
      return Chart.fromJson(j);
    } catch (e) {
      return null;
    }
  }

  /// URL de la portada de una cancion (si tiene).
  String portadaUrl(String portada) {
    if (portada.isEmpty) return '';
    if (portada.startsWith('http')) return portada;
    return '$baseUrl/static/portadas/$portada';
  }
}

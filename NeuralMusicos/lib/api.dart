import 'dart:convert';
import 'package:http/http.dart' as http;
import 'models.dart';
import 'app_channel.dart';

/// Cliente de la API de Neural Worship. Autentica con email+password,
/// guarda el token de la organizacion y lo usa en las llamadas /api/live/*.
class Api {
  static const String baseUrl = 'https://neuralworship.com';

  String? token;
  String orgNombre = '';
  String nombre = '';
  String apellido = '';
  Map<String, dynamic> features = {};

  String get nombreCompleto => [nombre, apellido].where((s) => s.isNotEmpty).join(' ');

  static final Api I = Api._();
  Api._();

  Map<String, String> get _authHeaders => {'Authorization': 'Bearer ${token ?? ''}'};

  // Sesion persistente via el archivo de preferencias nativo (neural/app).
  Future<void> cargarSesion() async {
    token = AppChannel.I.get('token');
    orgNombre = (AppChannel.I.get('org', '') ?? '').toString();
    nombre = (AppChannel.I.get('nombre', '') ?? '').toString();
    apellido = (AppChannel.I.get('apellido', '') ?? '').toString();
    final f = AppChannel.I.get('features');
    if (f is Map) features = Map<String, dynamic>.from(f);
  }

  Future<void> _guardarSesion() async {
    await AppChannel.I.setAll({
      'token': token,
      'org': orgNombre,
      'nombre': nombre,
      'apellido': apellido,
      'features': features,
    });
  }

  Future<void> logout() async {
    token = null;
    orgNombre = '';
    nombre = '';
    apellido = '';
    features = {};
    await AppChannel.I.remove(['token', 'org', 'nombre', 'apellido', 'features']);
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
        apellido = (j['apellido'] ?? '').toString();
        features = Map<String, dynamic>.from(j['features'] ?? {});
        await _guardarSesion();
        return {'ok': true};
      }
      return {'ok': false, 'mensaje': (j['mensaje'] ?? 'No se pudo iniciar sesion').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexion con el servidor'};
    }
  }

  /// Registro de musico: se une a una organizacion con su codigo. Queda pendiente.
  /// Devuelve {ok:bool, mensaje:String}
  Future<Map<String, dynamic>> unirse({
    required String codigo,
    required String nombre,
    required String apellido,
    required String email,
    required String password,
  }) async {
    try {
      final r = await http
          .post(
            Uri.parse('$baseUrl/api/auth/unirse'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({
              'codigo': codigo,
              'nombre': nombre,
              'apellido': apellido,
              'email': email,
              'password': password,
            }),
          )
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return {'ok': j['ok'] == true, 'mensaje': (j['mensaje'] ?? '').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexión con el servidor'};
    }
  }

  /// Solicita un código de reset al correo. Respuesta genérica.
  Future<Map<String, dynamic>> olvide(String email) async {
    try {
      final r = await http
          .post(Uri.parse('$baseUrl/api/auth/olvide'),
              headers: {'Content-Type': 'application/json'},
              body: jsonEncode({'email': email}))
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return {'ok': j['ok'] == true, 'mensaje': (j['mensaje'] ?? '').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexión con el servidor'};
    }
  }

  /// Canjea código + nueva contraseña.
  Future<Map<String, dynamic>> reset(String email, String codigo, String password) async {
    try {
      final r = await http
          .post(Uri.parse('$baseUrl/api/auth/reset'),
              headers: {'Content-Type': 'application/json'},
              body: jsonEncode({'email': email, 'codigo': codigo, 'password': password}))
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return {'ok': j['ok'] == true, 'mensaje': (j['mensaje'] ?? '').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexión con el servidor'};
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
      songs.sort((a, b) => a.id.compareTo(b.id)); // por numero de pista
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

  /// Sistema en vivo por LAN (NeuralPlay :5050): chart actual. base = http://ip:5050
  Future<Chart?> liveSong(String base) async {
    try {
      final r = await http.get(Uri.parse('$base/song')).timeout(const Duration(seconds: 6));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      final secs = j['secciones'];
      if (secs is! List || secs.isEmpty) return null;
      return Chart.fromJson(j);
    } catch (e) {
      return null;
    }
  }

  /// Estado del sistema en vivo: {idx, ver, playing}. null si no responde (offline).
  Future<Map<String, dynamic>?> liveState(String base) async {
    try {
      final r = await http.get(Uri.parse('$base/state')).timeout(const Duration(seconds: 6));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return {
        'idx': (j['idx'] is num) ? (j['idx'] as num).toInt() : 0,
        'ver': (j['ver'] is num) ? (j['ver'] as num).toInt() : 0,
        'playing': j['playing'] == true,
      };
    } catch (e) {
      return null;
    }
  }

  /// Estado del culto en vivo: {activo:bool, ip:String?}.
  Future<Map<String, dynamic>> liveStatus() async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live_status'))
          .timeout(const Duration(seconds: 10));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return {'activo': j['activo'] == true, 'ip': j['ip']?.toString()};
    } catch (e) {
      return {'activo': false, 'ip': null};
    }
  }

  /// Pistas (stems) + secciones de una cancion a un tono, para el modo ensayo.
  /// Devuelve el JSON crudo: {listo, hay_pistas, tempo, compas, stems:[...], secciones:[...]}
  Future<Map<String, dynamic>?> pistas(int numero, int sem) async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/pistas/$numero?t=$sem'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (r.statusCode != 200) return null;
      return jsonDecode(r.body) as Map<String, dynamic>;
    } catch (e) {
      return null;
    }
  }

  /// URL de descarga de un stem para el motor de audio nativo.
  /// El token va en el query (no como header) para que el redirect 302 a Spaces
  /// no arrastre el header Authorization (Spaces lo rechaza).
  String stemDownloadUrl(int numero, String file, int sem) {
    final u = Uri.parse(baseUrl).replace(
      pathSegments: ['api', 'live', 'pista', '$numero', ...file.split('/')],
      queryParameters: {'t': '$sem', if (token != null) 'token': token!},
    );
    return u.toString();
  }

  /// URL de la portada de una cancion. La web la sirve como /static/<portada>.
  /// [ts] es portada_ts para cache-busting. Si no hay portada, devuelve el logo.
  String portadaUrl(String portada, {int? ts}) {
    if (portada.isEmpty) return '$baseUrl/static/logo.png';
    if (portada.startsWith('http')) return portada;
    return '$baseUrl/static/$portada?v=${ts ?? 0}';
  }
}

import 'dart:convert';
import 'dart:io' show Platform;
import 'package:http/http.dart' as http;
import 'models.dart';
import 'app_channel.dart';

/// Cliente de la API de Neural Worship. Autentica con email+password,
/// guarda el token de la organizacion y lo usa en las llamadas /api/live/*.
class Api {
  static const String baseUrl = 'https://neuralworship.com';

  String? token;
  String? session; // token de sesión única por dispositivo
  String orgNombre = '';
  String nombre = '';
  String apellido = '';
  Map<String, dynamic> features = {};

  /// Se dispara cuando el servidor cierra esta sesión porque se inició en otro
  /// dispositivo. La UI lo usa para volver al login y avisar al usuario.
  void Function(String mensaje)? onKicked;
  bool _kicked = false;

  String get nombreCompleto => [nombre, apellido].where((s) => s.isNotEmpty).join(' ');

  static final Api I = Api._();
  Api._();

  Map<String, String> get _authHeaders => {
        'Authorization': 'Bearer ${token ?? ''}',
        if (session != null && session!.isNotEmpty) 'X-Session-Token': session!,
      };

  /// Revisa si el servidor expulsó esta sesión (401 sesion_reemplazada).
  /// Devuelve true si fue expulsada; en ese caso cierra sesión y avisa a la UI.
  bool _revisarExpulsion(http.Response r) {
    if (r.statusCode != 401) return false;
    try {
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['error'] == 'sesion_reemplazada') {
        final msg = (j['mensaje'] ?? 'Se inició sesión en otro dispositivo.').toString();
        if (!_kicked) {
          _kicked = true;
          logout();
          onKicked?.call(msg);
        }
        return true;
      }
    } catch (_) {}
    return false;
  }

  /// True si hay una sesión guardada de una versión anterior (token pero sin
  /// session_token). Hay que forzar re-login para activar la sesión única.
  bool get sesionVieja =>
      token != null && token!.isNotEmpty && (session == null || session!.isEmpty);

  /// ¿Hay internet? Ping liviano al servidor (cualquier respuesta = en línea).
  Future<bool> online() async {
    try {
      await http
          .get(Uri.parse('$baseUrl/api/auth/ping'))
          .timeout(const Duration(seconds: 6));
      return true;
    } catch (_) {
      return false;
    }
  }

  /// Marca (o consulta) si una canción quedó descargada para uso offline.
  Future<void> marcarDescargada(int numero) => AppChannel.I.set('dl_$numero', true);
  bool estaDescargada(int numero) => AppChannel.I.get('dl_$numero') == true;

  /// Al borrar el caché de audio, quitar también los indicadores de descarga.
  Future<void> limpiarDescargas() => AppChannel.I.removePrefix('dl_');

  /// Latido: confirma contra el servidor que esta sesión siga siendo la activa.
  /// Si fue reemplazada por otro dispositivo, dispara la expulsión (logout+aviso).
  Future<void> pingSesion() async {
    if (session == null || session!.isEmpty) return;
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/auth/ping'), headers: _authHeaders)
          .timeout(const Duration(seconds: 10));
      _revisarExpulsion(r);
    } catch (_) {}
  }

  // Sesion persistente via el archivo de preferencias nativo (neural/app).
  Future<void> cargarSesion() async {
    token = AppChannel.I.get('token');
    session = AppChannel.I.get('session');
    orgNombre = (AppChannel.I.get('org', '') ?? '').toString();
    nombre = (AppChannel.I.get('nombre', '') ?? '').toString();
    apellido = (AppChannel.I.get('apellido', '') ?? '').toString();
    final f = AppChannel.I.get('features');
    if (f is Map) features = Map<String, dynamic>.from(f);
  }

  Future<void> _guardarSesion() async {
    await AppChannel.I.setAll({
      'token': token,
      'session': session,
      'org': orgNombre,
      'nombre': nombre,
      'apellido': apellido,
      'features': features,
    });
  }

  Future<void> logout() async {
    // Avisar al servidor para cerrar la sesión (best-effort).
    final s = session;
    if (s != null && s.isNotEmpty) {
      try {
        await http.post(Uri.parse('$baseUrl/api/auth/logout'),
            headers: {'X-Session-Token': s}).timeout(const Duration(seconds: 6));
      } catch (_) {}
    }
    token = null;
    session = null;
    orgNombre = '';
    nombre = '';
    apellido = '';
    features = {};
    await AppChannel.I.remove(['token', 'session', 'org', 'nombre', 'apellido', 'features']);
  }

  /// Elimina definitivamente la cuenta del músico (requisito de App Store).
  /// Re-confirma con la contraseña. Devuelve {ok:bool, mensaje:String?}.
  Future<Map<String, dynamic>> eliminarCuenta(String password) async {
    final s = session;
    if (s == null || s.isEmpty) {
      return {'ok': false, 'mensaje': 'Iniciá sesión de nuevo.'};
    }
    try {
      final r = await http
          .post(Uri.parse('$baseUrl/api/auth/eliminar_cuenta'),
              headers: {'X-Session-Token': s, 'Content-Type': 'application/json'},
              body: jsonEncode({'password': password}))
          .timeout(const Duration(seconds: 12));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] == true) {
        // Limpiar todo el estado local tras borrar la cuenta en el servidor.
        token = null;
        session = null;
        orgNombre = '';
        nombre = '';
        apellido = '';
        features = {};
        await AppChannel.I.remove(['token', 'session', 'org', 'nombre', 'apellido', 'features']);
        return {'ok': true};
      }
      return {'ok': false, 'mensaje': (j['mensaje'] ?? 'No se pudo eliminar la cuenta.').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexión. Intentá de nuevo.'};
    }
  }

  /// Versión de la app (se actualiza al subir una versión nueva).
  static const String appVersion = '0.1.0';

  /// Envía un reporte de problema del usuario a /api/reporte.
  /// Devuelve {ok:bool, mensaje:String?}.
  Future<Map<String, dynamic>> reportarProblema(String mensaje) async {
    final msg = mensaje.trim();
    if (msg.isEmpty) return {'ok': false, 'mensaje': 'Escribí el problema.'};
    String device = '';
    try {
      device = '${Platform.operatingSystem} ${Platform.operatingSystemVersion}';
    } catch (_) {}
    try {
      final r = await http
          .post(Uri.parse('$baseUrl/api/reporte'),
              headers: {
                'Content-Type': 'application/json',
                if (session != null && session!.isNotEmpty) 'X-Session-Token': session!,
              },
              body: jsonEncode({
                'mensaje': msg,
                'app': 'neuralcharts',
                'version': appVersion,
                'device': device,
                if (session != null && session!.isNotEmpty) 'session': session,
              }))
          .timeout(const Duration(seconds: 12));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] == true) return {'ok': true};
      return {'ok': false, 'mensaje': (j['mensaje'] ?? 'No se pudo enviar.').toString()};
    } catch (e) {
      return {'ok': false, 'mensaje': 'Sin conexión. Intentá de nuevo.'};
    }
  }

  bool get logueado => token != null && token!.isNotEmpty;

  /// Devuelve {ok:bool, mensaje:String?}
  Future<Map<String, dynamic>> login(String email, String password) async {
    try {
      final r = await http
          .post(
            Uri.parse('$baseUrl/api/auth/login'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({'email': email, 'password': password, 'app': 'neuralcharts'}),
          )
          .timeout(const Duration(seconds: 20));
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (r.statusCode == 200 && j['ok'] == true) {
        token = j['token']?.toString();
        session = j['session']?.toString();
        orgNombre = (j['org_nombre'] ?? '').toString();
        nombre = (j['nombre'] ?? '').toString();
        apellido = (j['apellido'] ?? '').toString();
        features = Map<String, dynamic>.from(j['features'] ?? {});
        _kicked = false;
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
  Map<String, dynamic> _parseBiblioteca(Map<String, dynamic> j) {
    final songs = <Song>[];
    final idx = Map<String, dynamic>.from(j['canciones'] ?? {});
    idx.forEach((k, v) => songs.add(Song.fromIndex(k, Map<String, dynamic>.from(v as Map))));
    songs.sort((a, b) => a.id.compareTo(b.id)); // por numero de pista
    final setlists = <Setlist>[];
    for (final s in (j['setlists'] as List? ?? [])) {
      setlists.add(Setlist.fromJson(Map<String, dynamic>.from(s as Map)));
    }
    return {'ok': true, 'songs': songs, 'setlists': setlists};
  }

  Future<Map<String, dynamic>> biblioteca() async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/setlists'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (_revisarExpulsion(r)) return {'ok': false, 'error': 'kicked'};
      if (r.statusCode == 403) return {'ok': false, 'error': 'unauthorized'};
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] != true) return {'ok': false};
      // Guardar en caché para poder mostrarla sin internet.
      await AppChannel.I.set('bib_cache', r.body);
      return _parseBiblioteca(j);
    } catch (e) {
      // Sin conexión: usar la última biblioteca guardada.
      final cached = AppChannel.I.get('bib_cache');
      if (cached is String && cached.isNotEmpty) {
        try {
          final j = jsonDecode(cached) as Map<String, dynamic>;
          final res = _parseBiblioteca(j);
          res['offline'] = true;
          return res;
        } catch (_) {}
      }
      return {'ok': false, 'error': 'network'};
    }
  }

  /// Chart de una cancion transpuesto [sem] semitonos.
  Future<Chart?> chart(int numero, int sem) async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/chart/$numero?t=$sem'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (_revisarExpulsion(r)) return null;
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['ok'] != true) return null;
      await AppChannel.I.set('chart_${numero}_$sem', r.body); // caché offline
      return Chart.fromJson(j);
    } catch (e) {
      // Sin conexión: usar el chart guardado si existe.
      final cached = AppChannel.I.get('chart_${numero}_$sem');
      if (cached is String && cached.isNotEmpty) {
        try {
          return Chart.fromJson(jsonDecode(cached) as Map<String, dynamic>);
        } catch (_) {}
      }
      return null;
    }
  }

  /// Arreglo activo de una canción (el que el operador dejó marcado en NeuralPlay).
  /// Devuelve la secuencia de índices de sección reales (con repeticiones), o [] si no hay.
  /// Se cachea para que el repertorio respete el arreglo también sin conexión.
  Future<List<int>> songArrangement(int numero) async {
    List<int> parse(String body) {
      final j = jsonDecode(body) as Map<String, dynamic>;
      if (j['ok'] != true || j['arreglos'] is! List) return const [];
      for (final e in (j['arreglos'] as List)) {
        if (e is Map &&
            (e['id'] is num) &&
            (e['id'] as num).toInt() == numero &&
            e['last'] == true &&
            e['seq'] is List) {
          return [for (final x in (e['seq'] as List)) if (x is num) x.toInt()];
        }
      }
      return const [];
    }

    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/arreglos'), headers: _authHeaders)
          .timeout(const Duration(seconds: 12));
      if (_revisarExpulsion(r)) return const [];
      await AppChannel.I.set('arreglos_all', r.body); // caché offline (toda la org)
      return parse(r.body);
    } catch (e) {
      final cached = AppChannel.I.get('arreglos_all');
      if (cached is String && cached.isNotEmpty) {
        try {
          return parse(cached);
        } catch (_) {}
      }
      return const [];
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
      final arr = <int>[];
      if (j['arr'] is List) {
        for (final v in (j['arr'] as List)) {
          if (v is num) arr.add(v.toInt());
        }
      }
      return {
        'idx': (j['idx'] is num) ? (j['idx'] as num).toInt() : 0,
        'ver': (j['ver'] is num) ? (j['ver'] as num).toInt() : 0,
        'step': (j['step'] is num) ? (j['step'] as num).toInt() : -1,
        'arr': arr,
        'playing': j['playing'] == true,
      };
    } catch (e) {
      return null;
    }
  }

  /// Estado del culto en vivo: {activo:bool, ip:String?}.
  Future<Map<String, dynamic>> liveStatus() async {
    try {
      // Multi-tenant: mandamos el token para que el servidor devuelva el estado
      // del live de NUESTRA organización (sin token caería a la organización 1).
      final u = Uri.parse('$baseUrl/api/live_status')
          .replace(queryParameters: {'token': token ?? ''});
      final r = await http.get(u).timeout(const Duration(seconds: 10));
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
      if (_revisarExpulsion(r)) return null;
      if (r.statusCode != 200) return null;
      return jsonDecode(r.body) as Map<String, dynamic>;
    } catch (e) {
      return null;
    }
  }

  /// Pide al servidor que GENERE (renderice) un tono que todavia no esta listo.
  /// Devuelve true si ya quedo listo o si el render arranco/esta en proceso.
  Future<bool> pedirRenderTono(int numero, int sem) async {
    try {
      final r = await http
          .post(Uri.parse('$baseUrl/api/live/render/$numero/$sem'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (_revisarExpulsion(r)) return false;
      if (r.statusCode != 200) return false;
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      return j['listo'] == true || j['estado'] == 'procesando';
    } catch (e) {
      return false;
    }
  }

  /// Progreso del render de un tono: {listo:bool, progreso:"x/y"}.
  Future<Map<String, dynamic>?> estadoRenderTono(int numero, int sem) async {
    try {
      final r = await http
          .get(Uri.parse('$baseUrl/api/live/render/$numero/$sem/estado'), headers: _authHeaders)
          .timeout(const Duration(seconds: 20));
      if (_revisarExpulsion(r)) return null;
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

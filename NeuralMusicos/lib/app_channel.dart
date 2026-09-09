import 'dart:convert';
import 'package:flutter/services.dart';

/// Canal general: abrir URLs (EN VIVO) y guardar preferencias/sesion en un
/// archivo de la app (sin depender de plugins/CocoaPods).
class AppChannel {
  static const _ch = MethodChannel('neural/app');
  static final AppChannel I = AppChannel._();
  AppChannel._();

  Map<String, dynamic> _cache = {};
  bool _loaded = false;

  Future<void> load() async {
    try {
      final s = await _ch.invokeMethod('prefsGet');
      if (s is String && s.isNotEmpty) {
        _cache = (jsonDecode(s) as Map).cast<String, dynamic>();
      }
    } catch (_) {
      _cache = {};
    }
    _loaded = true;
  }

  Future<void> _save() async {
    try {
      await _ch.invokeMethod('prefsSet', {'json': jsonEncode(_cache)});
    } catch (_) {}
  }

  dynamic get(String k, [dynamic def]) => _loaded ? (_cache[k] ?? def) : def;

  Future<void> set(String k, dynamic v) async {
    _cache[k] = v;
    await _save();
  }

  Future<void> setAll(Map<String, dynamic> kv) async {
    _cache.addAll(kv);
    await _save();
  }

  Future<void> remove(List<String> keys) async {
    for (final k in keys) {
      _cache.remove(k);
    }
    await _save();
  }

  Future<void> openUrl(String url) async {
    try {
      await _ch.invokeMethod('openUrl', {'url': url});
    } catch (_) {}
  }
}

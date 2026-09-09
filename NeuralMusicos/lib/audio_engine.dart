import 'package:flutter/services.dart';

/// Puente al motor de audio multipista nativo (AVAudioEngine en macOS/iOS).
/// El canal se implementa en el Runner de cada plataforma (neural/audio).
class AudioEngine {
  static const _ch = MethodChannel('neural/audio');
  static final AudioEngine I = AudioEngine._();
  AudioEngine._();

  // Sesion cargada actualmente (para no re-descargar al reabrir el panel).
  int? loadedNumero;
  int? loadedSem;
  double loadedDur = 0;

  bool loadedFor(int numero, int sem) => loadedNumero == numero && loadedSem == sem;

  /// Carga los stems: cada uno {id, url}. Devuelve la duracion en segundos.
  Future<double> load(List<Map<String, String>> stems, {int? numero, int? sem}) async {
    final d = await _ch.invokeMethod('load', {'stems': stems});
    loadedDur = (d as num?)?.toDouble() ?? 0;
    loadedNumero = numero;
    loadedSem = sem;
    return loadedDur;
  }

  Future<void> play(double offset) => _ch.invokeMethod('play', {'offset': offset});
  Future<void> pause() => _ch.invokeMethod('pause');
  Future<void> seek(double offset) => _ch.invokeMethod('seek', {'offset': offset});
  Future<void> setVolume(String id, double value) =>
      _ch.invokeMethod('setVolume', {'id': id, 'value': value});

  Future<void> stop() async {
    await _ch.invokeMethod('stop');
    loadedNumero = null;
    loadedSem = null;
    loadedDur = 0;
  }

  Future<double> position() async {
    final p = await _ch.invokeMethod('position');
    return (p as num?)?.toDouble() ?? 0;
  }

  Future<bool> isPlaying() async {
    final b = await _ch.invokeMethod('isPlaying');
    return b == true;
  }

  /// Bytes ocupados por las pistas guardadas offline.
  Future<int> cacheSize() async {
    final n = await _ch.invokeMethod('cacheSize');
    return (n as num?)?.toInt() ?? 0;
  }

  /// Borra las pistas guardadas para liberar espacio.
  Future<void> clearCache() => _ch.invokeMethod('clearCache');
}

import 'package:flutter/services.dart';

/// Puente al motor de audio multipista nativo (AVAudioEngine en macOS/iOS).
/// El canal se implementa en el Runner de cada plataforma (neural/audio).
class AudioEngine {
  static const _ch = MethodChannel('neural/audio');
  static final AudioEngine I = AudioEngine._();
  AudioEngine._();

  /// Carga los stems: cada uno {id, url}. Devuelve la duracion en segundos.
  Future<double> load(List<Map<String, String>> stems) async {
    final d = await _ch.invokeMethod('load', {'stems': stems});
    return (d as num?)?.toDouble() ?? 0;
  }

  Future<void> play(double offset) => _ch.invokeMethod('play', {'offset': offset});
  Future<void> pause() => _ch.invokeMethod('pause');
  Future<void> seek(double offset) => _ch.invokeMethod('seek', {'offset': offset});
  Future<void> setVolume(String id, double value) =>
      _ch.invokeMethod('setVolume', {'id': id, 'value': value});
  Future<void> stop() => _ch.invokeMethod('stop');

  Future<double> position() async {
    final p = await _ch.invokeMethod('position');
    return (p as num?)?.toDouble() ?? 0;
  }
}

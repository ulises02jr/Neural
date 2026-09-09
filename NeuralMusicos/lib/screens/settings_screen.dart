import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';
import '../audio_engine.dart';
import 'login_screen.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  int _cacheBytes = 0;
  bool _cargando = true;

  @override
  void initState() {
    super.initState();
    _cargarCache();
  }

  Future<void> _cargarCache() async {
    setState(() => _cargando = true);
    int b = 0;
    try {
      b = await AudioEngine.I.cacheSize();
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _cacheBytes = b;
      _cargando = false;
    });
  }

  String _fmt(int bytes) {
    if (bytes <= 0) return '0 MB';
    final mb = bytes / (1024 * 1024);
    if (mb < 1) return '${(bytes / 1024).round()} KB';
    if (mb >= 1024) return '${(mb / 1024).toStringAsFixed(2)} GB';
    return '${mb.toStringAsFixed(1)} MB';
  }

  Future<void> _liberar() async {
    try {
      await AudioEngine.I.clearCache();
    } catch (_) {}
    await _cargarCache();
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Espacio liberado')),
      );
    }
  }

  Future<void> _salir() async {
    await Api.I.logout();
    if (!mounted) return;
    Navigator.of(context).pushAndRemoveUntil(
      MaterialPageRoute(builder: (_) => const LoginScreen()),
      (r) => false,
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Configuración', style: TextStyle(fontSize: 17))),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _seccion('CUENTA'),
          _tarjeta(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _fila('Organización', Api.I.orgNombre.isEmpty ? '—' : Api.I.orgNombre),
                const Divider(color: NW.line, height: 20),
                _fila('Usuario', Api.I.nombre.isEmpty ? '—' : Api.I.nombre),
              ],
            ),
          ),
          const SizedBox(height: 20),
          _seccion('ALMACENAMIENTO OFFLINE'),
          _tarjeta(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    const Icon(Icons.download_done, size: 18, color: NW.gold),
                    const SizedBox(width: 10),
                    const Expanded(
                      child: Text('Pistas guardadas para ensayar sin internet',
                          style: TextStyle(fontSize: 13, color: NW.txt)),
                    ),
                    Text(_cargando ? '…' : _fmt(_cacheBytes),
                        style: const TextStyle(
                            fontSize: 14, fontWeight: FontWeight.bold, color: NW.txt, fontFamily: NW.mono)),
                  ],
                ),
                const SizedBox(height: 14),
                SizedBox(
                  width: double.infinity,
                  child: OutlinedButton.icon(
                    onPressed: _cacheBytes > 0 ? _liberar : null,
                    icon: const Icon(Icons.delete_outline, size: 18),
                    label: const Text('Liberar espacio'),
                    style: OutlinedButton.styleFrom(
                      foregroundColor: NW.txt,
                      side: const BorderSide(color: NW.line),
                      padding: const EdgeInsets.symmetric(vertical: 12),
                    ),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 30),
          SizedBox(
            width: double.infinity,
            child: TextButton.icon(
              onPressed: _salir,
              icon: const Icon(Icons.logout, size: 18, color: NW.error),
              label: const Text('Cerrar sesión', style: TextStyle(color: NW.error)),
            ),
          ),
          const SizedBox(height: 20),
          const Center(
            child: Text('Neural Worship · Músicos',
                style: TextStyle(fontSize: 11, color: NW.txt3)),
          ),
        ],
      ),
    );
  }

  Widget _seccion(String t) => Padding(
        padding: const EdgeInsets.only(left: 4, bottom: 8),
        child: Text(t,
            style: const TextStyle(
                fontSize: 11, color: NW.txt2, fontWeight: FontWeight.w600, letterSpacing: 1.2)),
      );

  Widget _tarjeta({required Widget child}) => Container(
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: NW.surface,
          border: Border.all(color: NW.line),
          borderRadius: BorderRadius.circular(12),
        ),
        child: child,
      );

  Widget _fila(String k, String v) => Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(k, style: const TextStyle(fontSize: 13, color: NW.txt2)),
          Flexible(
            child: Text(v,
                textAlign: TextAlign.right,
                style: const TextStyle(fontSize: 13, color: NW.txt, fontWeight: FontWeight.w500)),
          ),
        ],
      );
}

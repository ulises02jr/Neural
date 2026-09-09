import 'package:flutter/material.dart';
import '../theme.dart';
import '../api.dart';

class ForgotScreen extends StatefulWidget {
  const ForgotScreen({super.key});

  @override
  State<ForgotScreen> createState() => _ForgotScreenState();
}

class _ForgotScreenState extends State<ForgotScreen> {
  final _email = TextEditingController();
  final _codigo = TextEditingController();
  final _pass = TextEditingController();
  final _pass2 = TextEditingController();
  bool _paso2 = false; // false = pedir email; true = codigo + nueva
  bool _cargando = false;
  String? _error;
  String? _info;

  Future<void> _enviarCodigo() async {
    final email = _email.text.trim();
    if (email.isEmpty) {
      setState(() => _error = 'Ingresá tu email');
      return;
    }
    setState(() { _cargando = true; _error = null; _info = null; });
    final r = await Api.I.olvide(email);
    if (!mounted) return;
    setState(() {
      _cargando = false;
      if (r['ok'] == true) {
        _paso2 = true;
        _info = (r['mensaje'] ?? '').toString();
      } else {
        _error = (r['mensaje'] ?? 'No se pudo enviar el código').toString();
      }
    });
  }

  Future<void> _cambiar() async {
    final codigo = _codigo.text.trim();
    final p = _pass.text;
    final p2 = _pass2.text;
    if (codigo.isEmpty || p.isEmpty) {
      setState(() => _error = 'Completá el código y la contraseña');
      return;
    }
    if (p != p2) {
      setState(() => _error = 'Las contraseñas no coinciden');
      return;
    }
    setState(() { _cargando = true; _error = null; });
    final r = await Api.I.reset(_email.text.trim(), codigo, p);
    if (!mounted) return;
    if (r['ok'] == true) {
      Navigator.of(context).pop();
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text(r['mensaje']?.toString() ?? 'Contraseña cambiada. Ya podés ingresar.'),
        duration: const Duration(seconds: 5),
      ));
    } else {
      setState(() {
        _cargando = false;
        _error = (r['mensaje'] ?? 'No se pudo cambiar la contraseña').toString();
      });
    }
  }

  Widget _label(String t) => Padding(
        padding: const EdgeInsets.only(bottom: 6, top: 12),
        child: Text(t,
            style: const TextStyle(
                fontSize: 11, color: NW.txt2, fontWeight: FontWeight.w500, letterSpacing: 1.2)),
      );

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Recuperar contraseña', style: TextStyle(fontSize: 17))),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(20),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 400),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Text(_paso2 ? 'Ingresá el código' : 'Recuperar acceso',
                    style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
                const SizedBox(height: 4),
                Text(
                  _paso2
                      ? 'Te enviamos un código a tu correo. Escribilo y elegí tu nueva contraseña.'
                      : 'Escribí tu email y te enviaremos un código de verificación.',
                  style: const TextStyle(fontSize: 12, color: NW.txt2, height: 1.4),
                ),
                if (_info != null) ...[
                  const SizedBox(height: 12),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
                    decoration: BoxDecoration(
                      color: const Color(0x1A86B36A),
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(color: const Color(0x4D86B36A)),
                    ),
                    child: Text(_info!,
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: NW.success, fontSize: 13)),
                  ),
                ],
                if (_error != null) ...[
                  const SizedBox(height: 12),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
                    decoration: BoxDecoration(
                      color: const Color(0x1FDC6262),
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(color: const Color(0x40DC6262)),
                    ),
                    child: Text(_error!,
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: NW.error, fontSize: 13)),
                  ),
                ],
                _label('EMAIL'),
                TextField(
                  controller: _email,
                  enabled: !_paso2,
                  keyboardType: TextInputType.emailAddress,
                  autocorrect: false,
                ),
                if (_paso2) ...[
                  _label('CÓDIGO DE VERIFICACIÓN'),
                  TextField(controller: _codigo, autocorrect: false),
                  _label('NUEVA CONTRASEÑA'),
                  TextField(controller: _pass, obscureText: true),
                  _label('REPETIR CONTRASEÑA'),
                  TextField(controller: _pass2, obscureText: true, onSubmitted: (_) => _cambiar()),
                ],
                const SizedBox(height: 22),
                FilledButton(
                  style: FilledButton.styleFrom(
                    backgroundColor: Colors.white,
                    foregroundColor: Colors.black,
                    padding: const EdgeInsets.symmetric(vertical: 14),
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                  ),
                  onPressed: _cargando ? null : (_paso2 ? _cambiar : _enviarCodigo),
                  child: _cargando
                      ? const SizedBox(
                          height: 20, width: 20,
                          child: CircularProgressIndicator(strokeWidth: 2, color: Colors.black))
                      : Text(_paso2 ? 'Cambiar contraseña' : 'Enviar código',
                          style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 14)),
                ),
                if (_paso2)
                  TextButton(
                    onPressed: _cargando ? null : _enviarCodigo,
                    child: const Text('Reenviar código',
                        style: TextStyle(color: NW.chord, fontSize: 12)),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
